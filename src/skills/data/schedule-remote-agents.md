---
name: schedule-remote-agents
description: Schedule and coordinate remote agent invocations using the RemoteTrigger tool and batbox cron scheduling
allowed_tools: [Read, Write, Bash]
---
# Schedule Remote Agents Skill

You are setting up, scheduling, or managing remote agent tasks using batbox's `RemoteTrigger` tool and its cron scheduling infrastructure. Remote agents allow batbox to trigger work on external agent endpoints — either on a recurring cron schedule or in response to events — without a human present at the keyboard.

This skill pairs with the `RemoteTrigger` tool, which handles the actual HTTP dispatch, and with `CronCreate` / `CronDelete` / `CronList` for persistent scheduling.

## What remote agent scheduling enables

- **Nightly builds or tests**: Run a build-and-test cycle every night without manual intervention.
- **Scheduled summaries**: Produce a daily code-review summary or changelog at a fixed time.
- **Event-driven workflows**: Trigger an agent run when a webhook fires (CI completion, PR opened, alert received).
- **Distributed work**: Coordinate multiple agent instances running on different machines or environments.

## Steps

1. **Understand the goal**: Read the user's request carefully. Determine:
   - What task the remote agent should perform
   - When it should run (one-shot, recurring, or event-driven)
   - Which agent endpoint it should target

2. **Define the task payload**: Write the exact task description the remote agent will receive. Be explicit — the remote agent has no access to the current conversation context. Include:
   - What to do
   - What success looks like
   - Any relevant file paths, branch names, or environment details

3. **Choose the scheduling method**:
   - **`CronCreate`**: for recurring schedules expressed as a standard 5-field cron expression (e.g., `0 2 * * *` for 2 AM daily)
   - **`RemoteTrigger`**: for immediate one-shot invocations or event-driven calls (called inline when the event arrives)
   - Both can be combined: `CronCreate` stores the schedule; the scheduler fires `RemoteTrigger` at the due time

4. **Configure the agent endpoint**: Identify:
   - Target URL (must match the RemoteTrigger allowlist pattern configured in `~/.batbox/settings.json`)
   - Authentication headers (Bearer token, API key, etc.)
   - Any timeout or retry settings

5. **Write the schedule or trigger configuration**:
   - For cron: provide the `CronCreate` call with expression, command/payload, and enabled flag
   - For one-shot: provide the `RemoteTrigger` call with URL, headers, and body

6. **Verify the configuration**:
   - Cron expression is valid (5 fields, correct ranges)
   - URL is present and matches the allowlist
   - Authentication is provided (never hardcode credentials — reference a config key or environment variable)
   - Payload is complete and self-contained

7. **Document the arrangement**: Produce a brief summary:
   - What was scheduled
   - When it runs (next fire time for cron entries)
   - What the remote agent will do
   - How to inspect it (`CronList`), pause it (`CronDelete` + recreate disabled), or cancel it (`CronDelete`)

## Security rules

- Apply the principle of least privilege: only include tools and permissions the remote agent genuinely needs for its task.
- Never embed credentials directly in the payload or cron entry. Reference `$ENV_VAR` or a named config key.
- Confirm the target URL is in the RemoteTrigger allowlist before attempting to fire.
- For sensitive workloads, prefer short-lived tokens that expire after the expected task duration.

## Example cron expressions

| Expression | Meaning |
|------------|---------|
| `0 2 * * *` | Every day at 02:00 |
| `0 9 * * 1` | Every Monday at 09:00 |
| `*/15 * * * *` | Every 15 minutes |
| `0 0 1 * *` | First day of each month at midnight |
