#include <batbox/inference/Activity.hpp>
#include <batbox/core/Uuid.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace batbox::inference {
namespace {
std::mutex current_mutex;
std::weak_ptr<ActivityMonitor> current_monitor;
thread_local std::string actor_id = "coordinator";

std::int64_t epoch_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string utc_now() {
    const auto now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}
bool idle_phase(std::string_view phase) {
    return phase == "waiting" || phase == "parked" || phase == "stopped"
        || phase == "errored" || phase == "completed";
}
void atomic_status(const std::filesystem::path& path, const std::string& text) {
    const auto temporary = path.string() + ".tmp." + Uuid::v4().to_string();
    int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot create activity snapshot");
    try {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto n = ::write(fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Cannot write activity snapshot");
            offset += static_cast<std::size_t>(n);
        }
        ::close(fd); fd = -1;
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}
} // namespace

struct ActivityMonitor::Impl {
    struct Actor {
        std::string session_id;
        std::string phase = "initializing";
        std::int64_t phase_started_at = epoch_now();
        std::int64_t protected_until = phase_started_at + 2100;
        std::map<std::string, Json> requests;
    };
    mutable std::mutex mutex;
    std::filesystem::path path;
    const std::string run_id = Uuid::v4().to_string();
    std::uint64_t revision = 0;
    std::map<std::string, Actor> actors;
    std::string phase = "initializing";
    bool wakeable = false;
    std::int64_t phase_started_at = epoch_now();
    std::string updated_at = utc_now();

    explicit Impl(std::filesystem::path p) : path(std::move(p)) {
        actors.try_emplace("coordinator");
    }

    Json snapshot() const {
        Json all = Json::array();
        std::size_t active = 0;
        std::int64_t protected_until = 0;
        for (const auto& [id, actor] : actors) {
            Json requests = Json::array();
            for (const auto& [request_id, request] : actor.requests) {
                (void)request_id;
                requests.push_back(request);
                protected_until = std::max(protected_until,
                    request.at("deadline_at").get<std::int64_t>());
            }
            active += actor.requests.size();
            // With an HTTP request in flight, its hard timeout is authoritative.
            // Idle sockets and token-progress timestamps do not renew this lease.
            if (actor.requests.empty() && !idle_phase(actor.phase))
                protected_until = std::max(protected_until, actor.protected_until);
            all.push_back({{"id", id}, {"session_id", actor.session_id},
                {"phase", actor.phase}, {"phase_started_at", actor.phase_started_at},
                {"active_requests", actor.requests.size()}, {"requests", std::move(requests)}});
        }
        return {{"schema", 1}, {"pid", ::getpid()}, {"run_id", run_id},
            {"revision", revision}, {"updated_at", updated_at}, {"phase", phase},
            {"phase_started_at", phase_started_at}, {"wakeable", wakeable},
            {"active_requests", active}, {"protected_until", protected_until},
            {"actors", std::move(all)}};
    }

    void publish() {
        const auto coordinator = actors.find("coordinator");
        const auto next_phase = coordinator == actors.end() ? "initializing" : coordinator->second.phase;
        bool next_wakeable = next_phase == "waiting";
        for (const auto& [id, actor] : actors) {
            (void)id;
            if (!actor.requests.empty() || !idle_phase(actor.phase)) next_wakeable = false;
        }
        if (phase != next_phase || wakeable != next_wakeable) phase_started_at = epoch_now();
        phase = next_phase;
        wakeable = next_wakeable;
        ++revision;
        updated_at = utc_now();
        atomic_status(path, snapshot().dump(2));
    }
};

ActivityMonitor::ActivityMonitor(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}
ActivityMonitor::~ActivityMonitor() = default;

std::shared_ptr<ActivityMonitor> ActivityMonitor::start(const std::filesystem::path& path) {
    auto result = std::shared_ptr<ActivityMonitor>(new ActivityMonitor(path));
    std::lock_guard lock(current_mutex);
    if (!current_monitor.expired()) throw std::runtime_error("Activity monitor already active");
    std::filesystem::create_directories(path.parent_path());
    result->impl_->publish();
    current_monitor = result;
    return result;
}
std::shared_ptr<ActivityMonitor> ActivityMonitor::current() {
    std::lock_guard lock(current_mutex);
    return current_monitor.lock();
}
std::string ActivityMonitor::run_id() const { return impl_->run_id; }
Json ActivityMonitor::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot();
}
void ActivityMonitor::phase(const std::string& id, std::string_view phase, int seconds) {
    std::lock_guard lock(impl_->mutex);
    auto& actor = impl_->actors[id];
    if (actor.phase == phase) return;
    actor.phase = phase;
    actor.phase_started_at = epoch_now();
    actor.protected_until = idle_phase(phase) ? 0 : epoch_now() + std::max(seconds, 1);
    impl_->publish();
}
void ActivityMonitor::session(const std::string& id, std::string_view session_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->actors[id].session_id = session_id;
    impl_->publish();
}
std::string ActivityMonitor::begin(const std::string& id, std::string_view kind, int seconds) {
    std::lock_guard lock(impl_->mutex);
    const auto request_id = Uuid::v4().to_string();
    auto& actor = impl_->actors[id];
    actor.requests[request_id] = {{"id", request_id}, {"kind", kind},
        {"started_at", epoch_now()}, {"deadline_at", epoch_now() + std::max(seconds, 1) + 5}};
    // A failed snapshot must never leave an internal phantom active request.
    try { impl_->publish(); }
    catch (...) { actor.requests.erase(request_id); throw; }
    return request_id;
}
void ActivityMonitor::renew(const std::string& id, const std::string& request, int seconds) {
    std::lock_guard lock(impl_->mutex);
    impl_->actors.at(id).requests.at(request)["deadline_at"] = epoch_now() + std::max(seconds, 1) + 5;
    impl_->publish();
}
void ActivityMonitor::end(const std::string& id, const std::string& request) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        auto& actor = impl_->actors.at(id);
        actor.requests.erase(request);
        // The next local operation starts now; do not inherit a pre-request
        // protection deadline which may have expired during long compaction.
        if (actor.requests.empty() && !idle_phase(actor.phase)) actor.protected_until = epoch_now() + 2100;
        impl_->publish();
    } catch (const std::exception& error) {
        std::cerr << "Activity completion snapshot failed: " << error.what() << '\n';
    }
}
bool ActivityMonitor::accept_wake(const Json& command, const std::function<void()>& commit) {
    std::lock_guard lock(impl_->mutex);
    if (!command.is_object()
        || !command.contains("schema") || !command["schema"].is_number_integer()
        || !command.contains("pid") || !command["pid"].is_number_integer()
        || !command.contains("revision") || !command["revision"].is_number_unsigned()
        || !command.contains("run_id") || !command["run_id"].is_string()
        || !command.contains("message") || !command["message"].is_string()
        || command["schema"] != 1
        || command["pid"] != ::getpid()
        || command["run_id"] != impl_->run_id
        || command["revision"] != impl_->revision
        || command["message"] != kWakeMessage
        || !command.contains("id") || !command["id"].is_string()
        || command["id"].get_ref<const std::string&>().empty()
        || command["id"].get_ref<const std::string&>().size() > 100
        || !impl_->wakeable) return false;
    commit();
    auto& coordinator = impl_->actors.at("coordinator");
    coordinator.phase = "waking";
    coordinator.phase_started_at = epoch_now();
    coordinator.protected_until = epoch_now() + 2100;
    impl_->publish();
    return true;
}

ActivityActor::ActivityActor(std::string id)
    : previous_(actor_id), id_(std::move(id)), monitor_(ActivityMonitor::current()) {
    actor_id = id_;
    try { if (monitor_) monitor_->phase(id_, "starting", 2100); }
    catch (const std::exception& error) {
        std::cerr << "Activity actor snapshot failed: " << error.what() << '\n';
    }
}
ActivityActor::~ActivityActor() {
    try { if (monitor_) monitor_->phase(id_, "stopped", 0); } catch (...) {}
    actor_id = previous_;
}
void ActivityActor::set_phase(std::string_view phase, int seconds) {
    if (auto monitor = ActivityMonitor::current()) monitor->phase(actor_id, phase, seconds);
}
void ActivityActor::set_session(std::string_view session_id) {
    if (auto monitor = ActivityMonitor::current()) monitor->session(actor_id, session_id);
}
RequestActivity::RequestActivity(std::string_view kind, int seconds)
    : monitor_(ActivityMonitor::current()), actor_(actor_id) {
    if (monitor_) request_ = monitor_->begin(actor_, kind, seconds);
}
RequestActivity::~RequestActivity() {
    if (monitor_) monitor_->end(actor_, request_);
}
void RequestActivity::renew(int seconds) {
    if (monitor_) monitor_->renew(actor_, request_, seconds);
}

} // namespace batbox::inference
