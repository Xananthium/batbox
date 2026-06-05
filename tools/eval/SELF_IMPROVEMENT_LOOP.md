# DIS-1018 — Bounded, human-in-the-loop self-improvement loop (design + OSS survey)

**Issue:** DIS-1018 (DIS-1012 Child B), acceptance criterion **B-AC3**.
**Driver:** `tools/eval/improve_loop.sh` wrapping `eval_orchestration_ollama`.

This is deliverable 2 of Child B: a self-improvement loop that is **bounded** (explicit
max iterations), has a **human/board gate before any change is applied**, and **captures
per-iteration artifacts**. Cass's instruction was "survey OSS first — like you can find
on GitHub — don't reinvent." We did; the survey is below, and the verdict is to lift the
shape of our in-house `terminal-bench-loop` skill.

---

## OSS survey — what was considered, what was lifted, why

Target pattern: **run eval → diagnose the exact weakness → propose ONE fix → human/board
approves → rerun**, with a hard iteration cap. Headline finding from the survey: the OSS
ecosystem ships two of the three primitives off the shelf — **bounded iteration** and
**eval/diagnose harnesses** — but the **explicit human/board approval gate before a change
is applied is essentially absent**. The closest mainstream project (DSPy) *removed* its
confirmation prompt. That asymmetry is the justification for the in-house gated loop.

| Project | Loop mechanism | Bounded? | HITL approval-before-apply? | Verdict |
|---|---|---|---|---|
| **DSPy** (`stanfordnlp/dspy`, MIPROv2) | Optimizer/teleprompter: proposes instruction + few-shot demo candidates, Bayesian search over `num_trials`, auto-picks best by a `metric`. | Yes — `auto` presets / `num_trials`, `max_bootstrapped_demos`, `max_errors`. | **No** — `requires_permission_to_run` was *removed* ("User confirmation is removed from MIPROv2"). Returns the optimized program automatically. | Reference for *bounded metric-driven iteration*. Black-box prompt search, not transparent diagnose→propose→approve. Its deliberate gate removal is the strongest argument *for* our board gate. **Not lifted.** |
| **Self-Refine** (`madaan/self-refine`; Reflexion `noahshinn/reflexion` is its cousin) | Per task: `Init` → `Feedback` (model critiques itself) → `Iterate` (model revises), repeat to a stop criterion. | Yes — explicit per-task cap (e.g. `--max_attempts 4`) + early stop. | **No** — fully automated self-grading, no external eval, no human in the loop. | Lift the **bounded-iterate shape** (max-iters + stop criterion) and the clean phase separation. Reject self-judgment + absent gate. **Shape lifted, judging rejected.** |
| **OpenAI Evals** (`openai/evals`) | An eval *harness* + benchmark registry (deterministic + model-graded). Runs an eval, reports scores. | N/A — no loop. | N/A in the framework. | Provides only the "run eval" sub-step (the primitive we diagnose *from*). No control flow to lift. **Primitive only.** |
| *Honorable mention:* **promptfoo** + `klausners/prompt-optimizer` | promptfoo = declarative eval/regression harness; the community wrapper closes the loop (eval → below-threshold prompts → LLM rewrite → re-eval) and **prompts before each rewrite (a HITL gate)**. | Wrapper: yes (threshold-driven). | **Yes** — the only OSS instance of "ask a human before each apply", but a thin community wrapper, not a framework. | Confirms the gate is buildable + valuable; too small to depend on. **Validates the design.** |
| *Academic:* **EvalGen** ("Who Validates the Validators?") | HITL alignment of *LLM-judge criteria* (humans grade a sample; assertions are proposed/aligned). | — | HITL on validating the *grader*, not approving a *fix*. | Grounds *why* a human belongs in eval-driven iteration. **Cited, not lifted.** |

**Decision:** default to the in-house **`terminal-bench-loop`** pattern
(`skills/terminal-bench-loop/SKILL.md` in the Paperclip repo). It is already the bounded,
board-gated iterate loop Cass referenced, and it is *stricter* than anything in the survey:
it makes "no coding before board approval" a hard rule, names the exact stop point before
proposing, and keeps a non-destructive audit trail of every iteration. We lift its shape
and bound it to this eval.

## What we lifted from `terminal-bench-loop`

1. **The three invariants**, checked + printed every iteration:
   1. *Productive work continues* — each iteration ends with a named next-action owner.
   2. *Only real blockers stop work* — the approval gate is a genuine blocker (board
      confirmation), not a pseudo-stop.
   3. *No infinite loops* — `MAX_ITERS` bound **plus** the gate halts before any change.
2. **Diagnose the exact stop point before proposing.** The loop parses the eval's
   `results.csv`, finds the single worst adverse signal (orchestration error > rubric
   failure > token regression), and frames ONE bounded fix.
3. **Human/board gate before any change.** The loop **never edits batbox source**. A fix is
   a human action, unlocked by an explicit approval token (a file a reviewer drops after
   reading `proposal.md`, standing in for a Paperclip `request_confirmation` acceptance).
4. **Per-iteration artifacts, never overwritten.** Each iteration writes its own
   `iter-N/{results.md,results.csv,run.stdout,run.stderr,diagnosis.md,proposal.md,STATUS}`.
5. **Every stop is an explicit state with an owner** — `done` (smoke pass),
   `awaiting-approval` (blocker: board), `blocked` (substrate down), or `stopped` (budget).

## How a fix actually lands (the loop stays hands-off)

The loop measures and proposes; it does **not** apply. The full cycle is:

1. `tools/eval/improve_loop.sh` runs the eval, diagnoses, writes `proposal.md`, halts at the
   gate in `awaiting-approval`.
2. A reviewer (Wren / board) reads `proposal.md`. If accepted, they `touch` the approval
   token, then **apply the approved batbox change and rebuild** out of band (a normal
   reviewed code change — its own commit, its own review).
3. Re-running the loop re-measures: iteration N+1's eval reflects the applied change. If the
   adverse delta is gone, the loop reports **smoke pass**; otherwise it proposes the next
   bounded fix, up to `MAX_ITERS`.

This keeps the loop bounded and auditable while ensuring no autonomous, unreviewed change
ever touches the organ.

## First diagnosed improvement candidate (from the live run)

The first eval run (see `DIS1018_EVAL_WRITEUP.md`) shows batbox spending **2.4–3.5× more
tokens** than the naive arm on multi-turn investigations on the same model, because the
standing warm window re-sends its full accumulated history (including the raw source) every
turn over stateless HTTP. The loop's iteration-1 `proposal.md` therefore proposes the single
bounded fix: **wire the notepad/compaction organ (DIS-981 S6 notepad / DIS-983
`compact_to_notepad`) into the standing window so follow-up turns carry a compacted summary
instead of raw history.** That proposal is gated — it is a real organ change and waits on
board approval before any code is written.

## Usage

```sh
# default: 3 iterations, 25% token-regression threshold, build dir <repo>/build
tools/eval/improve_loop.sh [MAX_ITERS]

# knobs
BATBOX_BUILD_DIR=/path/to/build \
BATBOX_LOOP_OUT=/tmp/batbox-dis1018-improve-loop \
BATBOX_TOKEN_REGRESSION_PCT=25 \
  tools/eval/improve_loop.sh 3

# approve iteration N's proposal (board/reviewer action), then apply+rebuild, then rerun:
touch /tmp/batbox-dis1018-improve-loop/approvals/iter-1.approved
```
