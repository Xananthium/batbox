#!/usr/bin/env bash
# tools/eval/improve_loop.sh
# =============================================================================
# DIS-1018 (DIS-1012 Child B) — BOUNDED, HUMAN-IN-THE-LOOP self-improvement loop
# around the batbox-vs-Claude orchestration eval (eval_orchestration_ollama).
#
# OSS SURVEY (B-AC3 "what was considered / lifted / why"): see
# tools/eval/SELF_IMPROVEMENT_LOOP.md.  Short version: surveyed Self-Refine /
# Reflexion (unbounded, no human gate by default), DSPy optimizers (bounded by a
# trial budget but optimize prompts autonomously, no per-change human gate), and
# OpenAI Evals (an eval harness, no improvement loop at all).  We LIFT the shape
# of our in-house `terminal-bench-loop` skill instead — it is already the bounded,
# board-gated iterate pattern Cass pointed at ("like you can find on GitHub", but
# we already own a stricter one): bounded iterations + diagnose the exact stop
# point + a human/board confirmation BEFORE any change is applied + per-iteration
# artifacts + an audit trail that never deletes a failed iteration.
#
# THE THREE INVARIANTS (lifted verbatim from terminal-bench-loop, printed each
# iteration so the operator can check them):
#   1. Productive work continues — every iteration ends with a named next action.
#   2. Only real blockers stop work — the gate is a real blocker (board approval).
#   3. No infinite loops — MAX_ITERS bound + the gate halts before any change.
#
# WHAT THIS LOOP DOES (and deliberately does NOT do):
#   - It MEASURES (runs the eval), DIAGNOSES (finds the worst adverse delta /
#     any rubric failure), and PROPOSES one bounded fix per iteration.
#   - It NEVER edits batbox source or applies a fix autonomously.  Any code change
#     is a human action, gated by an explicit approval token (a file a reviewer
#     drops after reading the proposal, standing in for a Paperclip
#     request_confirmation acceptance).  No coding before approval.
#   - On a gate-held iteration it STOPS in an explicit "awaiting-approval" state
#     (a real blocker with a named owner), not a silent spin.
#
# USAGE:
#   tools/eval/improve_loop.sh [MAX_ITERS]
# ENV:
#   BATBOX_BUILD_DIR   build dir holding ./tests/eval_orchestration_ollama
#                      (default: <repo>/build)
#   BATBOX_LOOP_OUT    artifact root (default: /tmp/batbox-dis1018-improve-loop)
#   BATBOX_TOKEN_REGRESSION_PCT  adverse-delta threshold that triggers a proposal
#                      (default: 25 — i.e. batbox >25% more tokens than Claude)
#
# Re-running the loop after a reviewer has (a) dropped the approval token AND
# (b) applied + rebuilt the approved change is how iteration N+1 re-measures the
# effect of that change against the same eval.  The loop itself stays hands-off.
# =============================================================================
set -euo pipefail

MAX_ITERS="${1:-3}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BATBOX_BUILD_DIR:-$REPO_ROOT/build}"
EVAL_BIN="$BUILD_DIR/tests/eval_orchestration_ollama"
LOOP_OUT="${BATBOX_LOOP_OUT:-/tmp/batbox-dis1018-improve-loop}"
APPROVALS="$LOOP_OUT/approvals"
REG_PCT="${BATBOX_TOKEN_REGRESSION_PCT:-25}"

mkdir -p "$LOOP_OUT" "$APPROVALS"

if [[ ! -x "$EVAL_BIN" ]]; then
  echo "ERROR: eval binary not found/executable: $EVAL_BIN" >&2
  echo "  build it:  cmake --build '$BUILD_DIR' --target eval_orchestration_ollama" >&2
  exit 2
fi

print_invariants() {
  cat <<'EOF'
  [invariants] 1. productive work continues (this iter ends with a named next action)
               2. only real blockers stop work (the gate IS a real blocker: board approval)
               3. no infinite loops (bounded by MAX_ITERS; gate halts before any change)
EOF
}

# Diagnose one iteration's results.csv -> prints a verdict line and writes
# diagnosis.md + proposal.md.  Exit 0 = SMOKE PASS (no adverse delta, all rubric
# pass); exit 10 = a fix is proposed; exit 2 = harness/data error.
diagnose() {
  local iter_dir="$1"
  python3 - "$iter_dir/results.csv" "$iter_dir" "$REG_PCT" <<'PY'
import csv, sys, os
csv_path, iter_dir, reg_pct = sys.argv[1], sys.argv[2], float(sys.argv[3])
rows = list(csv.DictReader(open(csv_path)))
if not rows:
    print("DATA ERROR: empty results.csv"); sys.exit(2)

# group by task
tasks = {}
for r in rows:
    tasks.setdefault(r["task"], {})[r["arm"]] = r

rubric_fails, regressions, errors = [], [], []
for task, arms in tasks.items():
    bx, cl = arms.get("batbox"), arms.get("claude")
    if not bx or not cl:
        continue
    for arm_name, a in (("batbox", bx), ("claude", cl)):
        if a["outcome"] == "ERROR":
            errors.append((task, arm_name))
        elif a["outcome"] != "PASS":
            rubric_fails.append((task, arm_name, a["outcome"]))
    try:
        bt, ct = int(bx["tokens"]), int(cl["tokens"])
        delta = 100.0 * (bt - ct) / ct if ct else 0.0
        if delta > reg_pct:
            regressions.append((task, bt, ct, delta))
    except ValueError:
        pass

lines = ["# Diagnosis", ""]
if errors:
    lines.append("## Orchestration errors (a real blocker — fix the harness/organ first)")
    for t, a in errors: lines.append(f"- {t}: {a} arm produced no output")
    lines.append("")
if rubric_fails:
    lines.append("## Quality (rubric) failures")
    for t, a, o in rubric_fails: lines.append(f"- {t}: {a} → {o}")
    lines.append("")
if regressions:
    lines.append(f"## Token regressions (batbox > {reg_pct:.0f}% over Claude, same model)")
    regressions.sort(key=lambda x: -x[3])
    for t, bt, ct, d in regressions:
        lines.append(f"- {t}: batbox {bt} vs claude {ct} tok ({d:+.1f}%)")
    lines.append("")

open(os.path.join(iter_dir, "diagnosis.md"), "w").write("\n".join(lines) + "\n")

# The exact stop point + the ONE bounded fix candidate.
if errors:
    worst = errors[0]
    prop = (f"# Fix proposal (iteration)\n\n"
            f"**Exact stop point:** {worst[0]} / {worst[1]} arm returned empty output.\n\n"
            f"**Proposed change (ONE, bounded):** repair the orchestration path so the "
            f"arm produces output before any token comparison is trusted. This is a real "
            f"blocker — the eval is not meaningful until both arms run.\n\n"
            f"**Three-invariant check:** (1) next action = fix organ/harness; "
            f"(2) real blocker = yes; (3) bounded = single fix, then rerun.\n")
    verdict = f"PROPOSE-FIX (orchestration error in {worst[0]}/{worst[1]})"
    code = 10
elif regressions:
    t, bt, ct, d = regressions[0]
    prop = (f"# Fix proposal (iteration)\n\n"
            f"**Exact stop point:** task `{t}` — batbox spends {bt} vs claude {ct} tokens "
            f"({d:+.1f}%) on the SAME model. Mechanism (from the eval's per-turn notes): the "
            f"standing warm window re-sends its full accumulated history (including the raw "
            f"source) every turn over stateless HTTP, while the naive arm sends a small "
            f"constant payload per question.\n\n"
            f"**Proposed change (ONE, bounded):** wire the notepad/compaction organ "
            f"(DIS-981 S6 notepad / DIS-983 compact_to_notepad) into the standing window so a "
            f"follow-up turn carries a COMPACTED summary of the source instead of the raw "
            f"history. Re-measure the same task; expect the per-turn token growth to flatten.\n\n"
            f"**Three-invariant check:** (1) next action = board reviews this proposal; "
            f"(2) real blocker = board approval before any organ change; "
            f"(3) bounded = exactly one change, then a single rerun.\n\n"
            f"**APPROVAL REQUIRED before any code is written or applied.**\n")
    verdict = f"PROPOSE-FIX (token regression in {t}: {d:+.1f}%)"
    code = 10
elif rubric_fails:
    t, a, o = rubric_fails[0]
    prop = (f"# Fix proposal (iteration)\n\n"
            f"**Exact stop point:** task `{t}` — {a} arm outcome `{o}` (quality rubric).\n\n"
            f"**Proposed change (ONE, bounded):** inspect the {a} arm's answer for `{t}`; "
            f"if the organ deflected/declined, tune the warm-window system prompt; if the "
            f"model is simply weak on this task, record it as a model limit, not an organ bug.\n\n"
            f"**APPROVAL REQUIRED before any change.**\n")
    verdict = f"PROPOSE-FIX (rubric fail in {t}/{a})"
    code = 10
else:
    prop = "# No fix proposed\n\nAll arms ran, all rubrics pass, no token regression over threshold.\n"
    verdict = "SMOKE PASS (no adverse delta, all rubric pass)"
    code = 0

open(os.path.join(iter_dir, "proposal.md"), "w").write(prop)
print(verdict)
sys.exit(code)
PY
}

echo "================================================================"
echo " DIS-1018 bounded self-improvement loop"
echo "   eval:      $EVAL_BIN"
echo "   artifacts: $LOOP_OUT"
echo "   MAX_ITERS: $MAX_ITERS   token-regression threshold: ${REG_PCT}%"
echo "================================================================"

for (( N=1; N<=MAX_ITERS; N++ )); do
  ITER_DIR="$LOOP_OUT/iter-$N"
  mkdir -p "$ITER_DIR"
  echo ""
  echo "---------- iteration $N / $MAX_ITERS ----------"
  print_invariants

  # 1. RUN the eval (the "smoke"). Artifacts are per-iteration; nothing overwritten.
  echo "  [run] eval_orchestration_ollama ..."
  if ! BATBOX_EVAL_OUT="$ITER_DIR" "$EVAL_BIN" >"$ITER_DIR/run.stdout" 2>"$ITER_DIR/run.stderr"; then
    rc=$?
    if [[ $rc -eq 77 ]]; then
      echo "  [skip] ollama/model unavailable (exit 77). Loop cannot measure — STOP."
      echo "STATE: blocked (real blocker: ollama substrate unavailable; owner: operator/workstation-admin)" | tee "$ITER_DIR/STATUS"
      exit 0
    fi
    echo "  [warn] eval exited $rc (an arm produced no output) — diagnosing anyway."
  fi

  # 2. DIAGNOSE the exact stop point; write diagnosis.md + proposal.md.
  set +e
  verdict="$(diagnose "$ITER_DIR")"; dcode=$?
  set -e
  echo "  [diagnose] $verdict"

  if [[ $dcode -eq 0 ]]; then
    echo "STATE: done (smoke pass — batbox matches/loses within threshold, all rubric pass)" | tee "$ITER_DIR/STATUS"
    echo ""
    echo "Loop complete: SMOKE PASS at iteration $N. No change proposed."
    exit 0
  elif [[ $dcode -ne 10 ]]; then
    echo "STATE: blocked (diagnosis data error; owner: Reed)" | tee "$ITER_DIR/STATUS"
    exit 2
  fi

  # 3. HUMAN/BOARD GATE — before ANY change is applied.
  APPROVAL_TOKEN="$APPROVALS/iter-$N.approved"
  if [[ -f "$APPROVAL_TOKEN" ]]; then
    echo "  [gate] APPROVED (token: $APPROVAL_TOKEN). The reviewer applies the proposed"
    echo "         change + rebuilds OUT OF BAND; iteration $((N+1)) re-measures the effect."
    echo "STATE: approved-iter-$N (proceeding to re-measure next iteration)" | tee "$ITER_DIR/STATUS"
    # The loop NEVER edits source. If the approved change was applied + rebuilt,
    # the next iteration's run will reflect it. If MAX_ITERS is reached, we stop.
    continue
  else
    echo "  [gate] HELD — no approval token for iteration $N."
    echo "         Proposal:  $ITER_DIR/proposal.md"
    echo "         To approve (board/reviewer action): touch '$APPROVAL_TOKEN'"
    echo "         then apply + rebuild the change and re-run this loop."
    echo "STATE: awaiting-approval (blocker: board confirmation of $ITER_DIR/proposal.md; owner: Wren/board)" | tee "$ITER_DIR/STATUS"
    echo ""
    echo "Loop paused at the human/board gate (invariant 2 + 3). No change applied."
    exit 0
  fi
done

echo ""
echo "STATE: stopped (iteration budget $MAX_ITERS exhausted without a smoke pass)" | tee "$LOOP_OUT/STATUS"
echo "Bounded stop: never silently starts iteration $((MAX_ITERS+1))."
exit 0
