#pragma once

#include "utils/cancellation.hpp"

#include <memory>

namespace agentos {

// Installs a process-wide Ctrl-C / SIGINT (and SIGTERM on POSIX) handler that
// trips the returned CancellationToken. Idempotent — subsequent calls return
// the same token and do not re-install the handler.
//
// First signal: token is cancelled, the handler returns, and any in-flight
// orchestration that holds the token cooperatively unwinds with `Cancelled`.
// Second signal: the handler restores the OS default disposition and re-raises
// the signal, so the user can still hard-kill if cooperative cancel is too
// slow. Stderr gets a one-line hint after the first signal.
//
// The token outlives the process (it is owned by a function-local static), so
// callers can safely capture it by `std::shared_ptr<CancellationToken>` and
// keep it past the lifetime of any single command handler.
std::shared_ptr<CancellationToken> InstallSignalCancellation();

}  // namespace agentos
