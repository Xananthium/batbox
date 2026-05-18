// tests/unit/test_tool_registry_schema_cache.cpp
//
// doctest suite for PEXT2 D-2: ToolRegistry::available_tool_schemas caching.
//
// Coverage:
//   - Unfiltered call: schema_json() invoked exactly once per tool on first
//     call; subsequent unfiltered calls hit the cache (zero new schema_json()
//     invocations).
//   - Cache invalidation: registering a 6th tool increments schemas_version_,
//     causing the next unfiltered call to rebuild (all 6 tools' schema_json()
//     called once more).
//   - Filtered call: never served from cache; schema_json() is called for
//     matched tools on every filtered invocation.
//   - Filtered calls do not pollute the unfiltered cache.
//
// Build (standalone, no CMake):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_tool_registry_schema_cache.cpp \
//       src/tools/ToolRegistry.cpp \
//       -o /tmp/test_schema_cache && /tmp/test_schema_cache

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace batbox;
using namespace batbox::tools;

// =============================================================================
// Test double: CountingTool
//
// Tracks how many times schema_json() has been called via an atomic counter.
// Allows the test to verify cache hits vs. cache misses.
// =============================================================================

class CountingTool final : public ITool {
public:
    explicit CountingTool(std::string name)
        : name_(std::move(name))
        , schema_call_count_(0)
    {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return "Counting mock tool."; }

    [[nodiscard]] Json schema_json() const override {
        schema_call_count_.fetch_add(1, std::memory_order_relaxed);
        return Json{
            {"name",        name_},
            {"description", "Counting mock tool."},
            {"parameters",  Json{
                {"type",       "object"},
                {"properties", Json::object()},
                {"required",   Json::array()}
            }}
        };
    }

    [[nodiscard]] ToolResult run(const Json& /*args*/, ToolContext& /*ctx*/) override {
        return ToolResult::ok(name_ + ":ok");
    }

    [[nodiscard]] bool is_read_only()          const override { return true; }
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    /// Returns the number of times schema_json() has been called so far.
    [[nodiscard]] int schema_call_count() const noexcept {
        return schema_call_count_.load(std::memory_order_relaxed);
    }

    /// Resets the counter to zero.
    void reset_count() noexcept {
        schema_call_count_.store(0, std::memory_order_relaxed);
    }

private:
    std::string               name_;
    mutable std::atomic<int>  schema_call_count_;
};

// =============================================================================
// Helper: make_counting_tool — keeps a raw pointer for counter inspection
// =============================================================================

static CountingTool* make_counting_tool(ToolRegistry& reg, const std::string& name) {
    auto owned = std::make_unique<CountingTool>(name);
    CountingTool* raw = owned.get();
    reg.register_tool(std::move(owned));
    return raw;
}

// =============================================================================
// TEST SUITE 1: Unfiltered cache — schema_json() called once per build
// =============================================================================

TEST_SUITE("ToolRegistry schema cache — unfiltered") {

    TEST_CASE("schema_json() called exactly once per tool on first unfiltered call") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 5; ++i) {
            tools.push_back(make_counting_tool(reg, "tool_" + std::to_string(i)));
        }

        // All counters should be 0 before any schema call.
        for (auto* t : tools) {
            CHECK(t->schema_call_count() == 0);
        }

        // First call — cache miss; each tool's schema_json() called once.
        auto schemas = reg.available_tool_schemas();
        CHECK(schemas.size() == 5);
        for (auto* t : tools) {
            CHECK(t->schema_call_count() == 1);
        }
    }

    TEST_CASE("calls 2-10 are cache hits: schema_json() not called again") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 5; ++i) {
            tools.push_back(make_counting_tool(reg, "tool_" + std::to_string(i)));
        }

        // Prime the cache.
        auto first = reg.available_tool_schemas();
        CHECK(first.size() == 5);

        // Calls 2-10 should not invoke schema_json() again.
        for (int call = 2; call <= 10; ++call) {
            auto schemas = reg.available_tool_schemas();
            CHECK(schemas.size() == 5);
        }

        // Each tool's schema_json() was called exactly once (cache filled on
        // the first invocation; hits on calls 2-10 skip schema_json()).
        for (auto* t : tools) {
            CHECK(t->schema_call_count() == 1);
        }
    }

    TEST_CASE("cache contents match a freshly built list") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 5; ++i) {
            tools.push_back(make_counting_tool(reg, "tool_" + std::to_string(i)));
        }

        auto schemas = reg.available_tool_schemas();
        REQUIRE(schemas.size() == 5);

        // Every element should have the OpenAI envelope shape.
        for (const auto& entry : schemas) {
            REQUIRE(entry.is_object());
            CHECK(entry.contains("type"));
            CHECK(entry["type"].get<std::string>() == "function");
            CHECK(entry.contains("function"));
            const auto& fn = entry["function"];
            CHECK(fn.contains("name"));
            CHECK(fn.contains("description"));
            CHECK(fn.contains("parameters"));
        }
    }
}

// =============================================================================
// TEST SUITE 2: Cache invalidation — registering a new tool triggers rebuild
// =============================================================================

TEST_SUITE("ToolRegistry schema cache — invalidation on register_tool") {

    TEST_CASE("registering a 6th tool causes all 6 schema_json()s to be called on next query") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 5; ++i) {
            tools.push_back(make_counting_tool(reg, "tool_" + std::to_string(i)));
        }

        // Prime the cache with 5 tools.
        auto first = reg.available_tool_schemas();
        REQUIRE(first.size() == 5);
        for (auto* t : tools) {
            REQUIRE(t->schema_call_count() == 1);
        }

        // Register 6th tool — must invalidate the cache.
        auto* t6 = make_counting_tool(reg, "tool_5");
        tools.push_back(t6);

        // Next unfiltered call should rebuild: all 6 schema_json()s called once.
        auto second = reg.available_tool_schemas();
        REQUIRE(second.size() == 6);

        // Tools 0-4: schema_json() was called once during the first build,
        // and once more during the rebuild after invalidation → count == 2.
        for (int i = 0; i < 5; ++i) {
            CHECK(tools[i]->schema_call_count() == 2);
        }
        // Tool 5 was registered after the first build: first called during rebuild → count == 1.
        CHECK(t6->schema_call_count() == 1);
    }

    TEST_CASE("cache is stable after rebuild: 10 calls after 6th registration hit cache") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 5; ++i) {
            tools.push_back(make_counting_tool(reg, "tool_" + std::to_string(i)));
        }

        // Prime cache.
        (void)reg.available_tool_schemas();

        // Invalidate by registering 6th tool, then rebuild.
        tools.push_back(make_counting_tool(reg, "tool_5"));
        (void)reg.available_tool_schemas(); // rebuild

        // Record call counts after rebuild.
        std::vector<int> counts_after_rebuild;
        for (auto* t : tools) {
            counts_after_rebuild.push_back(t->schema_call_count());
        }

        // 10 more calls — all should be cache hits (counts unchanged).
        for (int i = 0; i < 10; ++i) {
            auto schemas = reg.available_tool_schemas();
            CHECK(schemas.size() == 6);
        }

        for (int i = 0; i < static_cast<int>(tools.size()); ++i) {
            CHECK(tools[i]->schema_call_count() == counts_after_rebuild[i]);
        }
    }
}

// =============================================================================
// TEST SUITE 3: Filtered calls — never cached, never pollute unfiltered cache
// =============================================================================

TEST_SUITE("ToolRegistry schema cache — filtered calls") {

    TEST_CASE("filtered call returns only matching tools") {
        ToolRegistry reg;
        make_counting_tool(reg, "alpha");
        make_counting_tool(reg, "beta");
        make_counting_tool(reg, "gamma");

        std::vector<std::string> allow = {"alpha", "gamma"};
        auto schemas = reg.available_tool_schemas(allow);
        REQUIRE(schemas.size() == 2);

        // Verify names in result.
        bool found_alpha = false, found_gamma = false;
        for (const auto& entry : schemas) {
            const std::string n = entry["function"]["name"].get<std::string>();
            if (n == "alpha") found_alpha = true;
            if (n == "gamma") found_gamma = true;
        }
        CHECK(found_alpha);
        CHECK(found_gamma);
    }

    TEST_CASE("filtered call does not prevent unfiltered cache from working") {
        ToolRegistry reg;
        std::vector<CountingTool*> tools;
        for (int i = 0; i < 4; ++i) {
            tools.push_back(make_counting_tool(reg, "t" + std::to_string(i)));
        }

        // Filtered call first.
        std::vector<std::string> allow = {"t0", "t1"};
        (void)reg.available_tool_schemas(allow);

        // Now unfiltered — should build cache and return all 4.
        auto unfiltered = reg.available_tool_schemas();
        CHECK(unfiltered.size() == 4);

        // Second unfiltered call should be a cache hit.
        for (auto* t : tools) {
            t->reset_count();
        }
        (void)reg.available_tool_schemas();
        for (auto* t : tools) {
            CHECK(t->schema_call_count() == 0);  // cache hit, no new calls
        }
    }

    TEST_CASE("repeated filtered calls each invoke schema_json() for matched tools") {
        ToolRegistry reg;
        CountingTool* ta = make_counting_tool(reg, "alpha");
        CountingTool* tb = make_counting_tool(reg, "beta");

        std::vector<std::string> allow = {"alpha"};

        // 3 filtered calls — each rebuilds (no caching for filtered path).
        for (int i = 1; i <= 3; ++i) {
            (void)reg.available_tool_schemas(allow);
            CHECK(ta->schema_call_count() == i);  // called on each filtered pass
            CHECK(tb->schema_call_count() == 0);  // beta is not in the allow list
        }
    }

    TEST_CASE("empty filter vector returns empty result without touching cache") {
        ToolRegistry reg;
        CountingTool* t = make_counting_tool(reg, "solo");

        // Prime unfiltered cache.
        (void)reg.available_tool_schemas();
        int count_after_prime = t->schema_call_count();

        // Filtered with empty allow-list.
        std::vector<std::string> empty_allow;
        auto schemas = reg.available_tool_schemas(empty_allow);
        CHECK(schemas.empty());

        // Unfiltered cache should still be valid.
        t->reset_count();
        (void)reg.available_tool_schemas();
        CHECK(t->schema_call_count() == 0);  // still a cache hit
    }
}
