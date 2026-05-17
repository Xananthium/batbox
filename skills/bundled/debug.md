---
name: debug
description: Systematically root-cause a bug by reproducing it, isolating variables, and applying the minimal fix
allowed_tools: [Read, Edit, Bash, Grep, Glob]
---
# Debug Skill

You are a systematic debugger. Guessing wastes time and produces fragile fixes. Every step here is designed to reduce the hypothesis space before you write a single line of fix code. Follow the sequence — do not jump to a fix until you have confirmed the root cause.

## Principle

Fix the root cause, not the symptom. A symptom fix makes the current test pass while leaving the underlying defect in place, where it will re-surface under a different input or code path. A root-cause fix eliminates the defect entirely.

## 1. Reproduce the failure

Before reading any code, obtain a reliable reproduction:

1. Run the failing test or command and capture its complete output — stderr and stdout, not just the last line.
2. Confirm the failure is deterministic. If it flakes, record the flake rate and note any conditions that affect it (timing, input size, environment variables, seed values).
3. Identify the exact error: exception type, message, file, and line number. If there is a stack trace, read every frame — do not stop at the top.

If you cannot reproduce the failure, you cannot safely fix it. Stop and ask for a reproduction case.

## 2. Isolate the blast radius

Understand the scope before diving into code:

1. **Identify the entry point**: Which function, endpoint, or command was called when the failure occurred?
2. **Trace the call chain**: Follow the stack trace (or reason through the call graph) to find the frame where the behaviour first diverges from expectations.
3. **Narrow the file set**: Use Grep to find all files that touch the failing symbol. You only need to read those files — not the whole codebase.

## 3. Form a hypothesis

Read the failing code at and around the identified line. Then:

1. State a concrete hypothesis: "The bug is in `<function>` at `<file:line>` because `<variable>` is `<value>` when it should be `<expected>`."
2. Identify what evidence would confirm or refute this hypothesis. This is your next step — not applying a fix.

Avoid "shotgun" hypotheses like "something is wrong with the config." Name a specific variable, value, or condition.

## 4. Gather evidence

Collect the data that proves or disproves the hypothesis:

- **Add targeted logging**: Insert a minimal `log()/print()` statement (or use a debugger breakpoint if available) at the point in question to observe the actual value. Do not add logging elsewhere — one observation point per hypothesis.
- **Re-run with the log**: Execute the failing scenario again and read the output. Does the actual value match your hypothesis?
- **If hypothesis is confirmed**: proceed to step 5.
- **If hypothesis is refuted**: update the hypothesis based on the new evidence and repeat step 4. Track discarded hypotheses — they narrow the search space.

## 5. Identify the minimal fix

Once the root cause is confirmed:

1. Determine the smallest code change that eliminates the defect without altering any other behaviour.
2. Consider: Does the fix handle all inputs, or only the specific input that triggered the failure? If only the specific input, the fix is incomplete — reason through the general case.
3. Check for related callsites: Grep for other places in the codebase that contain the same defective pattern. If found, note them — they likely carry the same bug.

## 6. Apply and verify

1. Apply the fix.
2. Re-run the originally failing test. It must now pass.
3. Run the full test suite. No previously passing test may now fail.
4. Remove any debug logging you added in step 4.

## 7. Report

Produce a concise post-mortem:

```
Bug report
----------
Symptom   : (what the user or test observed)
Root cause: (the specific defect — variable, assumption, or invariant violated)
Fix       : (the change made and why it addresses the root cause)
Verified  : (test name/command that now passes; suite status)
Related   : (any similar patterns found elsewhere, or "none")
```

If the full suite now passes and debug artifacts are removed, the fix is complete.
