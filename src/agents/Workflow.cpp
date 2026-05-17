// src/agents/Workflow.cpp
//
// batbox::agents::Workflow — DAG-based multi-agent workflow executor.
//
// Algorithm overview:
//
//   1.  validate()      – detect duplicate step names; return Err immediately.
//
//   2.  topo_sort()     – Kahn's BFS topological sort.
//                         Builds an in-degree map + adjacency list from
//                         each step's `depends_on` vector.
//                         Cycle detection: if processed_count < total steps
//                         after BFS, a cycle exists → return Err.
//
//   3.  execute()       – iterative wave execution:
//                           a. Identify "ready" steps: all depends_on names
//                              are in the completed-outputs map.
//                           b. For each ready step, substitute
//                              {{dep_name.output}} in its prompt, then call
//                              supervisor.spawn().
//                           c. Wait (blocking poll loop with short sleep) for
//                              all in-flight agents to finish; capture their
//                              final output via snapshot().
//                           d. Repeat until all steps are done or a failure
//                              stops execution (StopOnFirst policy).
//
//   4.  output_of()     – once an agent finishes the last line of its
//                         last_5_lines snapshot is used as the captured output
//                         (or the full status string when last_5_lines is empty).
//
// Output capture strategy:
//   AgentSupervisor::snapshot() returns AgentSnapshot objects.  A step's
//   output is captured as the snapshot's last_5_lines joined by "\n" when the
//   status becomes "completed".  On "errored" or "cancelled" we capture the
//   status field and treat it as a failure.
//
// Blueprint contract:
//   struct  batbox::agents::WorkflowStep   (blueprints row 16774)
//   class   batbox::agents::Workflow       (blueprints row 16775)
//   file    src/agents/Workflow.cpp        (blueprints row 16776)

#include <batbox/agents/Workflow.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/CancelToken.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace batbox::agents {

// =============================================================================
// Workflow — construction
// =============================================================================

Workflow::Workflow(FailurePolicy policy)
    : policy_(policy)
{}

// =============================================================================
// add_step
// =============================================================================

void Workflow::add_step(WorkflowStep step) {
    steps_.push_back(std::move(step));
}

void Workflow::add_step(std::string              name,
                        std::string              agent_name,
                        std::string              prompt,
                        std::vector<std::string> depends_on)
{
    steps_.push_back(WorkflowStep{
        std::move(name),
        std::move(agent_name),
        std::move(prompt),
        std::move(depends_on)
    });
}

// =============================================================================
// Accessors
// =============================================================================

const std::vector<WorkflowStep>& Workflow::steps() const noexcept {
    return steps_;
}

FailurePolicy Workflow::failure_policy() const noexcept {
    return policy_;
}

void Workflow::set_failure_policy(FailurePolicy policy) noexcept {
    policy_ = policy;
}

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// validate_unique_names
// Check that all step names are unique.
// ---------------------------------------------------------------------------
[[nodiscard]] static std::string validate_unique_names(
    const std::vector<WorkflowStep>& steps)
{
    std::unordered_set<std::string> seen;
    for (const auto& s : steps) {
        if (s.name.empty()) {
            return "workflow step has an empty name";
        }
        if (!seen.insert(s.name).second) {
            return "duplicate step name: \"" + s.name + "\"";
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_deps_exist
// Ensure every name in depends_on refers to an existing step.
// ---------------------------------------------------------------------------
[[nodiscard]] static std::string validate_deps_exist(
    const std::vector<WorkflowStep>& steps)
{
    std::unordered_set<std::string> names;
    for (const auto& s : steps) names.insert(s.name);

    for (const auto& s : steps) {
        for (const auto& dep : s.depends_on) {
            if (names.find(dep) == names.end()) {
                return "step \"" + s.name + "\" depends on unknown step \"" + dep + "\"";
            }
            if (dep == s.name) {
                return "step \"" + s.name + "\" depends on itself";
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// topo_sort_check
// Kahn's algorithm: returns an error string if a cycle exists, or empty if
// the DAG is valid.  Also returns the topological processing order (unused
// here but helpful for debugging).
// ---------------------------------------------------------------------------
[[nodiscard]] static std::string topo_sort_check(
    const std::vector<WorkflowStep>& steps)
{
    const std::size_t n = steps.size();
    if (n == 0) return {};

    // Map step name → index for fast lookup.
    std::unordered_map<std::string, std::size_t> idx;
    idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        idx[steps[i].name] = i;
    }

    // in_degree[i] = number of unresolved dependencies for step i.
    std::vector<std::size_t> in_degree(n, 0);
    for (const auto& s : steps) {
        in_degree[idx[s.name]] += s.depends_on.size();
    }

    // Start with all nodes that have no dependencies.
    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) ready.push(i);
    }

    std::size_t processed = 0;
    while (!ready.empty()) {
        const std::size_t cur = ready.front();
        ready.pop();
        ++processed;

        // Decrement in-degree of every node that lists cur as a dependency.
        const std::string& cur_name = steps[cur].name;
        for (std::size_t j = 0; j < n; ++j) {
            const auto& deps = steps[j].depends_on;
            if (std::find(deps.begin(), deps.end(), cur_name) != deps.end()) {
                --in_degree[j];
                if (in_degree[j] == 0) {
                    ready.push(j);
                }
            }
        }
    }

    if (processed < n) {
        // Collect the names of steps in the cycle.
        std::ostringstream oss;
        oss << "cycle detected in workflow DAG involving steps: [";
        bool first = true;
        for (std::size_t i = 0; i < n; ++i) {
            if (in_degree[i] > 0) {
                if (!first) oss << ", ";
                oss << "\"" << steps[i].name << "\"";
                first = false;
            }
        }
        oss << "]";
        return oss.str();
    }

    return {};
}

// ---------------------------------------------------------------------------
// substitute_placeholders
// Replace every {{dep_name.output}} token in `prompt` with the captured
// output of the named step (from the completed-outputs map).
// ---------------------------------------------------------------------------
[[nodiscard]] static std::string substitute_placeholders(
    std::string                                prompt,
    const std::map<std::string, std::string>&  outputs)
{
    // Iterate over all outputs and replace any matching token.
    for (const auto& [step_name, step_output] : outputs) {
        const std::string token = "{{" + step_name + ".output}}";
        std::string::size_type pos = 0;
        while ((pos = prompt.find(token, pos)) != std::string::npos) {
            prompt.replace(pos, token.size(), step_output);
            pos += step_output.size();
        }
    }
    return prompt;
}

// ---------------------------------------------------------------------------
// capture_output
// Extract a human-readable output string from an AgentSnapshot.
// ---------------------------------------------------------------------------
[[nodiscard]] static std::string capture_output(const AgentSnapshot& snap) {
    if (!snap.last_5_lines.empty()) {
        std::ostringstream oss;
        bool first = true;
        for (const auto& line : snap.last_5_lines) {
            if (!first) oss << "\n";
            oss << line;
            first = false;
        }
        return oss.str();
    }
    // Fallback: return the status string so downstream steps get something.
    return snap.status;
}

// ---------------------------------------------------------------------------
// is_terminal
// Returns true when an agent has finished (successfully or otherwise).
// ---------------------------------------------------------------------------
[[nodiscard]] static bool is_terminal(const std::string& status) {
    return status == "completed" || status == "errored" || status == "cancelled";
}

// ---------------------------------------------------------------------------
// is_failed
// Returns true when an agent terminated unsuccessfully.
// ---------------------------------------------------------------------------
[[nodiscard]] static bool is_failed(const std::string& status) {
    return status == "errored" || status == "cancelled";
}

} // anonymous namespace

// =============================================================================
// Workflow::execute
// =============================================================================

Result<std::map<std::string, std::string>, std::string>
Workflow::execute(AgentSupervisor& supervisor, CancelToken ct)
{
    // -------------------------------------------------------------------------
    // 0.  Empty workflow is a valid no-op.
    // -------------------------------------------------------------------------
    if (steps_.empty()) {
        return std::map<std::string, std::string>{};
    }

    // -------------------------------------------------------------------------
    // 1.  Structural validation.
    // -------------------------------------------------------------------------
    {
        const std::string dup_err = validate_unique_names(steps_);
        if (!dup_err.empty()) {
            return batbox::Unexpected<std::string>(dup_err);
        }
    }
    {
        const std::string dep_err = validate_deps_exist(steps_);
        if (!dep_err.empty()) {
            return batbox::Unexpected<std::string>(dep_err);
        }
    }

    // -------------------------------------------------------------------------
    // 2.  Cycle detection (Kahn's algorithm).
    // -------------------------------------------------------------------------
    {
        const std::string cycle_err = topo_sort_check(steps_);
        if (!cycle_err.empty()) {
            return batbox::Unexpected<std::string>(cycle_err);
        }
    }

    // -------------------------------------------------------------------------
    // 3.  Iterative wave execution.
    //
    //     State:
    //       completed_outputs  – step_name → captured output string
    //       failed_steps       – names of steps that errored/cancelled
    //       agent_to_step      – agent_id → step_name (for snapshot lookup)
    //       pending_agents     – agent_id → step_name (in-flight agents)
    // -------------------------------------------------------------------------

    std::map<std::string, std::string>   completed_outputs;
    std::vector<std::string>             failed_steps;
    std::map<std::string, std::string>   agent_to_step; // agent_id → step_name
    std::set<std::string>                launched;      // step names we've started

    const std::size_t total_steps = steps_.size();

    // Main loop: keep iterating until every step has either completed or failed.
    while (completed_outputs.size() + failed_steps.size() < total_steps) {

        // Check for parent-level cancellation.
        if (ct.stop_requested()) {
            return batbox::Unexpected<std::string>(std::string("workflow cancelled by caller"));
        }

        // ---- A.  Find and launch all newly-ready steps ----------------------

        bool launched_any = false;
        for (const auto& step : steps_) {
            // Already launched or already failed — skip.
            if (launched.count(step.name)) continue;

            // With StopOnFirst, don't launch new steps if we already have failures.
            if (policy_ == FailurePolicy::StopOnFirst && !failed_steps.empty()) {
                // Mark this step as "skipped" so the loop can terminate.
                // We track it as failed so progress advances.
                failed_steps.push_back(step.name);
                launched.insert(step.name);
                continue;
            }

            // Check all dependencies have completed successfully.
            bool deps_met = true;
            for (const auto& dep : step.depends_on) {
                if (completed_outputs.find(dep) == completed_outputs.end()) {
                    // Dependency not in completed outputs.
                    // With ContinueAll, if the dep failed we skip this step too.
                    if (policy_ == FailurePolicy::ContinueAll) {
                        bool dep_failed = std::find(failed_steps.begin(),
                                                    failed_steps.end(),
                                                    dep) != failed_steps.end();
                        if (dep_failed) {
                            // Skip: a required dep failed; mark this step failed.
                            failed_steps.push_back(step.name);
                            launched.insert(step.name);
                            deps_met = false;
                            break;
                        }
                    }
                    deps_met = false;
                    break;
                }
            }

            if (!deps_met) continue;

            // All deps satisfied — substitute placeholders and spawn.
            const std::string resolved_prompt =
                substitute_placeholders(step.prompt, completed_outputs);

            AgentSpec spec = AgentSpec::from_type(step.agent_name);
            spec.name      = step.name; // Use the step name as the display name.

            // Create a child token linked to the workflow's cancel token.
            // This allows the individual agent to be cancelled by the workflow
            // while ct itself remains alive for the duration of the loop.
            auto [child_src, child_tok] = ct.child();
            (void)child_src; // lifetime managed by the spawned agent thread

            const std::string agent_id =
                supervisor.spawn(spec, resolved_prompt, /*parent_id=*/"", std::move(child_tok));

            agent_to_step[agent_id] = step.name;
            launched.insert(step.name);
            launched_any = true;
        }

        // If nothing new was launched and no agents are in-flight, we're stuck.
        // This should not happen after cycle detection — but guard defensively.
        const bool agents_in_flight = !agent_to_step.empty();
        if (!launched_any && !agents_in_flight) {
            // Any un-launched steps must be stuck because of failed deps.
            // Collect them into failed_steps so the while condition terminates.
            for (const auto& step : steps_) {
                if (!launched.count(step.name)) {
                    failed_steps.push_back(step.name);
                    launched.insert(step.name);
                }
            }
            break;
        }

        // ---- B.  Poll in-flight agents for completion ----------------------

        if (!agent_to_step.empty()) {
            // Take a snapshot and check which agents have finished.
            const std::vector<AgentSnapshot> snaps = supervisor.snapshot();

            // Build a map from agent_id → snapshot for fast lookup.
            std::unordered_map<std::string, const AgentSnapshot*> snap_map;
            snap_map.reserve(snaps.size());
            for (const auto& snap : snaps) {
                snap_map[snap.id] = &snap;
            }

            std::vector<std::string> finished_agent_ids;
            for (const auto& [agent_id, step_name] : agent_to_step) {
                auto it = snap_map.find(agent_id);
                if (it == snap_map.end()) {
                    // Snapshot not yet available; agent may still be starting.
                    continue;
                }
                const AgentSnapshot& snap = *(it->second);
                if (!is_terminal(snap.status)) {
                    continue; // Still running.
                }

                finished_agent_ids.push_back(agent_id);

                if (is_failed(snap.status)) {
                    failed_steps.push_back(step_name);
                } else {
                    completed_outputs[step_name] = capture_output(snap);
                }
            }

            // Remove finished agents from the in-flight tracking map.
            for (const auto& aid : finished_agent_ids) {
                agent_to_step.erase(aid);
            }
        }

        // ---- C.  Yield the CPU briefly before the next poll ----------------
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // 4.  Return result.
    // -------------------------------------------------------------------------

    if (failed_steps.empty()) {
        return completed_outputs;
    }

    // Build a descriptive error message listing all failed steps.
    std::ostringstream err;
    err << "workflow failed; offending step(s): [";
    bool first = true;
    for (const auto& name : failed_steps) {
        if (!first) err << ", ";
        err << "\"" << name << "\"";
        first = false;
    }
    err << "]";

    return batbox::Unexpected<std::string>(err.str());
}

} // namespace batbox::agents
