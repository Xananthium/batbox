// include/batbox/app/McpStatusPoller.hpp
// =============================================================================
// McpStatusPoller — background thread that polls McpServerRegistry::count_failed_servers()
// once per second and fires a callback when the count changes.
//
// Design (TUI-FLOW-T11):
//   Owns a std::thread started in the constructor and joined in the destructor.
//   The polling loop uses a condition_variable + mutex so that the destructor
//   can interrupt the sleep immediately (no 1s hang on exit).
//
//   The on_change callback is called from the poller thread when the failed-
//   server count changes from the last observed value.  Callers must ensure
//   the callback is safe to invoke from a background thread.
//
//   For InputBar integration (TUI-FLOW-T11), mcp_failed_ is declared as
//   std::atomic<int> (see InputBar.hpp) so the callback can call
//   InputBar::set_mcp_failed(n) directly from the poller thread without
//   additional locking.
//
//   Constructor: McpStatusPoller(McpServerRegistry* reg,
//                                std::function<void(int)> on_change,
//                                std::chrono::milliseconds tick_interval = 1s)
//   Destructor:  sets stop_flag_, notifies the CV, joins the thread.
//   The tick_interval parameter exists so unit tests can drive a 100ms tick.
//
// Thread safety:
//   The registry pointer must remain valid for the lifetime of the poller.
//   The callback is invoked at most once per tick interval.
//   Destroying the poller from the callback is undefined behaviour (deadlock).
// =============================================================================

#pragma once

#include <batbox/mcp/McpServerRegistry.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace batbox::app {

/// Polls McpServerRegistry for failed servers once per tick and fires on_change
/// when the count differs from the previous observation.
///
/// Ownership model:
///   - Registry pointer is borrowed; must outlive this object.
///   - on_change callback is stored by value; called from the poller thread.
///
/// Destruction is safe and prompt: the background thread is interrupted via
/// stop_flag_ + condition_variable so it returns within one tick interval at most.
class McpStatusPoller {
public:
    /// Construct and start the background polling thread.
    ///
    /// @param registry       Registry to poll.  Must not be null.  Must outlive
    ///                       this object.
    /// @param on_change      Callback invoked from the poller thread when the
    ///                       count of failed servers changes.  Receives the new
    ///                       count as its only argument.
    /// @param tick_interval  How long to wait between polls (default 1 second).
    ///                       Tests may supply a shorter interval (e.g. 100ms).
    McpStatusPoller(
        batbox::mcp::McpServerRegistry*  registry,
        std::function<void(int)>         on_change,
        std::chrono::milliseconds        tick_interval =
            std::chrono::milliseconds{1000});

    /// Stop the polling thread and join it before returning.
    /// Safe to call from any thread except the poller thread itself.
    ~McpStatusPoller();

    // Non-copyable, non-movable (owns a std::thread).
    McpStatusPoller(const McpStatusPoller&)            = delete;
    McpStatusPoller& operator=(const McpStatusPoller&) = delete;
    McpStatusPoller(McpStatusPoller&&)                 = delete;
    McpStatusPoller& operator=(McpStatusPoller&&)      = delete;

private:
    /// Body of the background thread.
    void poll_loop();

    batbox::mcp::McpServerRegistry*  registry_;
    std::function<void(int)>         on_change_;
    std::chrono::milliseconds        tick_interval_;

    std::mutex              mu_;
    std::condition_variable cv_;
    std::atomic<bool>       stop_flag_{false};

    std::thread thread_;
};

} // namespace batbox::app
