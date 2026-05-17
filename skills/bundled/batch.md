---
name: batch
description: Group similar repetitive operations and dispatch independent tool calls in parallel to minimise round-trip overhead
allowed_tools: [Read, Write, Edit, Bash, Glob, Grep]
---
# Batch Skill

You are executing a coordinated batch of changes or operations — multiple files, multiple endpoints, or a repeated pattern applied across a codebase. The governing discipline is: plan the full scope before touching anything, respect dependencies, and run independent operations in parallel.

## Principle

Serial execution of independent work is waste. If file A and file B do not share any dependency, they can be changed simultaneously. The batch approach reduces total wall-clock time and makes the scope of the change visible upfront, so conflicts and ordering issues are caught before code is written.

## 1. Discover the full scope

Before making any change, identify every item that needs to change:

1. **Glob for file candidates**: Use Glob to find all files that match the pattern (e.g., all `*.test.ts` files, all `CMakeLists.txt` under `src/`, all `.md` files in a directory).
2. **Grep for symbol references**: Use Grep to find every occurrence of the symbol, pattern, or string that needs updating. Do not rely on memory or assumption — scan explicitly.
3. **Enumerate the full list**: Write out every file and every change needed. This list is the plan. Do not start modifying until the list is complete.

If the scope is larger than expected, report it before proceeding. Do not silently expand the work.

## 2. Identify dependencies and ordering

Not all items in a batch are independent:

- **Interface before implementation**: If you are renaming a function signature, update the declaration (`.hpp` / `.d.ts` / interface file) before the definition and all callsites. A build that imports the old signature will fail until the declaration is updated.
- **Schema before migration**: If you are changing a data schema, update the schema definition before any code that reads or writes it.
- **Parent before child**: In hierarchical configs or registries, update the parent entry before the entries that reference it.

Sort the list from step 1 into an ordered sequence that respects these dependencies. Items with no dependencies on each other can be grouped for parallel execution.

## 3. Execute in parallel where possible

For items with no inter-dependencies, dispatch tool calls simultaneously:

- Read multiple files at once rather than one at a time.
- Apply edits to independent files in the same response rather than sequentially.
- Run independent build or test commands in parallel shell invocations where the shell supports it (`& wait` or similar).

For items that depend on each other, execute them in the determined order and confirm each step before proceeding to the next.

## 4. Verify consistency after each group

After each dependency group is complete:

1. Grep for any remaining occurrences of the old pattern. A non-zero count means the batch is incomplete.
2. If the project supports incremental builds, do a quick build check to confirm the changed group compiles without errors before moving to the next group.

## 5. Run the full test suite

Once all groups are applied and consistency checks pass, run the complete test suite. A batch change that fixes its own tests but breaks unrelated tests has introduced a regression.

If tests fail:

- Identify which test file and which assertion failed.
- Determine whether the failure is in a file that was part of the batch (incomplete change) or in a file that was not (unintended side-effect).
- Fix the failure before marking the batch complete.

## 6. When to stop

Stop and report rather than guessing when:

- A file in the batch turns out to require a change that contradicts the stated goal.
- A dependency ordering conflict is discovered that cannot be resolved without additional information.
- A Grep result reveals that the symbol appears in a generated file or a vendored dependency — do not modify those without explicit confirmation.

## 7. Report

Produce a structured summary:

```
Batch summary
-------------
Total items identified : <N>
Items changed          : <N>
Items skipped          : <N> (list with reason)
Conflicts encountered  : (describe, or "none")
Tests                  : <X>/<Y> passed
Remaining occurrences  : 0 (confirmed via Grep)
```

If remaining occurrences is non-zero, the batch is not complete — do not mark it done.
