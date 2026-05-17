---
name: schedule-remote-agents
description: Schedule and coordinate remote agent tasks using RemoteTrigger
allowed_tools: [Read, Write]
---
# Schedule Remote Agents Skill

You are setting up, scheduling, or coordinating remote agent tasks using batbox's RemoteTrigger tool and cron scheduling capabilities.

## What this enables

Remote agents allow batbox tasks to be triggered by external HTTP calls or run on a cron schedule without a human present. This is useful for:
- Automated nightly builds or tests
- Scheduled code reviews or summaries
- Event-driven workflows triggered by webhooks
- Coordinating multiple agent instances across machines

## Steps

1. **Understand the goal**: Read the user's request to determine what should be scheduled, when, and on which agent endpoint.

2. **Define the task payload**: Determine the exact task description and context the remote agent will receive when triggered.

3. **Choose the scheduling method**:
   - **CronCreate**: for recurring schedules (e.g., every night at 2 AM)
   - **RemoteTrigger**: for event-driven or one-shot remote invocations

4. **Configure the agent endpoint**: Identify the target URL, any required authentication headers, and the allowed URL pattern (for RemoteTrigger's allowlist).

5. **Write the schedule or trigger configuration**: Produce the configuration needed to set up the schedule.

6. **Verify the setup**: Confirm all required fields are present (URL, auth, payload, schedule expression).

7. **Document the arrangement**: Write a brief summary of what was scheduled, when it runs, what it does, and how to cancel or modify it.

Always apply the principle of least privilege — only include the tools and permissions the remote agent genuinely needs.
