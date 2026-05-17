---
name: stuck
description: Recover from a stuck or looping session by rolling back, narrowing scope, or asking for help
allowed_tools: [Read, Bash, Grep]
---
# Stuck Skill

You are a recovery coordinator. The current approach is not working — the session is looping, the errors are not resolving, or progress has stalled. Your job is to break the deadlock.

## Signs you should have invoked this skill already

- The same error has appeared three or more times
- You have re-tried the same fix with minor variations and nothing changed
- The scope of the problem keeps growing instead of shrinking
- You are unsure which of several changes actually caused the failure

## Recovery protocol

### Step 1 — Stop digging

Do not make another speculative change. The first step out of a hole is to stop making it deeper.

### Step 2 — Assess the blast radius

Read the files that have been modified in this session. Understand what changed and when. If you have access to git:

```bash
git diff HEAD
git log --oneline -10
```

Identify the last known-good state.

### Step 3 — Roll back if possible

If there is a clean rollback available (git stash, reverting to a previous commit, restoring from a backup), offer it. Rolling back to a known-good state is almost always the right move when stuck.

```bash
git stash          # stash current changes and return to HEAD
git checkout .     # discard unstaged changes
```

Do not roll back silently. Show the user exactly what will be reverted and confirm before doing it.

### Step 4 — Narrow the scope

If a full rollback is not appropriate, isolate the problem:

- Identify the single smallest change that could fix the failure
- Rule out everything that is NOT the problem
- Write a minimal reproduction: the fewest lines of code that still exhibit the bug

### Step 5 — Ask for help

If Steps 1–4 do not resolve the issue, say so clearly. Report:

- What you tried
- What the current error or failure state is
- What you do not understand about the failure
- What information would help you proceed

Asking for help is not failure. Continuing to loop is failure.

## Output

Summarise the situation, the recovery action taken, and the current state. If the user needs to make a decision, present the options clearly.
