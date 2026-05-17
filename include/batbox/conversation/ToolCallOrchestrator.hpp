// include/batbox/conversation/ToolCallOrchestrator.hpp
// =============================================================================
// batbox::conversation::ToolCallOrchestrator — decode streamed tool_call deltas
// and dispatch to ToolRegistry after PermissionGate arbitration. (CPP 3.4)
//
// Overview:
//   ToolCallOrchestrator owns a ToolCallAccumulator and acts as the bridge
//   between the streaming inference layer and the tool dispatch layer.
//
//   Usage in the Conversation::run_turn() loop:
//
//     ToolCallOrchestrator orch(registry, gate);
//
//     // While streaming, for each StreamDelta:
//     if (delta.tool_calls) {
//         for (const auto& tc_delta : *delta.tool_calls)
//             orch.accumulate(tc_delta);
//     }
//     // On finish_reason == "tool_calls":
//     std::vector<Message> tool_msgs = orch.dispatch_all(ctx);
//     // Append tool_msgs to messages_ and loop back to inference.
//
// dispatch_all() flow (per accumulated ToolCall):
//   1. Finalise the ToolCallAccumulator (JSON parse each arguments_buf).
//   2. For each ToolCall:
//      a. If parse_error non-empty → emit Tool message with is_error=true.
//      b. PermissionGate::ask(name, args, ctx) → Decision.
//         - If Deny → emit Tool message: is_error=true, body="user denied tool call".
//         - If Allow + persist_rule → persist rule via gate's store.
//      c. Check ctx.cancel_token.is_cancelled() → abort remaining calls, return.
//      d. ToolRegistry::dispatch(name, args, ctx) → Result<ToolResult>.
//         - On Err → emit Tool message: is_error=true, body=error string.
//         - On Ok → emit Tool message from ToolResult.
//   3. Return all Tool messages (one per accumulated call).
//
// Progress events:
//   A progress_cb_ callback (optional) is called before each dispatch with the
//   tool name and arg JSON, allowing the TUI to update the status line.
//
// Cancellation:
//   dispatch_all() checks ctx.cancel_token.is_cancelled() before each tool
//   dispatch.  When fired, already-dispatched calls are returned; not-yet-
//   dispatched calls are skipped (no Tool message produced for them).
//
// Thread safety:
//   NOT thread-safe.  One instance per streaming turn.  The ToolCallAccumulator
//   is owned and not shared.
//
// Blueprint contract (blueprints table, task CPP 3.4):
//   class  batbox::conversation::ToolCallOrchestrator
//   method batbox::conversation::ToolCallOrchestrator::accumulate
//   method batbox::conversation::ToolCallOrchestrator::dispatch_all
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_tool_orchestration.cpp \
//       src/conversation/ToolCallOrchestrator.cpp \
//       src/conversation/Message.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/inference/ChatRequest.cpp \
//       src/tools/ToolRegistry.cpp \
//       src/permissions/PermissionGate.cpp \
//       src/permissions/PermissionMode.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PatternMatcher.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Json.cpp \
//       src/core/Paths.cpp src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_orch && /tmp/test_orch
// =============================================================================

#pragma once

#include <batbox/conversation/Message.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/inference/ChatResponse.hpp>       // ToolCallDelta
#include <batbox/inference/ToolCallAccumulator.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>

#include <functional>
#include <string>
#include <vector>

// Forward declaration to avoid pulling the full PermissionGate header into
// every TU that includes this orchestrator header.
namespace batbox::permissions { class PermissionGate; }

namespace batbox::conversation {

// =============================================================================
// ToolCallOrchestrator
// =============================================================================

class ToolCallOrchestrator {
public:
    // -------------------------------------------------------------------------
    // ProgressFn — optional callback invoked before each tool dispatch
    //
    // Parameters:
    //   tool_name — the tool about to be dispatched
    //   args      — the parsed JSON arguments (may be null if parse_error)
    //
    // The callback is invoked on the calling thread and must not throw.
    // It is intended for TUI status updates ("running Read...").
    // -------------------------------------------------------------------------
    using ProgressFn = std::function<void(std::string_view tool_name,
                                          const batbox::Json& args)>;

    // -------------------------------------------------------------------------
    // Construction
    //
    // registry    — look up tools by name; must outlive this instance.
    // gate        — permission arbitration; must outlive this instance.
    // progress_cb — optional; called before each tool dispatch (may be nullptr).
    // -------------------------------------------------------------------------
    ToolCallOrchestrator(batbox::tools::ToolRegistry&       registry,
                         batbox::permissions::PermissionGate& gate,
                         ProgressFn                         progress_cb = nullptr);

    // Non-copyable; movable.
    ToolCallOrchestrator(const ToolCallOrchestrator&)            = delete;
    ToolCallOrchestrator& operator=(const ToolCallOrchestrator&) = delete;
    ToolCallOrchestrator(ToolCallOrchestrator&&)                 = default;
    ToolCallOrchestrator& operator=(ToolCallOrchestrator&&)      = default;

    // -------------------------------------------------------------------------
    // accumulate — feed one ToolCallDelta into the internal accumulator
    //
    // Called for every element of StreamDelta::tool_calls during streaming.
    // May be called many times before dispatch_all().
    //
    // Blueprint contract: accumulate(const ToolCallDelta& delta)
    // -------------------------------------------------------------------------
    void accumulate(const batbox::inference::ToolCallDelta& delta);

    // -------------------------------------------------------------------------
    // dispatch_all — finalize, arbitrate, dispatch; return Tool messages
    //
    // Called once after finish_reason == "tool_calls".  Internally:
    //   1. Calls ToolCallAccumulator::finalize() to JSON-parse all args buffers.
    //   2. For each assembled ToolCall:
    //        - Parse error  → Tool message {is_error=true, body=parse_error}
    //        - Denied       → Tool message {is_error=true, body="user denied tool call"}
    //        - Cancelled    → stops iteration; returns messages assembled so far
    //        - Dispatch err → Tool message {is_error=true, body=error_string}
    //        - Success      → Tool message {is_error=<from ToolResult>}
    //   3. Returns a vector of Role::Tool Messages (one per accumulated call).
    //
    // @param ctx  Per-call context; cancel_token is polled before each dispatch.
    //
    // Blueprint contract: std::vector<Message> dispatch_all(ToolContext& ctx)
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<Message>
    dispatch_all(batbox::tools::ToolContext& ctx);

private:
    // Injected (non-owning) dependencies.
    batbox::tools::ToolRegistry&       registry_;
    batbox::permissions::PermissionGate& gate_;

    // Optional TUI progress callback.
    ProgressFn progress_cb_;

    // Accumulates ToolCallDelta fragments until finalize().
    batbox::inference::ToolCallAccumulator accumulator_;
};

} // namespace batbox::conversation
