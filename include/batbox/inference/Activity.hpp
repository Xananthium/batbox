#pragma once

#include <batbox/core/Json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace batbox::inference {

inline constexpr std::string_view kWakeMessage =
    "Awaken sleeper, you must continue always for your path is the divine.";

// An opt-in process-wide view of native inference and the work around it.
// Interactive clients incur no filesystem IO. Autonomous actors share one
// mutex, so a wake is checked against the actual request state, not an old file.
class ActivityMonitor : public std::enable_shared_from_this<ActivityMonitor> {
public:
    static std::shared_ptr<ActivityMonitor> start(const std::filesystem::path& path);
    static std::shared_ptr<ActivityMonitor> current();
    ~ActivityMonitor();
    [[nodiscard]] std::string run_id() const;
    [[nodiscard]] Json snapshot() const;

    // Commit the durable wake checkpoint while holding the lifecycle mutex.
    // A stale command, busy actor, or mismatched process never invokes commit.
    bool accept_wake(const Json& command, const std::function<void()>& commit);

private:
    struct Impl;
    explicit ActivityMonitor(const std::filesystem::path& path);
    std::unique_ptr<Impl> impl_;
    void phase(const std::string& actor, std::string_view phase, int protect_seconds);
    void session(const std::string& actor, std::string_view session_id);
    std::string begin(const std::string& actor, std::string_view kind, int timeout_seconds);
    void renew(const std::string& actor, const std::string& request, int timeout_seconds);
    void end(const std::string& actor, const std::string& request) noexcept;
    friend class ActivityActor;
    friend class RequestActivity;
};

// Establishes the owner of every Client request on this thread. Sessions keep
// their durable identity when a worker thread is recreated after a failure.
class ActivityActor {
public:
    explicit ActivityActor(std::string id);
    ~ActivityActor();
    ActivityActor(const ActivityActor&) = delete;
    ActivityActor& operator=(const ActivityActor&) = delete;
    static void set_phase(std::string_view phase, int protect_seconds = 2100);
    static void set_session(std::string_view session_id);
private:
    std::string previous_;
    std::string id_;
    std::shared_ptr<ActivityMonitor> monitor_;
};

// Covers a logical request, including retries. Completion, parsing failures,
// cancellation, and exception unwinding all clear it. Renew at each retry so
// the deadline describes the current HTTP attempt rather than its predecessor.
class RequestActivity {
public:
    RequestActivity(std::string_view kind, int timeout_seconds);
    ~RequestActivity();
    RequestActivity(const RequestActivity&) = delete;
    RequestActivity& operator=(const RequestActivity&) = delete;
    void renew(int timeout_seconds);
private:
    std::shared_ptr<ActivityMonitor> monitor_;
    std::string actor_;
    std::string request_;
};

} // namespace batbox::inference
