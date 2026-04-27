#include "utils/signal_cancellation.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace agentos {

namespace {

// Module-level state. The token outlives the process; capturing handlers
// reference these via the function-local static returned by GetState().
struct SignalState {
    std::shared_ptr<CancellationToken> token = std::make_shared<CancellationToken>();
    std::atomic<bool> handler_installed{false};
    std::atomic<int> signal_count{0};
};

SignalState& GetState() {
    static SignalState state;
    return state;
}

void NoteFirstSignal() {
    // async-signal-safe: write(2) on POSIX is signal-safe; on Windows the
    // console-control handler runs in a thread, so std::fputs is fine.
    static const char kMessage[] =
        "\nagentos: cancellation requested; press Ctrl-C again to force exit.\n";
#ifdef _WIN32
    std::fputs(kMessage, stderr);
    std::fflush(stderr);
#else
    // write(2) is the only signal-safe way to reach stderr; fputs is not.
    const auto written = ::write(2, kMessage, sizeof(kMessage) - 1);
    (void)written;
#endif
}

#ifdef _WIN32

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type != CTRL_C_EVENT && ctrl_type != CTRL_BREAK_EVENT &&
        ctrl_type != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
    auto& state = GetState();
    const int previous = state.signal_count.fetch_add(1);
    state.token->cancel();
    if (previous == 0) {
        NoteFirstSignal();
        // Returning TRUE swallows this Ctrl-C so the process keeps running
        // and observes the cooperative cancel. The next Ctrl-C falls through
        // to the default handler (terminates the process).
        return TRUE;
    }
    // Second-and-later: let Windows terminate.
    return FALSE;
}

#else

extern "C" void PosixSignalHandler(int signum) {
    auto& state = GetState();
    const int previous = state.signal_count.fetch_add(1);
    state.token->cancel();
    if (previous == 0) {
        NoteFirstSignal();
        return;
    }
    // Second signal: restore default disposition and re-raise so the user can
    // forcibly kill if cooperative cancel is too slow.
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(signum, &default_action, nullptr);
    raise(signum);
}

#endif

}  // namespace

std::shared_ptr<CancellationToken> InstallSignalCancellation() {
    auto& state = GetState();
    bool expected = false;
    if (!state.handler_installed.compare_exchange_strong(expected, true)) {
        return state.token;
    }

#ifdef _WIN32
    if (!::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE)) {
        // Couldn't install — best effort. Reset the flag so a later call can
        // retry, but still return the token so callers get a usable object.
        state.handler_installed.store(false);
    }
#else
    struct sigaction action{};
    action.sa_handler = &PosixSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif

    return state.token;
}

}  // namespace agentos
