// src/conversation/ToolCallOrchestrator.cpp
// =============================================================================
// batbox::conversation::ToolCallOrchestrator implementation (CPP 3.4).
//
// See the header for the full class contract and decision flow.
// =============================================================================

#include "batbox/conversation/ToolCallOrchestrator.hpp"

#include <batbox/core/Logging.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <utility>

namespace batbox::conversation {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/// Build a Role::Tool Message from a call id, tool name, and ToolResult.
Message make_tool_message(const std::string&             call_id,
                          const std::string&             tool_name,
                          const batbox::tools::ToolResult& result) {
    Message msg;
    // id and ts are set by the default Message constructor (uuid_v4 + now).
    msg.role        = Role::Tool;
    msg.content     = result.body;
    msg.tool_call_id = call_id;
    msg.tool_name    = tool_name;
    msg.is_error     = result.is_error ? std::optional<bool>{true} : std::nullopt;
    return msg;
}

/// Build a Role::Tool error Message for non-dispatch failures (denied, parse error).
Message make_error_tool_message(const std::string& call_id,
                                const std::string& tool_name,
                                const std::string& error_body) {
    Message msg;
    msg.role         = Role::Tool;
    msg.content      = error_body;
    msg.tool_call_id = call_id;
    msg.tool_name    = tool_name;
    msg.is_error     = true;
    return msg;
}

} // anonymous namespace

// =============================================================================
// ToolCallOrchestrator — constructor
// =============================================================================

ToolCallOrchestrator::ToolCallOrchestrator(
        batbox::tools::ToolRegistry&         registry,
        batbox::permissions::PermissionGate& gate,
        ProgressFn                           progress_cb)
    : registry_(registry)
    , gate_(gate)
    , progress_cb_(std::move(progress_cb))
{
    // accumulator_ is default-constructed (empty, ready to receive deltas).
}

// =============================================================================
// accumulate — feed one ToolCallDelta to the internal accumulator
// =============================================================================

void ToolCallOrchestrator::accumulate(const batbox::inference::ToolCallDelta& delta) {
    accumulator_.accumulate(delta);
}

// =============================================================================
// dispatch_all — finalise, arbitrate, dispatch; return Tool messages
// =============================================================================

std::vector<Message>
ToolCallOrchestrator::dispatch_all(batbox::tools::ToolContext& ctx) {
    auto logger = batbox::log::get("orchestrator");

    // ------------------------------------------------------------------
    // 1. Finalise the accumulator: JSON-parse all argument buffers.
    //    The outer Result is always Ok (errors are per-call in parse_error).
    // ------------------------------------------------------------------
    auto finalize_result = accumulator_.finalize();
    // finalize() always returns Ok per the ToolCallAccumulator contract.
    const auto& calls = finalize_result.value();

    std::vector<Message> tool_messages;
    tool_messages.reserve(calls.size());

    // ------------------------------------------------------------------
    // 2. Process each assembled ToolCall.
    // ------------------------------------------------------------------
    for (const auto& call : calls) {
        // ------------------------------------------------------------------
        // 2a. Arguments parse error: surface as Tool error message so the
        //     model can self-correct.  No permission check needed.
        // ------------------------------------------------------------------
        if (!call.parse_error.empty()) {
            logger->warn("ToolCallOrchestrator: parse_error for tool '{}' (id={}): {}",
                         call.name, call.id, call.parse_error);

            const std::string body =
                "tool call arguments parse error: " + call.parse_error;
            tool_messages.push_back(
                make_error_tool_message(call.id, call.name, body));
            continue;
        }

        // ------------------------------------------------------------------
        // 2b. Cancellation check: stop processing remaining calls and return
        //     whatever messages have been assembled so far.
        // ------------------------------------------------------------------
        if (ctx.cancel_token.is_cancelled()) {
            logger->debug("ToolCallOrchestrator: cancelled before dispatching '{}'",
                          call.name);
            break;
        }

        // ------------------------------------------------------------------
        // 2c. Permission gate: ask whether this call is permitted.
        // ------------------------------------------------------------------
        logger->debug("ToolCallOrchestrator: asking permission for tool '{}'", call.name);
        const batbox::permissions::Decision decision =
            gate_.ask(call.name, call.arguments, ctx);

        if (decision.kind == batbox::permissions::Decision::Kind::Deny) {
            logger->info("ToolCallOrchestrator: tool '{}' denied by permission gate",
                         call.name);
            tool_messages.push_back(
                make_error_tool_message(call.id, call.name, "user denied tool call"));
            continue;
        }

        // Permission granted — persist any rule the gate handed back.
        if (decision.persist_rule.has_value()) {
            const auto& rule = *decision.persist_rule;
            if (rule.kind == batbox::permissions::PermissionRule::Kind::Allow) {
                auto persist_result = gate_.store().add_allow_rule(rule.pattern);
                if (!persist_result.has_value()) {
                    logger->warn("ToolCallOrchestrator: failed to persist allow rule '{}': {}",
                                 rule.pattern, persist_result.error());
                }
            } else if (rule.kind == batbox::permissions::PermissionRule::Kind::Deny) {
                auto persist_result = gate_.store().add_deny_rule(rule.pattern);
                if (!persist_result.has_value()) {
                    logger->warn("ToolCallOrchestrator: failed to persist deny rule '{}': {}",
                                 rule.pattern, persist_result.error());
                }
            }
        }

        // ------------------------------------------------------------------
        // 2d. Second cancellation check (after potentially blocking in gate).
        // ------------------------------------------------------------------
        if (ctx.cancel_token.is_cancelled()) {
            logger->debug("ToolCallOrchestrator: cancelled after permission gate for '{}'",
                          call.name);
            break;
        }

        // ------------------------------------------------------------------
        // 2e. Progress callback (TUI status line update).
        // ------------------------------------------------------------------
        if (progress_cb_) {
            progress_cb_(call.name, call.arguments);
        }

        // ------------------------------------------------------------------
        // 2f. Dispatch the tool.
        // ------------------------------------------------------------------
        logger->debug("ToolCallOrchestrator: dispatching tool '{}'", call.name);
        auto dispatch_result = registry_.dispatch(call.name, call.arguments, ctx);

        if (!dispatch_result.has_value()) {
            // Registry returned an Err (unknown tool, plan-mode block, etc.)
            const std::string& err_str = dispatch_result.error();
            logger->warn("ToolCallOrchestrator: dispatch error for tool '{}': {}",
                         call.name, err_str);
            tool_messages.push_back(
                make_error_tool_message(call.id, call.name, err_str));
        } else {
            // Successful dispatch — ToolResult may itself carry is_error=true
            // when the tool encountered a recoverable error (file not found, etc.)
            const batbox::tools::ToolResult& tr = dispatch_result.value();
            logger->debug("ToolCallOrchestrator: tool '{}' returned is_error={}",
                          call.name, tr.is_error);
            tool_messages.push_back(make_tool_message(call.id, call.name, tr));
        }
    }

    return tool_messages;
}

} // namespace batbox::conversation
