# batbox vs. Claude orchestration, same model — first eval (DIS-1018)

**Issue:** DIS-1018 (DIS-1012 Child B) · **Author:** Reed · **Reviewer:** Wren
**Model held constant:** `qwen2.5:7b`, temperature 0, served by local ollama at
`:11434/v1` behind the counting `ollama_proxy.py`.
**Harness:** `tests/integration/eval_orchestration_ollama.cpp` (CTest tier
`requires_ollama`). **Loop:** `tools/eval/improve_loop.sh`. **Raw artifacts:**
`${BATBOX_EVAL_OUT:-/tmp/batbox-dis1018-eval}/results.{md,csv}`.

---

## The question (Cass's bar)

> "use ollama+Claude and ollama+batbox to run the same task, compare output and
> token usage … they'll both be running the same model so it'll all be the harness
> doing the lifting."

Hold the **model** constant so the only variable is the **orchestration shape**. Does
batbox's design do better — or cheaper — work than a plain subagent-dispatch harness, on
the identical brain? This is the "batbox is *good*, not just clever" proof. It is reported
honestly: where batbox wins, where it loses, with numbers.

## The two arms (what actually differs)

Both arms call the **same** ollama model through an identical counting proxy. Token
accounting is defined **once** and applied to both: an arm's "tokens" = the sum of
`usage.total_tokens` over every model call it made, read from that arm's proxy `/__stats`.
The proxy forces `temperature:0` for both arms equally.

- **batbox arm** — the standing-window organ. A warm SubAgent is seeded with the source
  **once** (`AgentSupervisor::spawn`), `promote()`d, then `interrogate()`d for each
  follow-up. This is the exact machinery DIS-1017 proved answers a follow-up without
  re-distilling the source. For the lookup task the organ's `StandingSelector::distill()`
  is driven directly and is expected to stay **closed**.
- **Claude arm** — the definitional subagent-dispatch shape *without* a standing window:
  every follow-up is a **fresh, stateless** chat completion handed the full raw source +
  the question. That is what "driving subagents" means when there is no warm-window organ
  to reuse: a new context each turn. No batbox organ is involved — a plain
  OpenAI-compatible call to the same model.

## Task set (B-AC5: ≥1 investigation, ≥1 lookup)

1. **`lang-choice-2q`** *(investigation)* — a conflicting multi-source research body (no
   single answer) + **2** follow-up questions. Exercises the standing/interrogate organ.
2. **`lang-choice-4q-deep`** *(investigation)* — the same source + **4** follow-ups, to
   expose how the gap scales with conversation length.
3. **`config-port-lookup`** *(lookup)* — one resolved fact (`listen_port = 8080`) buried in
   a 120-line config. The organ should stay **closed** (no warm window); we check batbox
   does not regress on a flat lookup.

## Results (live run, `qwen2.5:7b`, temp 0)

| Task | Class | Arm | Quality | Tokens | Model calls | Token Δ vs Claude |
|------|-------|-----|---------|--------|-------------|-------------------|
| lang-choice-2q | investigation | **batbox** | PASS | **2937** | 3 | **+248.4%** |
| lang-choice-2q | investigation | claude | PASS | 843 | 2 | — |
| lang-choice-4q-deep | investigation | **batbox** | PASS | **5642** | 5 | **+238.9%** |
| lang-choice-4q-deep | investigation | claude | PASS | 1665 | 4 | — |
| config-port-lookup | lookup | **batbox** | PASS | **1507** | 1 | **+25.5%** |
| config-port-lookup | lookup | claude | PASS | 1201 | 1 | — |

Per-turn token deltas (the mechanism):

| | turn 1* | turn 2 | turn 3 | turn 4 |
|---|---|---|---|---|
| batbox (warm window) | **1800** | 1137 | 1291 | 1414 |
| claude (fresh dispatch) | 432 | 411 | 395 | 427 |

\* batbox turn 1 includes the one-time spawn turn that loads the source into the window.

## Verdict — with numbers

**Quality: a wash.** Both arms PASS the keyword rubric on every task at every conversation
length. On `qwen2.5:7b` the orchestration shape did **not** change task correctness —
neither arm beat the other on quality. (One qualitative wrinkle: on a short investigation
the warm window occasionally answered a follow-up with a clarifying question rather than a
direct answer, where the fresh-dispatch arm answered crisply. It still satisfied the rubric;
worth noting, not a quality win for batbox.)

**Token spend: batbox loses, and the loss is structural.**
- Multi-turn investigation: batbox costs **+248%** (2 turns) and **+239%** (4 turns) — i.e.
  **~3.4×** the tokens for the same answers on the same model.
- Flat lookup: batbox costs **+25%** even though it correctly stays closed — the
  `report_gold` structured-distill call carries tool-schema overhead the single plain
  completion does not.

**Why.** Chat completions are stateless at the HTTP layer, so *both* arms re-send context
each turn — but they re-send **different** things. The Claude arm sends a small, **constant**
payload per question (source + one question ≈ 410 tok/turn, flat across all four turns). The
batbox warm window re-sends its **full accumulated history** — the raw source plus every
prior Q&A — and that grows every turn (1137 → 1291 → 1414). The warm window's proven
property (DIS-1017) is "no re-*distillation* of the source" (the selector endpoint stays at
0 requests) — but that is **not** the same as "fewer tokens on the wire." The source is
still carried in the window's history and re-sent each turn.

## What this means (and where batbox *is* good)

This is the honest "good, not just clever" answer on the token axis: **on this model,
batbox's standing-window orchestration is not a token-saver — it is a token-cost.** What it
genuinely buys is *not* visible in `total_tokens`:

- **It does not re-engulf/re-distill the source** on follow-ups (DIS-1017's structural
  guarantee). When source ingestion is *expensive* — a large document, a tool-heavy fetch,
  or a costly distill step the naive arm would repeat per question — that property is a real
  latency/compute win the token count alone doesn't capture.
- **It stays closed on flat lookups** (organ correctly opened no warm window;
  `standing_count == 0`), so the warm-window machinery adds no *behavioral* overhead on
  simple tasks — only the modest structured-distill cost.
- **Prompt-cache reuse:** ollama keeps the warm window's KV cache hot, so batbox's larger
  re-sent prefix is cheaper in *wall-clock/compute* than the token count implies — but
  `usage.total_tokens` still counts those tokens, so this does not show up in the metric.

## The fix this points at (gated, not applied)

The token loss has a clear, single, bounded cause: **the warm window carries raw history.**
The first iteration of the self-improvement loop (`tools/eval/improve_loop.sh`,
`iter-1/proposal.md`) therefore proposes exactly one change — **wire the notepad/compaction
organ (DIS-981 S6 notepad / DIS-983 `compact_to_notepad`) into the standing window so a
follow-up turn carries a *compacted* summary instead of raw history.** Expected effect: the
batbox per-turn curve flattens and the multi-turn token gap closes or reverses. That is an
organ change — it is **gated on board approval and not applied here** (see
`SELF_IMPROVEMENT_LOOP.md`).

This result also directly feeds **DIS-1011**: if the warm window is justified by latency /
no-re-distill rather than token thrift, its re-scope from a *correctness gate* to a
*warm-cache hint* is the right read.

## Reproduce

```sh
ollama pull qwen2.5:7b
cmake --build build --target eval_orchestration_ollama
ctest --test-dir build -L requires_ollama -R eval_orchestration_ollama -V
# or run the binary directly:
BATBOX_EVAL_OUT=/tmp/batbox-dis1018-eval build/tests/eval_orchestration_ollama
# override the model:  BATBOX_OLLAMA_TEST_MODEL=llama3.1:8b build/tests/eval_orchestration_ollama
```

On a box with no ollama/GPU the harness returns 77 → CTest reports it **SKIPPED**, never
failed; the hermetic suite is untouched.
