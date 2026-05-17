// src/app/McpStatusPoller.cpp
// =============================================================================
// McpStatusPoller implementation — see include/batbox/app/McpStatusPoller.hpp.
//
// The polling loop:
//   1. Wait on a condition_variable for tick_interval_ or until stop_flag_ is set.
//   2. If stop_flag_ is set, exit.
//   3. Call registry_->count_failed_servers().
//   4. If the count changed from the last observed value, call on_change_(count).
//   5. Repeat.
//
// Destruction:
//   Sets stop_flag_ = true, notifies cv_ (interrupting any in-progress sleep),
//   then joins the thread.  The join completes promptly because the cv_ wakeup
//   causes the loop to check stop_flag_ and exit immediately.
// =============================================================================

#include <batbox/app/McpStatusPoller.hpp>

#include <batbox/core/Logging.hpp>

namespace batbox::app {

// =============================================================================
// Constructor / Destructor
// =============================================================================

McpStatusPoller::McpStatusPoller(
    batbox::mcp::McpServerRegistry*  registry,
    std::function<void(int)>         on_change,
    std::chrono::milliseconds        tick_interval)
    : registry_(registry)
    , on_change_(std::move(on_change))
    , tick_interval_(tick_interval)
{
    // Start the polling thread immediately.  The thread references this object,
    // so the object must be fully constructed before the thread can run.
    thread_ = std::thread([this]() { poll_loop(); });
}

McpStatusPoller::~McpStatusPoller()
{
    // Signal the polling thread to exit.
    stop_flag_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Notify under the lock so the CV wakeup is not lost between the
        // wait_for check and the actual wait.
    }
    cv_.notify_one();

    if (thread_.joinable()) {
        thread_.join();
    }
}

// =============================================================================
// Polling loop (runs on background thread)
// =============================================================================

void McpStatusPoller::poll_loop()
{
    int last_count = -1; // -1 forces the callback on the first poll

    while (true) {
        // Sleep for tick_interval_ or until stop_flag_ is set.
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, tick_interval_, [this]() {
                return stop_flag_.load(std::memory_order_relaxed);
            });
        }

        if (stop_flag_.load(std::memory_order_relaxed)) {
            break;
        }

        const int count = registry_->count_failed_servers();
        if (count != last_count) {
            last_count = count;
            BATBOX_LOG_DEBUG("McpStatusPoller: failed server count changed to {}", count);
            if (on_change_) {
                on_change_(count);
            }
        }
    }

    BATBOX_LOG_DEBUG("McpStatusPoller: polling thread exited");
}

} // namespace batbox::app
