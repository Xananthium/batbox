---
name: simplify
description: Delete unused code, inline single-use abstractions, deduplicate logic, and reduce cognitive load without changing behaviour
allowed_tools: [Read, Edit, Glob, Grep]
---
# Simplify Skill

You are reducing complexity. Complexity is not just long files — it is any structure that forces the reader to hold more in their head than the problem requires. Every step here removes something: dead code, unnecessary indirection, duplicated logic, or misleading names. You do not change external behaviour. You change the cost of understanding the code.

## Principle

The goal is not "fewer lines." The goal is fewer concepts per line. A 30-line function that does one thing is simpler than a 10-line function that does three.

## 1. Read the target scope

Read each file in the scope of this simplification task in full. Do not begin editing until you have a complete picture:

1. Identify every function, class, constant, and type that is exported or used externally — these are off-limits for deletion (though they can be refactored in-place).
2. Map the internal call graph: which helpers call which, what is called only once, what is never called.
3. Note any patterns that appear more than once — these are deduplication candidates.

## 2. Find dead code

Dead code includes:

- **Unreachable branches**: `if (false)`, conditions that can never be true given the type constraints or invariants.
- **Unused variables**: local variables assigned but never read; function parameters that are ignored.
- **Unused imports and dependencies**: modules imported but not referenced; vcpkg / npm packages listed but never used.
- **Commented-out code**: blocks of code wrapped in `/* */` or `//` that are not explanatory comments — delete them, do not keep them "just in case" (source control exists for that).
- **Unreferenced exports**: functions or types exported but not imported anywhere in the codebase (verify with Grep before deleting).

Delete dead code first, before any other change. This clears noise and often makes the remaining opportunities obvious.

## 3. Inline single-use abstractions

An abstraction earns its existence by being reused or by naming a concept that is materially clearer than its implementation. A function called exactly once from exactly one place, with a name that does not add clarity beyond "this is the code that runs here," is pure overhead.

For each such function:

1. Confirm it is called exactly once (Grep for the name).
2. Confirm its name does not carry meaningful semantic value beyond what the call site already communicates.
3. Inline its body at the call site and delete the function definition.
4. Check the call site reads clearly after inlining — if it does not, the abstraction was earning its keep and should be restored.

Apply the same logic to single-use type aliases and wrapper types that add no invariant or documentation value.

## 4. Deduplicate repeated logic

When the same logic appears in two or more places:

1. Read both copies carefully. Confirm they are truly identical in semantics, not just similar in shape — subtle differences often hide intentional divergence.
2. Extract the common logic into a single well-named function.
3. Replace both originals with calls to the new function.
4. Run the tests to confirm the deduplication did not alter behaviour.

Do not extract logic that is similar but not identical. Forced abstraction over slightly-different-things creates coupling that is harder to evolve than duplication.

## 5. Reduce nesting

Deep nesting multiplies cognitive load because the reader must track all enclosing conditions simultaneously. Flatten it:

- **Early return / guard clause**: If the first thing a function does is check a precondition, return immediately on failure rather than wrapping the happy path in a block.
- **De-Morgan inversion**: Rewrite `if (!condition) { ... } else { main path }` as `if (condition) { main path }`.
- **Extract loop bodies**: If the body of a loop is more than ~10 lines, extract it into a named function.

Target: no function should have more than three levels of nesting. If you cannot achieve three levels without breaking behaviour, leave it and note why.

## 6. Clarify names

A name is misleading when it does not match what the thing actually does. Misleading names are worse than generic names because they actively misdirect. Rename:

- Functions that do more (or less) than their name promises.
- Variables whose names describe their type rather than their purpose (`data`, `info`, `temp`, `result`).
- Boolean variables with ambiguous polarity (`flag`, `status`, `enabled` when "enabled" is unclear in context).

Do not rename for stylistic preference alone — only rename when the current name causes genuine confusion.

## 7. Verify behaviour is unchanged

After each change (not all at once):

1. Ensure the project still builds cleanly.
2. Run the relevant test suite. Every previously passing test must still pass.
3. If a test fails after a refactor, the refactor changed behaviour — revert and re-examine.

## 8. Report

Produce a structured summary of what was changed:

```
Simplification summary
----------------------
Dead code removed     : (list functions/variables/files deleted, or "none")
Abstractions inlined  : (list inlined functions, or "none")
Duplicates merged     : (describe what was merged, or "none")
Nesting reduced       : (list functions flattened, or "none")
Names clarified       : (list renames, or "none")

Tests                 : all <N> passing
Net lines removed     : <N> (informational only — not the primary metric)
```

If any section has nothing to report, say "none." Do not fabricate improvements.
