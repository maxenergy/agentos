#include "utils/cancellation.hpp"

namespace agentos {

void CancellationToken::cancel() noexcept {
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
}

bool CancellationToken::is_cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
}

bool CancellationToken::wait_for_cancel(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this] {
        return cancelled_.load(std::memory_order_acquire);
    });
}

}  // namespace agentos
