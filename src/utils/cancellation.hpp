#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace agentos {

// Shared cancellation signal passed by reference to long-running operations.
// One producer (the orchestrator) calls cancel(); many observers may poll
// is_cancelled() or wait().
//
// Replaces the old IAgentAdapter::cancel(task_id) method, which forced every
// adapter to maintain a private task_id -> state map.
class CancellationToken {
public:
    void cancel() noexcept;
    [[nodiscard]] bool is_cancelled() const noexcept;

    // Blocks until cancel() is called or `timeout` elapses. Returns true if
    // cancelled before timeout, false on timeout. Useful for adapters that
    // want to interrupt their own wait loops promptly.
    bool wait_for_cancel(std::chrono::milliseconds timeout) const;

private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
};

}  // namespace agentos
