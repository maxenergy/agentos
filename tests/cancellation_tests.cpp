#include "utils/cancellation.hpp"
#include "utils/signal_cancellation.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestCancellationTokenStartsUntripped() {
    agentos::CancellationToken token;
    Expect(!token.is_cancelled(),
           "TestCancellationTokenStartsUntripped: fresh token should not be cancelled");
}

void TestCancellationTokenIsIdempotent() {
    agentos::CancellationToken token;
    token.cancel();
    Expect(token.is_cancelled(),
           "TestCancellationTokenIsIdempotent: token should be cancelled after first cancel()");
    // Second cancel must not deadlock and must leave the token cancelled.
    token.cancel();
    Expect(token.is_cancelled(),
           "TestCancellationTokenIsIdempotent: token should remain cancelled after second cancel()");
}

void TestWaitForCancelTimesOutWhenNotTripped() {
    agentos::CancellationToken token;
    const auto start = std::chrono::steady_clock::now();
    const bool tripped = token.wait_for_cancel(std::chrono::milliseconds(50));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    Expect(!tripped,
           "TestWaitForCancelTimesOutWhenNotTripped: wait_for_cancel should return false on timeout");
    // Allow generous slack for Windows scheduler jitter; require at least ~45ms.
    Expect(elapsed_ms >= 45,
           "TestWaitForCancelTimesOutWhenNotTripped: elapsed should be at least ~45ms (got "
               + std::to_string(elapsed_ms) + "ms)");
}

void TestWaitForCancelReturnsImmediatelyWhenAlreadyCancelled() {
    agentos::CancellationToken token;
    token.cancel();
    const auto start = std::chrono::steady_clock::now();
    const bool tripped = token.wait_for_cancel(std::chrono::milliseconds(5000));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    Expect(tripped,
           "TestWaitForCancelReturnsImmediatelyWhenAlreadyCancelled: should return true for "
           "already-cancelled token");
    Expect(elapsed_ms < 100,
           "TestWaitForCancelReturnsImmediatelyWhenAlreadyCancelled: elapsed should be well "
           "under 100ms (got "
               + std::to_string(elapsed_ms) + "ms)");
}

void TestWaitForCancelReleasesAllWaiters() {
    agentos::CancellationToken token;
    constexpr int kWaiters = 4;
    std::vector<std::thread> threads;
    threads.reserve(kWaiters);
    std::atomic<int> tripped_count{0};

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&token, &tripped_count]() {
            if (token.wait_for_cancel(std::chrono::milliseconds(2000))) {
                tripped_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Give all waiters a chance to enter wait_for_cancel before cancelling.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    token.cancel();

    for (auto& t : threads) {
        t.join();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    Expect(tripped_count.load() == kWaiters,
           "TestWaitForCancelReleasesAllWaiters: all waiters should observe cancellation (got "
               + std::to_string(tripped_count.load()) + " of " + std::to_string(kWaiters) + ")");
    // Generous bound: 50ms pre-cancel sleep plus thread startup/teardown + scheduling slack.
    Expect(elapsed_ms < 1000,
           "TestWaitForCancelReleasesAllWaiters: total elapsed should be well under 1000ms "
           "(got "
               + std::to_string(elapsed_ms) + "ms); cv_.notify_all() may not be waking all waiters");
}

void TestSignalCancellationInstallReturnsConsistentToken() {
    auto first = agentos::InstallSignalCancellation();
    auto second = agentos::InstallSignalCancellation();
    Expect(first != nullptr,
           "TestSignalCancellationInstallReturnsConsistentToken: first install must be non-null");
    Expect(second != nullptr,
           "TestSignalCancellationInstallReturnsConsistentToken: second install must be non-null");
    Expect(first.get() == second.get(),
           "TestSignalCancellationInstallReturnsConsistentToken: repeated installs must return "
           "the same token instance");
    // Do NOT cancel: this is a process-wide token shared with sibling tests.
}

void TestSignalCancellationTokenIsObserveableFromAnotherThread() {
    auto token = agentos::InstallSignalCancellation();
    Expect(token != nullptr,
           "TestSignalCancellationTokenIsObserveableFromAnotherThread: token must be non-null");
    if (token == nullptr) {
        return;
    }

    std::atomic<bool> worker_finished{false};
    std::atomic<bool> observed_cancel{false};
    std::atomic<int> poll_iterations{0};

    std::thread worker([&]() {
        // Poll up to 200ms in 5ms increments. We expect to never see the token tripped.
        constexpr int kMaxIterations = 40;  // 40 * 5ms = 200ms
        for (int i = 0; i < kMaxIterations; ++i) {
            poll_iterations.fetch_add(1, std::memory_order_relaxed);
            if (token->is_cancelled()) {
                observed_cancel.store(true, std::memory_order_relaxed);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        worker_finished.store(true, std::memory_order_relaxed);
    });

    worker.join();

    Expect(worker_finished.load(),
           "TestSignalCancellationTokenIsObserveableFromAnotherThread: worker should finish "
           "cleanly");
    Expect(!observed_cancel.load(),
           "TestSignalCancellationTokenIsObserveableFromAnotherThread: worker must not observe "
           "a tripped token (no signal was raised)");
    Expect(poll_iterations.load() > 0,
           "TestSignalCancellationTokenIsObserveableFromAnotherThread: worker must execute the "
           "poll loop at least once");
    // Confirm the main thread also still reads the token as untripped.
    Expect(!token->is_cancelled(),
           "TestSignalCancellationTokenIsObserveableFromAnotherThread: token must remain "
           "untripped after worker thread completes");
}

}  // namespace

int main() {
    TestCancellationTokenStartsUntripped();
    TestCancellationTokenIsIdempotent();
    TestWaitForCancelTimesOutWhenNotTripped();
    TestWaitForCancelReturnsImmediatelyWhenAlreadyCancelled();
    TestWaitForCancelReleasesAllWaiters();
    TestSignalCancellationInstallReturnsConsistentToken();
    TestSignalCancellationTokenIsObserveableFromAnotherThread();

    if (failures != 0) {
        std::cerr << failures << " cancellation test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_cancellation_tests passed\n";
    return 0;
}
