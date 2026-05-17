// src/tools/SleepTool.cpp
//
// Implementation of batbox::tools::SleepTool.
//
// Interruptible sleep:
//   - Registers an on_cancel callback on ctx.cancel_token that notifies a
//     std::condition_variable.
//   - Calls cv.wait_for(lock, duration, predicate) where the predicate
//     returns true (stop waiting) when the token is cancelled.
//   - On normal completion returns "slept N seconds".
//   - On cancellation returns "(cancelled)" as a success result so the
//     model receives the message and can reason about it.
//
// Blueprint contract: batbox::tools::SleepTool (CPP 5.21)

#include <batbox/tools/SleepTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <chrono>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <string>

namespace batbox::tools {

namespace {

/// Maximum allowed sleep duration in seconds.
constexpr double kMaxSeconds = 300.0;

} // namespace

// ---------------------------------------------------------------------------
// name()
// ---------------------------------------------------------------------------
std::string_view SleepTool::name() const {
    return "Sleep";
}

// ---------------------------------------------------------------------------
// description()
// ---------------------------------------------------------------------------
std::string_view SleepTool::description() const {
    return "Sleep for a specified number of seconds (maximum 300); "
           "the sleep is cancellable and returns immediately if cancelled.";
}

// ---------------------------------------------------------------------------
// schema_json()
// ---------------------------------------------------------------------------
Json SleepTool::schema_json() const {
    return Json{
        {"name",        "Sleep"},
        {"description", "Sleep for a specified number of seconds (maximum 300); "
                        "the sleep is cancellable and returns immediately if cancelled."},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"seconds", Json{
                    {"type",        "number"},
                    {"description", "Duration to sleep in seconds. Must be >= 0 and <= 300."},
                    {"minimum",     0},
                    {"maximum",     300}
                }}
            }},
            {"required",   Json::array({"seconds"})}
        }}
    };
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------
ToolResult SleepTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 1. Validate arguments.
    // ------------------------------------------------------------------
    if (!args.contains("seconds") || !args["seconds"].is_number()) {
        return ToolResult::error(
            "Sleep: missing or non-numeric 'seconds' argument");
    }

    const double seconds = args["seconds"].get<double>();

    if (seconds < 0.0) {
        return ToolResult::error(
            "Sleep: 'seconds' must be >= 0 (got " + std::to_string(seconds) + ")");
    }

    if (seconds > kMaxSeconds) {
        return ToolResult::error(
            "Sleep: 'seconds' exceeds maximum of 300 (got " +
            std::to_string(seconds) + ")");
    }

    // ------------------------------------------------------------------
    // 2. Early-exit if already cancelled.
    // ------------------------------------------------------------------
    if (ctx.cancel_token.is_cancelled()) {
        return ToolResult::ok("(cancelled)");
    }

    // ------------------------------------------------------------------
    // 3. Interruptible sleep.
    //
    //    We allocate the mutex, condition variable, and cancelled flag on
    //    the heap (via shared_ptr) so the on_cancel callback — which may
    //    outlive this stack frame in theory, though in practice it is
    //    destroyed before we return — can safely access them.
    // ------------------------------------------------------------------
    auto mtx       = std::make_shared<std::mutex>();
    auto cv        = std::make_shared<std::condition_variable>();
    auto cancelled = std::make_shared<bool>(false);

    // Register a callback: when the cancel token fires, set the flag and
    // wake the waiting thread.
    auto cancel_handle = ctx.cancel_token.on_cancel([mtx, cv, cancelled]() {
        {
            std::lock_guard<std::mutex> lk(*mtx);
            *cancelled = true;
        }
        cv->notify_all();
    });

    const auto duration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(seconds));

    bool was_cancelled = false;
    {
        std::unique_lock<std::mutex> lk(*mtx);
        // wait_for returns false on timeout (full duration elapsed),
        // true when the predicate becomes true (cancelled).
        was_cancelled = cv->wait_for(lk, duration, [&cancelled]() {
            return *cancelled;
        });
    }

    // Release the on_cancel handle — deregisters the callback.
    cancel_handle.reset();

    if (was_cancelled || ctx.cancel_token.is_cancelled()) {
        return ToolResult::ok("(cancelled)");
    }

    const int whole_seconds = static_cast<int>(std::floor(seconds));
    return ToolResult::ok("slept " + std::to_string(whole_seconds) + " seconds");
}

} // namespace batbox::tools
