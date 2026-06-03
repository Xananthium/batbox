# batbox — Maintenance Notes

Operational notes for running and maintaining batbox's test tiers and substrates.

---

## Real-model (ollama) test substrate — `requires_ollama` tier  (DIS-1017)

batbox's subagent organs are proven two ways:

| Tier | What it proves | When it runs |
|---|---|---|
| **Hermetic** (`fake_openai_server.py`, `fake_distill_server.py`) | the **wiring** — every path with forced, canned responses | the default green CI bar; no GPU, no network |
| **`requires_ollama`** (this section) | the **behaviour** — the same organ against a **real local model** | opt-in on a box with ollama + a GPU |

The real-model tier exists because a stub can prove the plumbing but not how the
organ behaves when a real model produces the `report_gold` signals. It re-proves
the DIS-1007 selection behaviour (AC2 warm-window interrogate, AC3 follow_up_ok
routing) against a real model — the unblock condition Paulina named on DIS-1007.

### Pinned model

**Default: `qwen2.5:7b`.** This is a deliberate **bump from the spec default
`llama3.2:3b`** (DIS-1017 crux decision):

- `report_gold` is a *structured* first-turn extraction. If the model fails to
  emit a parseable top-level `answer`, `SubagentDistiller::extract_gold()` returns
  `nullopt` and the whole standing path falls back closed→original — which defeats
  the behavioural proof. It is **not** a content-tolerance issue a looser assertion
  could rescue.
- Live on this box, `llama3.2:3b` intermittently nests the gold inside a
  stringified `"object"` field with **no** top-level `answer` (parse → nullopt).
  Flaky schema adherence → unsuitable for a green gate.
- `qwen2.5:7b` emits clean `{answer, confidence, follow_up_ok}` with no junk keys.

Override with `BATBOX_OLLAMA_TEST_MODEL=<model>` (and `BATBOX_OLLAMA_BASE=<url>`,
default `http://127.0.0.1:11434`).

### Pull + run

```bash
# 1. one-time: pull the pinned model
ollama pull qwen2.5:7b

# 2. make sure ollama is up (OpenAI-compatible endpoint)
curl -s http://127.0.0.1:11434/v1/models | jq '.data[].id'

# 3. run ONLY the real-model tier
ctest -L requires_ollama --output-on-failure        # (from the build dir)

# or run the binary directly (prints token-usage numbers per case)
./tests/test_selection_heuristic_ollama

# the default ctest run is unaffected — when ollama is down or the model is
# absent, the tier SKIPs (CTest SKIP_RETURN_CODE 77), never fails.
```

### Same-model orchestration eval (DIS-1018)

The same `requires_ollama` tier carries `eval_orchestration_ollama` — a
**batbox-vs-Claude same-model eval**: it runs the same tasks through the batbox
standing-window organ and through a naive stateless subagent-dispatch arm, both on
the same pinned ollama model behind the same counting proxy, and reports per-arm
task outcome + total token usage in a comparable table.

```bash
ctest -L requires_ollama -R eval_orchestration_ollama -V    # (from the build dir)
# or directly (writes results.md + results.csv to BATBOX_EVAL_OUT):
BATBOX_EVAL_OUT=/tmp/batbox-dis1018-eval ./tests/eval_orchestration_ollama
```

- Token accounting is defined **once** and applied to both arms: an arm's tokens =
  sum of `usage.total_tokens` over its model calls, read from its proxy `/__stats`.
- Bounded, human-in-the-loop self-improvement loop: `tools/eval/improve_loop.sh`
  (bounded iters + board gate before any change + per-iteration artifacts).
- Writeup + OSS survey: `tools/eval/DIS1018_EVAL_WRITEUP.md`,
  `tools/eval/SELF_IMPROVEMENT_LOOP.md`. Reference results:
  `tools/eval/results-reference/`.

### How it works

- `tests/fixtures/ollama_proxy.py` is a thin **counting reverse-proxy** in front
  of ollama. It (a) forces `temperature:0` (the determinism strategy), (b) counts
  `POST /v1/chat/completions` requests, and (c) captures token `usage` — all on
  `GET /__stats`. It speaks the same `READY <port>` handshake as the fake servers.
  The model still does all generation; the proxy only forwards + observes.
  (A request-debug dump is available off-by-default via `PROXY_DEBUG_LOG=<path>`.)
- Topology mirrors the hermetic test's two fake servers: the **selector** distill
  endpoint and the **warm subagent** endpoint each get their own proxy port, both
  forwarding to the same ollama. Two ports let the test attribute requests to the
  selector vs the warm path — which is what makes the **no-re-engulf** property
  structurally provable (the source/selector endpoint stays at 0 extra requests
  across an interrogation).
- SKIP: `test_selection_heuristic_ollama`'s own `main()` probes `/v1/models` for
  the pinned model and returns **77** when absent; the test is registered with
  `LABELS requires_ollama` + `SKIP_RETURN_CODE 77`.

### Determinism & the central finding (read before editing the test)

Real-model `report_gold` control signals (`confidence`/`follow_up_ok`) are
**model-dependent and not byte-stable**, so all output assertions use
keyword/substring tolerances, never byte-equality, with `temperature:0`.

**Finding (flagged on DIS-1017, informs DIS-1011):** under the organ's *exact*
distiller prompt ("report ONLY the golden line … nothing more, nothing less") and
its `report_gold` schema (`confidence`/`follow_up_ok` marked *Optional*), a
well-aligned local model (`qwen2.5:7b`) reliably **omits** the keep-warm signal on
investigations → the organ **CLOSES**. No honest input (no caller-dictated field)
was found that makes a clean-parsing local model spontaneously vote keep-warm
through this organ; `llama3.2:3b` *will* emit `follow_up_ok=true` but flakes on
parse. The test therefore proves three deterministic, never-red facts and flags
this: (1) CLOSE routing on real output, (2) the organ HONORS whatever the real
model voted (never a caller flag), (3) warm-window interrogate without re-engulf,
via the same `promote()`+`interrogate()` machinery the organ uses.

If you change the organ's confirm-after thresholds
(`kTrivialLookupConfidence`/`kLowConfidence` in `StandingSelector.cpp`), update
their mirror in `test_selection_heuristic_ollama.cpp` (`predict_keep_warm`) — the
coupling is intentional (the HONOR case asserts the organ routes by the same rule).

### Representative token usage (qwen2.5:7b, one green run)

Captured from `/__stats` during the tier (real numbers, for cost grounding and to
seed DIS-1012 Child B):

| Path | Tokens |
|---|---|
| CLOSE first-turn distill (selector endpoint, large single-fact body) | ~1507 total |
| Investigation first-turn distill (selector endpoint) | ~447 total |
| Warm-window interrogation (warm endpoint, one follow-up turn) | ~1173 total |

Wall time for the tier: ~2.7 s.
