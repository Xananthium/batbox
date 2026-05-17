// include/batbox/agents/Workflow.hpp
//
// batbox::agents::WorkflowStep + batbox::agents::Workflow
//
// DAG-based multi-agent workflow:
//   - WorkflowStep: a named step with an agent_name, a prompt, and an optional
//     list of step names it depends on.
//   - Workflow: holds a vector<WorkflowStep>; execute() runs them in topological
//     order, fanning out independent steps in parallel through
//     AgentSupervisor (still capped by its 4-slot semaphore).
//
// Topological sort:
//   Kahn's algorithm (BFS-based).  Cycle detection is performed before any
//   agents are spawned; if a cycle is found execute() returns an error string
//   immediately.
//
// Output piping:
//   Each step's prompt may contain {{step_name.output}} placeholders.
//   Before a step is launched, the placeholders are replaced with the captured
//   output of the named (already-completed) dependency step.
//
// Failure policy:
//   Controlled by FailurePolicy enum:
//     StopOnFirst  – cancel all pending/running steps and return an error as
//                    soon as any step fails.
//     ContinueAll  – run every step regardless; collect all errors and return
//                    them in the error string after all steps have finished.
//
// Return value:
//   execute() returns Result<std::map<std::string,std::string>, std::string>
//   On success: map of step_name → output string.
//   On failure: error message that names the offending step(s).
//
// Blueprint contract: blueprints rows 16773–16776  (task CPP 6.7)
//   struct  batbox::agents::WorkflowStep
//   class   batbox::agents::Workflow
//
// =============================================================================

#pragma once

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>

#include <map>
#include <string>
#include <vector>

namespace batbox::agents {

// =============================================================================
// WorkflowStep — one node in the agent DAG
// =============================================================================

/// A single step in a Workflow.
///
/// `agent_name` is the sub-agent type to launch (passed to AgentSpec::from_type
/// if no .md file exists for the name).
///
/// `depends_on` lists the names of steps that must complete before this step
/// starts.  Empty = no dependencies (runs in the first wave).
///
/// `prompt` is the text delivered as the initial user-turn to the spawned agent.
/// It may contain {{step_name.output}} tokens which are resolved to the captured
/// output of the named completed steps before the agent is started.
///
/// Blueprint contract: batbox::agents::WorkflowStep (blueprints row 16774)
struct WorkflowStep {
    /// Unique name for this step within the workflow (used in depends_on and
    /// {{step_name.output}} substitution).
    std::string name;

    /// Sub-agent type / display name.  Maps to AgentSpec::from_type(agent_name).
    std::string agent_name;

    /// Initial prompt delivered to the spawned agent.
    /// May contain {{step_name.output}} placeholders.
    std::string prompt;

    /// Names of steps that this step depends on.  All must complete
    /// successfully before this step may launch (StopOnFirst policy) or, with
    /// ContinueAll, before successful dependencies' outputs are substituted.
    std::vector<std::string> depends_on;
};

// =============================================================================
// FailurePolicy — controls step-failure behaviour
// =============================================================================

enum class FailurePolicy {
    /// Stop the workflow as soon as any step fails; cancel remaining steps.
    StopOnFirst,

    /// Continue executing all independent steps even when one fails; collect
    /// all errors and surface them in the final error string.
    ContinueAll,
};

// =============================================================================
// Workflow — DAG executor
// =============================================================================

/// Holds a directed-acyclic graph of WorkflowSteps and executes them through
/// an AgentSupervisor.
///
/// Typical use:
/// @code
///   Workflow wf;
///   wf.add_step({"fetch",   "fetcher-agent",  "Fetch https://example.com"});
///   wf.add_step({"analyse", "analyst-agent",  "Analyse: {{fetch.output}}",
///                {"fetch"}});
///   auto result = wf.execute(supervisor, ct);
/// @endcode
///
/// Blueprint contract: batbox::agents::Workflow (blueprints row 16775)
class Workflow {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct an empty workflow.  Failure policy defaults to StopOnFirst.
    Workflow() = default;

    explicit Workflow(FailurePolicy policy);

    // -------------------------------------------------------------------------
    // Step registration
    // -------------------------------------------------------------------------

    /// Append a step to the workflow.  Step names must be unique; adding a
    /// duplicate name causes execute() to return an error.
    ///
    /// @param step  The WorkflowStep to add.
    void add_step(WorkflowStep step);

    /// Convenience: construct a WorkflowStep in-place from its components.
    void add_step(std::string name,
                  std::string agent_name,
                  std::string prompt,
                  std::vector<std::string> depends_on = {});

    // -------------------------------------------------------------------------
    // execute — run the DAG
    // -------------------------------------------------------------------------

    /// Execute all steps in topological order through `supervisor`.
    ///
    /// Steps whose dependencies have all completed are fanned out in parallel;
    /// the supervisor's 4-slot semaphore still applies.
    ///
    /// Cycle detection runs before any spawn call.  If a cycle exists the
    /// method returns immediately with an error string naming the cycle.
    ///
    /// @param supervisor  AgentSupervisor used to spawn each step's agent.
    /// @param ct          CancelToken; cancelling it propagates to all agents.
    ///
    /// @returns
    ///   Ok  : map<step_name, output> for every step that ran successfully.
    ///   Err : human-readable error string naming the offending step(s) or cycle.
    ///
    /// Blueprint contract: Workflow::execute (blueprints row 16775)
    [[nodiscard]] Result<std::map<std::string, std::string>, std::string>
    execute(AgentSupervisor& supervisor, CancelToken ct);

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /// Return a const reference to the registered steps (for inspection / test).
    [[nodiscard]] const std::vector<WorkflowStep>& steps() const noexcept;

    /// Return the current failure policy.
    [[nodiscard]] FailurePolicy failure_policy() const noexcept;

    /// Set the failure policy (must be called before execute()).
    void set_failure_policy(FailurePolicy policy) noexcept;

private:
    std::vector<WorkflowStep> steps_;
    FailurePolicy             policy_{FailurePolicy::StopOnFirst};
};

} // namespace batbox::agents
