#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <batbox/inference/Activity.hpp>
#include <batbox/core/Uuid.hpp>

#include <filesystem>
#include <fstream>
#include <latch>
#include <thread>
#include <unistd.h>

using namespace batbox;
using namespace batbox::inference;

namespace {
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / Uuid::v4().to_string();
    std::shared_ptr<ActivityMonitor> monitor = ActivityMonitor::start(directory / "activity.json");
    ActivityActor coordinator{"coordinator"};
    ~Fixture() {
        ActivityActor::set_phase("stopped");
        std::filesystem::remove_all(directory);
    }
    Json command() {
        const auto state = monitor->snapshot();
        return {{"schema", 1}, {"id", "wake-test"}, {"pid", state["pid"]},
            {"run_id", state["run_id"]}, {"revision", state["revision"]},
            {"message", kWakeMessage}};
    }
};
}

TEST_CASE("inference activity survives silent requests and clears on exception") {
    Fixture f;
    ActivityActor::set_phase("running");
    ActivityActor::set_session("durable-coordinator-session");
    for (auto kind : {"tokenize", "nonstream_completion", "stream_completion"}) {
        try {
            RequestActivity request(kind, 1800);
            const auto state = f.monitor->snapshot();
            CHECK(state["active_requests"] == 1);
            CHECK_FALSE(state["wakeable"].get<bool>());
            const auto detail = state["actors"][0]["requests"][0];
            CHECK(detail["kind"] == kind);
            CHECK(detail["deadline_at"].get<long long>() - detail["started_at"].get<long long>() >= 1800);
            CHECK(state["actors"][0]["session_id"] == "durable-coordinator-session");
            throw std::runtime_error("transport/parse/cancellation path");
        } catch (const std::runtime_error&) {}
        CHECK(f.monitor->snapshot()["active_requests"] == 0);
    }
    ActivityActor::set_phase("tools_running");
    CHECK_FALSE(f.monitor->snapshot()["wakeable"].get<bool>());
    CHECK(f.monitor->snapshot()["protected_until"].get<long long>() > 0);
    ActivityActor::set_phase("waiting");
    CHECK(f.monitor->snapshot()["wakeable"].get<bool>());
    CHECK(f.monitor->snapshot()["protected_until"] == 0);
}

TEST_CASE("wake acceptance is fenced and durable commit occurs only once") {
    Fixture f;
    ActivityActor::set_phase("waiting");
    auto command = f.command();
    int commits = 0;
    auto commit = [&] { ++commits; };
    for (auto field : {"schema", "pid", "run_id", "revision", "message"}) {
        auto wrong = command;
        wrong[field] = nullptr;
        CHECK_FALSE(f.monitor->accept_wake(wrong, commit));
    }
    auto stale = command;
    stale["pid"] = ::getpid() + 1;
    CHECK_FALSE(f.monitor->accept_wake(stale, commit));
    stale = command;
    stale["run_id"] = "previous-process";
    CHECK_FALSE(f.monitor->accept_wake(stale, commit));
    CHECK_THROWS(f.monitor->accept_wake(command, [] { throw std::runtime_error("disk full"); }));
    CHECK(f.monitor->snapshot()["wakeable"].get<bool>());
    CHECK(f.monitor->accept_wake(command, commit));
    CHECK(commits == 1);
    CHECK_FALSE(f.monitor->accept_wake(command, commit));
    ActivityActor::set_phase("waiting");
    CHECK_FALSE(f.monitor->accept_wake(command, commit));
    CHECK(commits == 1);
}

TEST_CASE("all worker requests block coordinator wake until both scopes end") {
    Fixture f;
    ActivityActor::set_phase("waiting");
    const auto before = f.command();
    std::latch started{2};
    std::latch release{1};
    auto worker = [&](const char* id) {
        ActivityActor actor{id};
        ActivityActor::set_session(std::string("session-") + id);
        ActivityActor::set_phase("running");
        RequestActivity request("nonstream_completion", 1800);
        started.count_down();
        release.wait();
    };
    std::jthread one(worker, "worker-one");
    std::jthread two(worker, "worker-two");
    started.wait();
    const auto active = f.monitor->snapshot();
    CHECK(active["active_requests"] == 2);
    CHECK(active["actors"].size() == 3);
    CHECK_FALSE(active["wakeable"].get<bool>());
    CHECK_FALSE(f.monitor->accept_wake(before, [] {}));
    release.count_down();
    one.join(); two.join();
    CHECK(f.monitor->snapshot()["active_requests"] == 0);
    CHECK(f.monitor->snapshot()["wakeable"].get<bool>());
    CHECK_FALSE(f.monitor->accept_wake(before, [] {}));
    CHECK(f.monitor->accept_wake(f.command(), [] {}));
}

TEST_CASE("retry deadline renewal preserves request identity and count") {
    Fixture f;
    ActivityActor::set_phase("running");
    RequestActivity request("stream_completion", 1);
    auto initial = f.monitor->snapshot()["actors"][0]["requests"][0];
    request.renew(1800);
    auto later = f.monitor->snapshot();
    CHECK(later["active_requests"] == 1);
    CHECK(later["actors"][0]["requests"][0]["id"] == initial["id"]);
    CHECK(later["actors"][0]["requests"][0]["deadline_at"].get<long long>()
        > initial["deadline_at"].get<long long>());
}
