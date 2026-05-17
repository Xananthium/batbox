---
name: loop
description: Run an iterative refinement loop — execute, evaluate, adjust — until success or a step limit is reached
allowed_tools: [Read, Write, Edit, Bash]
---
# Loop Skill

You are executing a task in a self-paced iterative loop. Each iteration runs the task, checks whether the goal was achieved, and — if not — adjusts and tries again. You repeat until the success condition is satisfied or the maximum iteration count is exhausted.

This skill is the runtime companion to the `/loop` slash command, which pre-configures the task and success condition before handing off to you.

## Setup (do once before the first iteration)

1. **Identify the task**: Understand exactly what must be executed on each iteration (a command to run, a fix to apply, a file to generate, etc.).
2. **Define the success condition**: Determine what output, file state, exit code, or test result means "done". Be concrete and measurable.
3. **Set the iteration limit**: Default is **10 iterations**. Use the user-supplied limit if one was provided. Never loop indefinitely.
4. **Record the baseline**: Note the current state before iteration 1 so you can detect whether progress is being made.

## Loop body (repeat until success or limit)

For each iteration:

1. Execute the task (run the command, apply the edit, generate the content, etc.).
2. Capture the result (exit code, output, file diff, test report).
3. Evaluate the result against the success condition.
   - **Condition met** → stop. Go to the "Success" report below.
   - **Condition not met** → continue.
4. Analyse what changed versus the previous iteration. Identify the specific obstacle blocking success.
5. Determine one concrete adjustment for the next iteration.
6. Log a one-line entry: `[Iter N/MAX] <what was done> → <result> | next: <adjustment>`.

## Stopping rules

| Condition | Action |
|-----------|--------|
| Success condition met | Stop and report success |
| Iteration limit reached | Stop and report final state + reason not achieved |
| Same result two iterations in a row | Stop — task appears stuck; report and ask for guidance |
| Fatal error (no recovery path) | Stop immediately and report the blocker |

## Reports

**On success:**
```
Loop complete — success on iteration N/MAX.
Condition: <success condition>
Result: <final output or state>
```

**On exhaustion or stuck:**
```
Loop ended after N iterations — goal not achieved.
Last result: <output>
Obstacle: <what is blocking success>
Recommendation: <suggested next step for the user>
```

Always be explicit about how many iterations were used and why the loop ended.
