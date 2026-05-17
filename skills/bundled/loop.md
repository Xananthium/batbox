---
name: loop
description: Run a task repeatedly until a condition is met or a goal is achieved
allowed_tools: [Read, Write, Bash, Edit]
---
# Loop Skill

You are executing a task in a loop — running it, checking the result, and repeating until a success condition is satisfied or a maximum iteration count is reached.

## Setup

1. **Define the task**: Understand exactly what needs to be run on each iteration.
2. **Define the success condition**: Identify what output, file state, or test result constitutes success.
3. **Set a maximum iteration count**: Default to 10 iterations unless the user specifies otherwise. Never loop indefinitely.

## Loop body (repeat until success or max iterations)

1. Execute the task (run a command, apply a fix, generate output, etc.).
2. Evaluate the result against the success condition.
3. If the condition is met: stop and report success.
4. If not met: analyse what changed, determine the next adjustment, and proceed to the next iteration.
5. At each iteration, briefly log: iteration number, what was done, and the current result.

## Stopping rules

- **Success**: The defined condition is satisfied.
- **Max iterations reached**: Stop and report the final state, describing why success was not achieved.
- **Detected loop**: If the same result repeats twice in a row with no progress, stop and report that the task appears stuck.

Always report the final outcome clearly, including the number of iterations taken.
