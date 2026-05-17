// src/tools/CronScheduler.cpp
//
// Implementation of batbox::tools::CronScheduler, CronEntry, CronExpr,
// and CronField.
//
// Cron expression format (5-field, no seconds):
//   "MIN HOUR DOM MON DOW"
//
//   Each field accepts:
//     *           — matches any value in the field's range
//     N           — exact integer (e.g. "5")
//     N,M,P       — comma-separated list of integers
//     */N         — step: every N units (e.g. "*/15" in minutes → 0,15,30,45)
//     A-B         — range (inclusive, e.g. "1-5")
//     A-B/N       — range with step
//
//   Field ranges:
//     minute  [0, 59]
//     hour    [0, 23]
//     dom     [1, 31]
//     month   [1, 12]
//     dow     [0, 6]   (Sunday=0)
//
// Scheduler loop:
//   - Wakes every 1s (interrupted on stop).
//   - For each enabled entry, computes next fire time (caches it in-memory).
//   - When next_fire_time <= now: fires callback, recomputes next fire time.
//   - Persists no changes to disk on fire (entries are persistent by design).
//
// Blueprint contract: CPP 5.17

#include <batbox/tools/CronScheduler.hpp>
#include <batbox/core/Uuid.hpp>

#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// expand_tilde
// ---------------------------------------------------------------------------
fs::path expand_tilde(std::string_view p) {
    if (p.empty() || p[0] != '~') {
        return fs::path(std::string(p));
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return fs::path(home) / fs::path(std::string(p.substr(2)));
    }
    return fs::temp_directory_path() / "batbox_cron_fallback";
}

// ---------------------------------------------------------------------------
// parse_int_range
//
// Parse "A-B" or "A" into [lo,hi] inclusive (0 on parse error).
// Returns false on error.
// ---------------------------------------------------------------------------
bool parse_int_range(const std::string& s, int& lo, int& hi) {
    const auto dash = s.find('-');
    try {
        if (dash == std::string::npos) {
            lo = hi = std::stoi(s);
        } else {
            lo = std::stoi(s.substr(0, dash));
            hi = std::stoi(s.substr(dash + 1));
        }
        return lo <= hi;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// parse_cron_field
//
// Parses one cron field token into a CronField given [range_lo, range_hi].
// Returns false on error.
// ---------------------------------------------------------------------------
bool parse_cron_field(const std::string& token,
                      int                range_lo,
                      int                range_hi,
                      CronField&         out) {
    out.is_wildcard = false;
    out.values.clear();

    if (token == "*") {
        out.is_wildcard = true;
        return true;
    }

    // Check for step syntax: "*/N" or "A-B/N"
    const auto slash = token.find('/');
    if (slash != std::string::npos) {
        int step = 0;
        try {
            step = std::stoi(token.substr(slash + 1));
        } catch (...) {
            return false;
        }
        if (step <= 0) return false;

        const std::string base = token.substr(0, slash);
        int lo = range_lo, hi = range_hi;
        if (base != "*") {
            if (!parse_int_range(base, lo, hi)) return false;
        }
        for (int v = lo; v <= hi; v += step) {
            if (v >= range_lo && v <= range_hi) {
                out.values.push_back(v);
            }
        }
        if (out.values.empty()) return false;
        std::sort(out.values.begin(), out.values.end());
        return true;
    }

    // Comma-separated list (each element may be A-B or N)
    std::istringstream ss(token);
    std::string elem;
    while (std::getline(ss, elem, ',')) {
        if (elem.empty()) return false;
        int lo = 0, hi = 0;
        if (!parse_int_range(elem, lo, hi)) return false;
        for (int v = lo; v <= hi; ++v) {
            if (v < range_lo || v > range_hi) return false;
            out.values.push_back(v);
        }
    }
    if (out.values.empty()) return false;
    std::sort(out.values.begin(), out.values.end());
    out.values.erase(std::unique(out.values.begin(), out.values.end()),
                     out.values.end());
    return true;
}

} // anonymous namespace

// =============================================================================
// CronField
// =============================================================================

bool CronField::matches(int v) const noexcept {
    if (is_wildcard) return true;
    for (int x : values) {
        if (x == v) return true;
    }
    return false;
}

// =============================================================================
// CronExpr
// =============================================================================

bool CronExpr::parse(const std::string& expr, CronExpr& out) {
    // Split into exactly 5 whitespace-separated tokens.
    std::istringstream ss(expr);
    std::vector<std::string> tokens;
    std::string tok;
    while (ss >> tok) {
        tokens.push_back(tok);
    }
    if (tokens.size() != 5) return false;

    // minute [0,59], hour [0,23], dom [1,31], month [1,12], dow [0,6]
    CronExpr tmp;
    if (!parse_cron_field(tokens[0],  0, 59, tmp.minute)) return false;
    if (!parse_cron_field(tokens[1],  0, 23, tmp.hour))   return false;
    if (!parse_cron_field(tokens[2],  1, 31, tmp.dom))    return false;
    if (!parse_cron_field(tokens[3],  1, 12, tmp.month))  return false;
    if (!parse_cron_field(tokens[4],  0,  6, tmp.dow))    return false;

    out = std::move(tmp);
    return true;
}

std::optional<std::time_t>
CronExpr::next_fire(std::time_t from_time) const noexcept {
    // Advance by 1 minute from from_time (exclusive) then search.
    // We search at 1-minute granularity for up to ~4 years (2,102,400 minutes).
    constexpr int kMaxMinutes = 4 * 366 * 24 * 60;

    // Align to start of the minute after from_time.
    std::time_t t = from_time + 60 - (from_time % 60);

    for (int i = 0; i < kMaxMinutes; ++i, t += 60) {
        std::tm tm_val{};
#if defined(_WIN32)
        if (gmtime_s(&tm_val, &t) != 0) continue;
#else
        if (gmtime_r(&t, &tm_val) == nullptr) continue;
#endif
        const int t_min   = tm_val.tm_min;
        const int t_hour  = tm_val.tm_hour;
        const int t_dom   = tm_val.tm_mday;
        const int t_month = tm_val.tm_mon + 1;  // tm_mon is [0,11]
        const int t_dow   = tm_val.tm_wday;     // [0,6] Sunday=0

        if (!minute.matches(t_min))        continue;
        if (!this->hour.matches(t_hour))   continue;
        if (!this->month.matches(t_month)) continue;
        // dom AND dow: standard cron OR semantics when both are non-wildcard.
        // When both are wildcards or only one is restricted, standard behaviour.
        const bool dom_wild = this->dom.is_wildcard;
        const bool dow_wild = this->dow.is_wildcard;
        if (!dom_wild && !dow_wild) {
            // Both restricted: fire if EITHER matches (standard cron).
            if (!this->dom.matches(t_dom) && !this->dow.matches(t_dow)) continue;
        } else {
            if (!this->dom.matches(t_dom)) continue;
            if (!this->dow.matches(t_dow)) continue;
        }
        return t;
    }
    return std::nullopt;
}

// =============================================================================
// CronEntry — serialisation
// =============================================================================

Json CronEntry::to_json() const {
    return Json{
        {"id",         id},
        {"expression", expression},
        {"prompt",     prompt},
        {"enabled",    enabled},
        {"created_at", created_at},
    };
}

bool CronEntry::from_json(const Json& j, CronEntry& out) {
    if (!j.is_object()) return false;

    auto get_str = [&](const char* key, std::string& dst, bool required) -> bool {
        auto it = j.find(key);
        if (it == j.end() || !it->is_string()) {
            return !required;
        }
        dst = it->get<std::string>();
        return true;
    };

    if (!get_str("id",         out.id,         true))  return false;
    if (!get_str("expression", out.expression, true))  return false;
    if (!get_str("prompt",     out.prompt,     true))  return false;
    if (!get_str("created_at", out.created_at, false)) {}

    // enabled: optional bool, default true
    {
        auto it = j.find("enabled");
        if (it != j.end() && it->is_boolean()) {
            out.enabled = it->get<bool>();
        } else {
            out.enabled = true;
        }
    }

    return true;
}

// =============================================================================
// CronScheduler — constructor / destructor
// =============================================================================

CronScheduler::CronScheduler(std::filesystem::path cron_path)
    : cron_path_(std::move(cron_path)) {}

CronScheduler::~CronScheduler() {
    stop();
}

// =============================================================================
// CronScheduler — lifecycle
// =============================================================================

void CronScheduler::set_fire_callback(CronFireCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    fire_callback_ = std::move(cb);
}

void CronScheduler::start() {
    thread_ = std::jthread([this](std::stop_token st) {
        scheduler_loop(std::move(st));
    });
}

void CronScheduler::stop() noexcept {
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

// =============================================================================
// CronScheduler — scheduler loop
// =============================================================================

void CronScheduler::scheduler_loop(std::stop_token stop_tok) {
    // Per-entry cached next fire time (keyed by entry id).
    // Rebuilt when entries change (we reload on each tick).
    struct ScheduledEntry {
        CronEntry   entry;
        std::time_t next_fire = 0;
    };

    std::vector<ScheduledEntry> scheduled;

    while (!stop_tok.stop_requested()) {
        const std::time_t now = now_time_t();

        // ------------------------------------------------------------------
        // 1. Reload entries from disk and synchronise with scheduled cache.
        // ------------------------------------------------------------------
        std::vector<CronEntry> entries;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            entries = load();
        }

        // Build new scheduled list, preserving cached next_fire values.
        std::vector<ScheduledEntry> new_scheduled;
        new_scheduled.reserve(entries.size());
        for (const auto& e : entries) {
            if (!e.enabled) continue;
            CronExpr expr;
            if (!CronExpr::parse(e.expression, expr)) continue;

            // Look up cached next_fire.
            std::time_t nf = 0;
            for (const auto& s : scheduled) {
                if (s.entry.id == e.id) {
                    nf = s.next_fire;
                    break;
                }
            }
            if (nf == 0) {
                // First time seeing this entry: compute next fire strictly
                // after now (exclusive).  Passing `now` (not `now - 1`) to
                // next_fire() ensures we get the next minute boundary AFTER
                // the current second.  Using `now - 1` caused an off-by-60s
                // bug when the scheduler loop happened to start exactly at a
                // UTC minute boundary (now % 60 == 0): next_fire(now - 1)
                // returned `now` itself, firing the entry 60 seconds before
                // the test's expected_fire = now + 60 - (now % 60).
                auto opt_nf = expr.next_fire(now);
                nf = opt_nf.value_or(0);
            }
            new_scheduled.push_back({e, nf});
        }
        scheduled = std::move(new_scheduled);

        // ------------------------------------------------------------------
        // 2. Fire entries that are due.
        // ------------------------------------------------------------------
        for (auto& s : scheduled) {
            if (s.next_fire == 0) continue;
            if (s.next_fire > now) continue;

            // Fire!
            {
                CronFireCallback cb;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    cb = fire_callback_;
                }
                if (cb) {
                    try {
                        cb(s.entry);
                    } catch (...) {
                        // Callbacks must not propagate exceptions.
                    }
                }
            }

            // Recompute next fire time after firing (use fire time as baseline).
            CronExpr expr;
            if (CronExpr::parse(s.entry.expression, expr)) {
                auto opt_nf = expr.next_fire(s.next_fire);
                s.next_fire = opt_nf.value_or(0);
            } else {
                s.next_fire = 0;
            }
        }

        // ------------------------------------------------------------------
        // 3. Sleep for 1 second (interruptible by stop).
        //    Poll every 50ms to check stop_requested() so we respond promptly.
        // ------------------------------------------------------------------
        for (int tick = 0; tick < 20 && !stop_tok.stop_requested(); ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// =============================================================================
// CronScheduler — CRUD
// =============================================================================

std::optional<CronEntry> CronScheduler::create_entry(
    const std::string& expression,
    const std::string& prompt,
    bool               enabled)
{
    // Validate expression first.
    CronExpr expr;
    if (!CronExpr::parse(expression, expr)) {
        return std::nullopt;
    }

    if (prompt.empty()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lk(mutex_);

    auto entries = load();

    CronEntry e;
    e.id         = batbox::Uuid::v4().to_string();
    e.expression = expression;
    e.prompt     = prompt;
    e.enabled    = enabled;
    e.created_at = now_iso8601();

    entries.push_back(e);

    if (!save(entries)) {
        return std::nullopt;
    }

    return e;
}

std::vector<CronEntry> CronScheduler::list_entries() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return load();
}

bool CronScheduler::delete_entry(const std::string& id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto entries = load();

    const std::size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&id](const CronEntry& e) { return e.id == id; }),
                  entries.end());

    if (entries.size() == before) {
        return false;  // Not found.
    }

    return save(entries);
}

// =============================================================================
// CronScheduler — low-level helpers
// =============================================================================

std::filesystem::path CronScheduler::default_path() {
    return expand_tilde("~/.batbox/cron.json");
}

std::vector<CronEntry> CronScheduler::load() const {
    std::error_code ec;
    if (!fs::exists(cron_path_, ec) || ec) {
        return {};
    }

    std::ifstream in(cron_path_, std::ios::in | std::ios::binary);
    if (!in) return {};

    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    if (text.empty()) return {};

    Json j;
    try {
        j = Json::parse(text);
    } catch (...) {
        return {};
    }

    if (!j.is_array()) return {};

    std::vector<CronEntry> result;
    result.reserve(j.size());
    for (const auto& item : j) {
        CronEntry e;
        if (CronEntry::from_json(item, e)) {
            result.push_back(std::move(e));
        }
    }
    return result;
}

bool CronScheduler::save(const std::vector<CronEntry>& entries) const {
    if (!ensure_dir()) return false;

    Json arr = Json::array();
    for (const auto& e : entries) {
        arr.push_back(e.to_json());
    }

    const std::string text = arr.dump(2);

    const fs::path tmp = cron_path_.parent_path()
                       / (std::string(".cron_tmp_") + cron_path_.filename().string());

    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }

    std::error_code ec;
    fs::rename(tmp, cron_path_, ec);
    if (ec) {
        fs::copy_file(tmp, cron_path_, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return false;
    }

    return true;
}

const std::filesystem::path& CronScheduler::path() const noexcept {
    return cron_path_;
}

bool CronScheduler::ensure_dir() const {
    std::error_code ec;
    fs::create_directories(cron_path_.parent_path(), ec);
    return !ec;
}

std::string CronScheduler::now_iso8601() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_val{};
#if defined(_WIN32)
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_val);
    return buf;
}

std::time_t CronScheduler::now_time_t() noexcept {
    return std::time(nullptr);
}

} // namespace batbox::tools
