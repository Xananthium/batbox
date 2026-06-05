// tests/unit/test_compaction_standdown.cpp
// =============================================================================
// S5 (DIS-983) AC5 — S9 stand-down: compaction is a no-op when the active
// provider manages its own context window.
//
// compaction_should_run(needs, manages) == (needs && !manages) is the pure
// decision both the proactive (pre-flight) and reactive (overflow-retry)
// compaction paths in Conversation consult.  This test exercises the full truth
// table AND ties it to the REAL Provider object: an OpenAiCompatibleProvider
// constructed with the S9 opt-out flag reports manages_own_context()==true, and
// feeding that into the predicate stands compaction down — proving the wiring is
// not vacuous.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_compaction_standdown.cpp \
//       src/conversation/Compactor.cpp src/conversation/Message.cpp \
//       src/inference/Provider.cpp src/inference/Client.cpp \
//       src/inference/ChatRequest.cpp src/inference/SseParser.cpp \
//       src/inference/CanonicalModel.cpp src/inference/ReasoningTagProfile.cpp \
//       src/inference/ThinkSplitter.cpp \
//       src/core/Uuid.cpp src/core/CancelToken.cpp src/core/Logging.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/x64-linux/lib/libcpr.a \
//       build/vcpkg_installed/x64-linux/lib/libcurl.a \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       build/vcpkg_installed/x64-linux/lib/libssl.a \
//       build/vcpkg_installed/x64-linux/lib/libcrypto.a \
//       build/vcpkg_installed/x64-linux/lib/libz.a -lpthread -ldl \
//       -o /tmp/test_compaction_standdown && /tmp/test_compaction_standdown
//   (Uuid.cpp must be the DIS-969-fixed copy.)
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Compactor.hpp>
#include <batbox/inference/Provider.hpp>
#include <batbox/config/Config.hpp>

using batbox::conversation::compaction_should_run;

TEST_SUITE("S5 compaction stand-down (AC5)") {

    // -----------------------------------------------------------------------
    // The pure decision predicate — full truth table.
    // -----------------------------------------------------------------------
    TEST_CASE("compaction_should_run truth table") {
        // gate fires, provider does NOT own context → compact.
        CHECK(compaction_should_run(/*needs=*/true,  /*manages=*/false) == true);
        // provider owns its own window → stand down even when the gate fires.
        CHECK(compaction_should_run(/*needs=*/true,  /*manages=*/true)  == false);
        // gate quiet → never compact regardless of ownership.
        CHECK(compaction_should_run(/*needs=*/false, /*manages=*/false) == false);
        CHECK(compaction_should_run(/*needs=*/false, /*manages=*/true)  == false);
    }

    // -----------------------------------------------------------------------
    // Tie the decision to the REAL Provider object (not a bare bool) so the
    // wiring is provably non-vacuous.
    // -----------------------------------------------------------------------
    TEST_CASE("a context-owning provider stands compaction down") {
        batbox::config::Config cfg;
        cfg.api.base_url = "http://127.0.0.1:1/v1";  // never dialled here

        // S9 opt-out flag true → provider owns its window.
        batbox::inference::OpenAiCompatibleProvider owns_ctx{cfg, /*manages=*/true};
        REQUIRE(owns_ctx.manages_own_context() == true);
        CHECK(compaction_should_run(true, owns_ctx.manages_own_context()) == false);

        // Default provider → batbox owns the window → compaction runs.
        batbox::inference::OpenAiCompatibleProvider batbox_owns{cfg};
        REQUIRE(batbox_owns.manages_own_context() == false);
        CHECK(compaction_should_run(true, batbox_owns.manages_own_context()) == true);
    }
}
