// include/batbox/tools/ToolSubagentEnvelope.hpp
//
// batbox::tools::ToolSubagentEnvelope — the universal subagent-dispatch seam.
//
// batbox's central design commitment (Cass, project_batbox.md): every tool —
// even `grep` — is a subagent.  A tool engulfs its work in its own window,
// reports back distilled gold, and the harness chooses closed vs standing.
// The architectural precondition for that is a single chokepoint through which
// EVERY tool result flows, with no tool able to bypass it.
//
// batbox already has that chokepoint: ToolRegistry::dispatch() is the one place
// ITool::run() is ever invoked (native tools and McpTool alike — McpTool is just
// another ITool, so there is no separate MCP path).  This file makes the
// chokepoint a *subagent boundary*: dispatch routes the ToolResult produced by
// run() through a ToolSubagentEnvelope before returning it.  Because run() is
// only reachable through dispatch, and dispatch's run-path always returns the
// envelope's output, there is structurally no un-wrapped path from a tool call
// to a Tool message.
//
// The envelope owns two pluggable extension points, each a small interface with
// a trivial S7 default.  S7 ships the SEAM, not the mechanism — both defaults
// are pass-through, so behavior is byte-identical to pre-S7.  Later organs fill
// the hooks WITHOUT re-touching this seam:
//
//   IEngulfDecider  (decision hook) — "does this result need to be engulfed into
//                    a subagent (too big / an investigation)?"  This is where
//                    S1's threshold trigger (goose's large_response_handler /
//                    200k-char gate) will live.  S7 default: always false.
//
//   IResultDistiller (distiller hook) — "engulf this result in a subagent window
//                    on a local model, distill to the golden line, return the
//                    distilled ToolResult."  This is where S4's report_gold
//                    mechanism will live.  S7 default: identity.
//
// Composition:  process(result) := should_engulf(result)
//                                    ? distill(result)   // engulf → distill
//                                    : result;           // pass-through
//
// Recognized cross-cutting stages already at the dispatch seam (the envelope
// JOINS these; it does NOT replace them — see ToolRegistry::dispatch):
//   1. allowed_tools gate     — ctx.tool_is_allowed(name); rejects before run().
//   2. plan-mode gate         — is_plan_mode() && !is_read_only(); rejects
//                               non-read-only tools in Plan mode before run().
//   3. exception error-wrap   — run() throwing is caught and converted to
//                               ToolResult::error so the model self-corrects.
//   (The permission gate and cancellation checks live one layer up, in
//    ToolCallOrchestrator::dispatch_all, and are likewise unchanged.)
// Stages 1 and 2 short-circuit BEFORE any result exists, so they never traverse
// the envelope.  Once run() is invoked, its result — whether returned normally
// or synthesized from a thrown exception — passes through the envelope exactly
// once.

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <memory>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// IEngulfDecider — decision hook
// =============================================================================
//
// "Should this tool result be engulfed into a subagent?"  Pure-virtual so S1
// can drop in a real threshold trigger without touching the envelope or the
// registry.  Implementations MUST be cheap and side-effect free — they run on
// every dispatched tool result, on the dispatch thread.
class IEngulfDecider {
public:
    virtual ~IEngulfDecider() = default;

    /// @param tool_name  Canonical name of the tool that produced the result.
    /// @param args       The parsed JSON arguments the tool was dispatched with.
    /// @param result     The ToolResult returned by ITool::run().
    /// @param ctx        Per-dispatch context (read-only here).
    /// @returns true  → the result should be engulfed (distiller is invoked).
    ///          false → pass the result through unchanged.
    [[nodiscard]] virtual bool should_engulf(std::string_view  tool_name,
                                             const Json&        args,
                                             const ToolResult&  result,
                                             const ToolContext& ctx) const = 0;

protected:
    IEngulfDecider() = default;
};

// =============================================================================
// IResultDistiller — distiller hook
// =============================================================================
//
// "Engulf this result in a subagent window on a local model and distill it to
// the golden line."  Pure-virtual so S4 can drop in the real report_gold
// mechanism without touching the envelope or the registry.  Only invoked when
// the decider returns true.
class IResultDistiller {
public:
    virtual ~IResultDistiller() = default;

    /// @param tool_name  Canonical name of the tool that produced the result.
    /// @param args       The parsed JSON arguments the tool was dispatched with.
    /// @param result     The ToolResult to distill (taken by value; move from it).
    /// @param ctx        Per-dispatch context (mutable — a real distiller spawns
    ///                   a subagent and needs the cancel token / identity).
    /// @returns the distilled ToolResult that the model will actually see.
    [[nodiscard]] virtual ToolResult distill(std::string_view tool_name,
                                             const Json&      args,
                                             ToolResult       result,
                                             ToolContext&     ctx) const = 0;

protected:
    IResultDistiller() = default;
};

// =============================================================================
// PassThroughDecider — S7 default decision hook
// =============================================================================
//
// Never engulfs.  With this default the distiller is never invoked, so the
// envelope is a no-op and dispatch behaves byte-identically to pre-S7.
class PassThroughDecider final : public IEngulfDecider {
public:
    [[nodiscard]] bool should_engulf(std::string_view  /*tool_name*/,
                                     const Json&        /*args*/,
                                     const ToolResult&  /*result*/,
                                     const ToolContext& /*ctx*/) const override {
        return false;
    }
};

// =============================================================================
// IdentityDistiller — S7 default distiller hook
// =============================================================================
//
// Returns the result unchanged.  Present so the envelope always has a valid
// distiller to call (defensive); with the PassThroughDecider it is never
// reached on the default path.
class IdentityDistiller final : public IResultDistiller {
public:
    [[nodiscard]] ToolResult distill(std::string_view /*tool_name*/,
                                     const Json&      /*args*/,
                                     ToolResult       result,
                                     ToolContext&     /*ctx*/) const override {
        return result;
    }
};

// =============================================================================
// ToolSubagentEnvelope
// =============================================================================
//
// Interposes at the dispatch chokepoint.  Default-constructed with the
// pass-through decider and identity distiller (S7 behavior = no change).  Hooks
// are swappable at runtime via set_decider/set_distiller so S1+S4 can fill them
// without re-touching the seam.
//
// Ownership: hooks are held by shared_ptr.  Never null after construction (the
// constructors install the defaults; the setters reject null).
//
// Thread-safety: process() is const and only reads the held hooks; it is safe
// to call concurrently PROVIDED the hooks are not swapped concurrently.  As with
// the rest of ToolRegistry, hook installation is a startup-phase operation done
// before concurrent dispatch begins.
class ToolSubagentEnvelope {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Default: PassThroughDecider + IdentityDistiller (pure pass-through).
    ToolSubagentEnvelope();

    /// Inject custom hooks at construction.  A null argument falls back to the
    /// corresponding default (never leaves a null hook installed).
    ToolSubagentEnvelope(std::shared_ptr<IEngulfDecider>   decider,
                         std::shared_ptr<IResultDistiller> distiller);

    // Copyable and movable (just two shared_ptrs).
    ToolSubagentEnvelope(const ToolSubagentEnvelope&)            = default;
    ToolSubagentEnvelope& operator=(const ToolSubagentEnvelope&) = default;
    ToolSubagentEnvelope(ToolSubagentEnvelope&&)                 = default;
    ToolSubagentEnvelope& operator=(ToolSubagentEnvelope&&)      = default;
    ~ToolSubagentEnvelope()                                      = default;

    // -------------------------------------------------------------------------
    // process — the single seam every dispatched tool result flows through.
    // -------------------------------------------------------------------------
    //
    // Applies the decision hook; if it returns true, applies the distiller hook
    // and returns its output; otherwise returns the result unchanged.
    //
    // @param tool_name  Tool that produced the result.
    // @param args       The dispatched arguments (forwarded to both hooks).
    // @param result     The ToolResult from ITool::run() (moved into the hooks).
    // @param ctx        Per-dispatch context.
    [[nodiscard]] ToolResult process(std::string_view tool_name,
                                     const Json&      args,
                                     ToolResult       result,
                                     ToolContext&     ctx) const;

    // -------------------------------------------------------------------------
    // Hook installation — swappable so S1+S4 fill the seam without changing it.
    // -------------------------------------------------------------------------

    /// Install a decision hook.  A null argument is ignored (the current hook is
    /// kept) to preserve the never-null invariant.
    void set_decider(std::shared_ptr<IEngulfDecider> decider);

    /// Install a distiller hook.  A null argument is ignored (the current hook
    /// is kept) to preserve the never-null invariant.
    void set_distiller(std::shared_ptr<IResultDistiller> distiller);

    /// Accessors (non-owning view of the held hooks; never null).
    [[nodiscard]] const IEngulfDecider&   decider()   const noexcept { return *decider_; }
    [[nodiscard]] const IResultDistiller& distiller() const noexcept { return *distiller_; }

private:
    std::shared_ptr<IEngulfDecider>   decider_;
    std::shared_ptr<IResultDistiller> distiller_;
};

} // namespace batbox::tools
