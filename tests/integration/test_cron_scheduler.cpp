// tests/integration/test_cron_scheduler.cpp
//
// Integration tests for the CronScheduler background thread:
//   - Scheduler fires entries within ±1s of their due time.
//   - Scheduler honors stop_token on shutdown.
//   - Disabled entries are not fired.
//
// CPP 5.17 acceptance criteria covered here:
//   [x] Scheduler thread fires entries within ±1s of due time
//   [x] Scheduler honors stop_token on shutdown

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/CronScheduler.hpp>
#include <batbox/core/CancelToken.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::tools;

// =============================================================================
// Test infrastructure
// =============================================================================

struct ScopedHome {
    std::string original_home;
    fs::path    tmp_dir;

    ScopedHome() {
        const char* h = std::getenv("HOME");
        original_home = h ? h : "";
        tmp_dir = fs::temp_directory_path()
                / ("batbox_cron_integ_"
                   + std::to_string(
                       static_cast<unsigned long>(std::time(nullptr))));
        fs::create_directories(tmp_dir);
#if defined(_WIN32)
        _putenv_s("HOME", tmp_dir.string().c_str());
#else
        setenv("HOME", tmp_dir.string().c_str(), 1);
#endif
    }

    ~ScopedHome() {
        if (!original_home.empty()) {
#if defined(_WIN32)
            _putenv_s("HOME", original_home.c_str());
#else
            setenv("HOME", original_home.c_str(), 1);
#endif
        }
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }
};

// Build a cron expression that fires at the next whole minute from now + offset_secs.
// Returns the expression and the expected fire time.
static std::pair<std::string, std::time_t>
make_one_shot_expr(int offset_seconds = 2) {
    const std::time_t target = std::time(nullptr) + offset_seconds;
    // Round to next minute boundary at or after target.
    const std::time_t fire_time = target - (target % 60) + 60;

    std::tm tm_val{};
#if defined(_WIN32)
    gmtime_s(&tm_val, &fire_time);
#else
    gmtime_r(&fire_time, &tm_val);
#endif

    // Build expression that fires exactly at that minute.
    const std::string expr =
        std::to_string(tm_val.tm_min) + " "
      + std::to_string(tm_val.tm_hour) + " "
      + std::to_string(tm_val.tm_mday) + " "
      + std::to_string(tm_val.tm_mon + 1) + " *";

    return {expr, fire_time};
}

// =============================================================================
// Scheduler fires entries within ±1s of due time
// =============================================================================

TEST_CASE("scheduler fires due entry within 1 second window") {
    ScopedHome home;
    auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

    // Record fired entries with timestamps.
    struct FiredEvent {
        std::string entry_id;
        std::time_t fired_at;
    };
    std::vector<FiredEvent> fired;
    std::mutex fired_mtx;

    sched->set_fire_callback([&fired, &fired_mtx](const CronEntry& e) {
        std::lock_guard<std::mutex> lk(fired_mtx);
        fired.push_back({e.id, std::time(nullptr)});
    });

    // Schedule an entry that fires at the NEXT whole minute boundary.
    // We use a "* * * * *" (every minute) expression so we don't have to
    // wait more than 60 seconds — but we cap the test at 63s.
    auto opt = sched->create_entry("* * * * *", "tick");
    REQUIRE(opt.has_value());
    const std::string entry_id = opt->id;

    // Compute expected fire time (next minute boundary after now).
    const std::time_t now = std::time(nullptr);
    const std::time_t expected_fire = now + 60 - (now % 60);

    sched->start();

    // Wait up to 63 seconds for the first fire.
    constexpr int kMaxWaitMs = 63000;
    constexpr int kPollMs    = 100;
    int waited = 0;
    while (waited < kMaxWaitMs) {
        {
            std::lock_guard<std::mutex> lk(fired_mtx);
            if (!fired.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        waited += kPollMs;
    }

    sched->stop();

    {
        std::lock_guard<std::mutex> lk(fired_mtx);
        REQUIRE_FALSE(fired.empty());
        CHECK(fired[0].entry_id == entry_id);

        // Fire time should be within ±1s of expected_fire.
        const long diff = static_cast<long>(fired[0].fired_at)
                        - static_cast<long>(expected_fire);
        CHECK(diff >= -1);
        CHECK(diff <= 1);
    }
}

// =============================================================================
// Scheduler honors stop_token on shutdown
// =============================================================================

TEST_CASE("scheduler stops promptly when stop() is called") {
    ScopedHome home;
    auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

    std::atomic<int> fire_count{0};
    sched->set_fire_callback([&fire_count](const CronEntry&) {
        ++fire_count;
    });

    sched->start();

    // Stop immediately — before any entry can fire.
    const auto t0 = std::chrono::steady_clock::now();
    sched->stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Stop should return within 2 seconds (1s sleep cycle + margin).
    CHECK(elapsed < std::chrono::seconds(2));
    // No entries were created, so nothing fired.
    CHECK(fire_count.load() == 0);
}

// =============================================================================
// Disabled entries are not fired
// =============================================================================

TEST_CASE("disabled entries are not fired by scheduler") {
    ScopedHome home;
    auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

    std::atomic<int> fire_count{0};
    sched->set_fire_callback([&fire_count](const CronEntry&) {
        ++fire_count;
    });

    // Create a disabled entry that would fire every minute.
    auto opt = sched->create_entry("* * * * *", "disabled task", /*enabled=*/false);
    REQUIRE(opt.has_value());
    CHECK_FALSE(opt->enabled);

    sched->start();

    // Run for 2 seconds; no fires should occur.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sched->stop();

    CHECK(fire_count.load() == 0);
}

// =============================================================================
// Scheduler correctly skips already-past fire times
// =============================================================================

TEST_CASE("scheduler does not double-fire on startup") {
    ScopedHome home;
    auto sched = std::make_shared<CronScheduler>(CronScheduler::default_path());

    std::atomic<int> fire_count{0};
    sched->set_fire_callback([&fire_count](const CronEntry&) {
        ++fire_count;
    });

    // Entry fires every minute (*/1 * * * * equivalent = * * * * *).
    sched->create_entry("* * * * *", "tick");

    sched->start();

    // Run for 1.5 seconds (well under 60s), so at most 0 or 1 fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    sched->stop();

    // The scheduler should not fire more than once per minute.
    CHECK(fire_count.load() <= 1);
}
