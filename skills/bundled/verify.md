---
name: verify
description: Verify a code change — re-read every changed file, run the build, execute the test suite, and type-check before declaring work done
allowed_tools: [Read, Bash, Glob, Grep]
---
# Verify Skill

You are performing a rigorous post-change verification pass. Do not declare success until every check below has been completed and passed. The goal is to catch regressions, incomplete changes, and integration issues before they reach review or production.

## 1. Understand what changed

Before running anything, understand the scope of the change:

1. Read each modified file in full — do not skim. Pay attention to the diff boundary: what was there before, what is there now, and why.
2. Identify every symbol that was renamed, moved, or deleted. These are the highest-risk breakage points.
3. Note any files that the changed code imports from or exports to — those are the blast radius.

## 2. Static checks (no build required)

Run these immediately; they are fast and catch entire classes of errors before compilation:

- **Type-check**: `tsc --noEmit` (TypeScript), `mypy` (Python), or the equivalent for the project's language. Fix all errors before proceeding — do not silence with suppression comments unless the suppression existed before your change.
- **Lint**: Run the project linter (`eslint`, `ruff`, `clang-tidy`, etc.) on the changed files. Address any new warnings introduced by your change, even if pre-existing warnings remain.
- **Grep for debug artifacts**: Search for `console.log`, `debugger`, `TODO`, `FIXME`, `print(`, `dbg!`, and any hardcoded test values (fake IDs, magic strings) you may have added during development. Remove them.

## 3. Build

Run the full build, not an incremental one if there is any doubt about cache freshness:

```
cmake --build build --target all   # C++ projects
npm run build                      # TypeScript/Node
cargo build                        # Rust
```

A clean build with zero warnings is the standard. If the build introduces new warnings, treat them as errors and fix them before moving on.

## 4. Run the test suite

Run the tests most directly relevant to the change, then the full suite:

1. **Targeted tests first**: Run only the test file(s) for the changed component. If any targeted test fails, stop here and fix the failure — do not drown it in a full-suite run.
2. **Full suite**: Once targeted tests pass, run the complete test suite. A change that passes its own tests but breaks unrelated tests has introduced a regression.
3. **Check coverage delta**: If the project tracks test coverage, confirm your change did not reduce coverage of the affected module.

Report the exact test runner command used, the number of tests run, and the number passed/failed.

## 5. Verify the primary behaviour

For each acceptance criterion of the original task, confirm it is met:

- If the change adds a feature: exercise the feature through its primary interface (CLI invocation, API call, UI action).
- If the change fixes a bug: reproduce the original failure condition and confirm it no longer triggers.
- If the change is a refactor: confirm the externally visible behaviour is identical before and after.

## 6. Check edge cases

Think about the boundaries of the change and test at least two non-happy-path scenarios:

- **Empty input**: What happens when the primary data is absent, nil, or empty?
- **Error path**: What happens when a dependency fails, a file is missing, or a network call returns an error?
- **Concurrent access**: If the changed code is called from multiple threads or async contexts, consider whether races are possible.

## 7. Report

Produce a structured summary:

```
Verification complete
--------------------
Files reviewed   : <N>
Static checks    : PASS / FAIL (describe issues)
Build            : PASS / FAIL (describe errors)
Tests            : <X>/<Y> passed
Behaviour check  : PASS / FAIL (describe what was exercised)
Edge cases       : PASS / FAIL (describe what was tested)

Issues found     : (list each with file:line and description, or "none")
```

If any item is FAIL, do not mark the task complete. Fix the issue and re-run verification from the beginning of the failing section.
