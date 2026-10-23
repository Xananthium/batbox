# MTP sampling source audit — 2026-10-23

The audit does **not establish that speculative decoding caused the October 22
repeated-word incident**. The active V2 sampling path includes the requested
controls at each verification position. Thirteen CPU source checks pass,
including penalty history for earlier drafts, accepted-state updates, and
target probability verification. This is useful evidence against a specific
missing-penalty explanation, not a proof that the complete GPU implementation
is correct.

The captured production environment and the October 22 15:42 launch metadata
both explicitly set `VLLM_USE_V2_MODEL_RUNNER=1`. The installed source files
audited here match the October 23 pre-benchmark Python source manifest. Their
exact October 22 contents have not been established. The legacy Qwen serving
alias referred to the converted Gemma4 model; it is not model-family evidence.
The model revision `9654466...` is not a vLLM source commit.

## Active execution path

The captured MTP configuration uses two draft tokens and local argmax
reduction. It does not override `draft_sample_method` or
`rejection_sample_method`; installed defaults are `greedy` and `standard`.
`DraftModelSpeculator.sample_draft` proposes raw draft argmax tokens, optionally
through the local reduction. The absence of target penalties in that proposal
step is intentional: the proposal is a one-hot distribution, and the target
sampler subsequently verifies it. Proposal quality affects acceptance and
speed; repeated proposals are not automatically accepted.

| Stage | Repetition / frequency / presence | Temperature, min_p, top_k, top_p |
|---|---|---|
| Greedy draft proposal | Not applied to raw draft argmax | Not used for greedy proposal |
| First target verification row | Prompt + committed output history | All applied |
| Later verification rows | Also include all earlier proposed tokens in the same request | All applied |
| Bonus row after all drafts | Includes all earlier proposed tokens | All applied |
| Ordinary non-speculative sampling | Prompt + committed output history | Same sampling-parameter method; final sampling can use a different fused GPU path |

Relevant installed hooks, with file hashes and extracted symbol line numbers
in [the result JSON](20261023-mtp-sampling-audit.json):

- `v1/worker/gpu/spec_decode/rejection_sampler.py`: `_verify` calls the shared
  `Sampler.apply_sampling_params` on every expanded row, then `rejection_sample`.
- `v1/worker/gpu/sample/sampler.py`: processors → thinking-budget enforcement →
  temperature → min_p → top_k/top_p. The ordinary sampler calls the same method
  and can defer top_k/top_p to its final fused sampler.
- `v1/worker/gpu/sample/penalties.py`: `_penalties_kernel` starts with committed
  output counts, then adds `input_ids[start + prev_pos + 1]` for earlier draft
  positions. Local position zero is the already-counted last committed token;
  it is not counted twice. Prompt tokens affect repetition but not output
  frequency/presence penalties.
- `v1/worker/gpu/input_batch.py`: `_post_update_kernel` increments counts only
  for `num_sampled` emitted tokens. Unaccepted suffix drafts are not committed.
- `v1/worker/gpu/spec_decode/rejection_sampler_utils.py`: standard stochastic
  verification tests `log p(x) > log u + log q(x)`. For a greedy proposal,
  `log q(x)=0`. A rejected one-hot proposal is excluded from the residual
  distribution. Greedy target verification instead requires matching argmax.

This is the standard rejection-correction design, whose distribution-preserving
property depends on correct target probabilities, histories, and implementation.
It does not prove numerical or concurrency correctness of this locally modified
backend. [Leviathan et al., *Fast Inference from Transformers via Speculative Decoding*](https://arxiv.org/abs/2211.17192).

## What the CPU checks establish

`mtp_sampling_audit.py` reads installed files with `ast`, removes decorators and
type annotations from selected functions, and executes their unchanged bodies.
Its small pointer/vector adapter implements the required Triton operations with
CPU Torch. It never imports vLLM or Triton and requires a CPU-only Torch build.
This is source-semantics execution, not a CUDA interpreter or a GPU parity test.
Top-k/top-p use the installed PyTorch fallback, not the active Triton or
FlashInfer kernels. No request, transcript or model forward pass is executed.

The 13 checks cover:

- Two drafts of the same token, correct incremental frequency counts, and no
  double counting of the last committed token.
- Prompt-only repetition penalties; mixed requests with different penalties;
  no verification-time mutation of committed counts.
- Repetition penalty unchanged between 1 and 62,784 prior occurrences.
- Commit only the emitted prefix and recovered token, with rejected-token
  accounting and last-token state updated.
- min_p, top_k and top_p on every verification/bonus row.
- Twenty deterministic probability/uniform combinations in the extracted
  standard rejection kernel: repeated drafts are accepted only when allowed
  by target probability. Target probability zero always rejects them.
- Greedy target argmax verification and the inactive legacy-path distinction.

The synthetic loop check uses a 64-token vocabulary, repeated-token raw logit
10, other logits 0, and the configured defaults temperature .6, repetition
penalty 1.1, min_p .05, top_k 64, top_p .9. After penalties and filtering, every
row assigns the repeated token probability 1. This demonstrates that these
settings can sustain a loop even with MTP disabled. It is **not measured Gemma
logits** and does not show how the historical request entered its loop.

Repetition penalty is a fixed sign-aware adjustment for any token already seen:
positive logits are divided by the penalty, negative logits multiplied. It does
not become 62,784 times stronger after 62,784 repeats. Frequency penalty is the
separate count-dependent control; no nonzero configured frequency penalty was
established for the incident. Low temperature and support truncation can leave
no alternative token once one is sufficiently dominant. Higher MTP acceptance
would then be an expected consequence of easy-to-predict repetition, not
evidence that MTP originally caused it.

## Findings that do not apply as a demonstrated incident cause

The installed legacy `v1/sample/rejection_sampler.py` does not apply its
argmax-invariant processor list on verified draft rows; its min_p processor is
in that list. Ordinary/bonus legacy sampling does apply it. This is a concrete
source inconsistency worth a separate isolated reproducer if that runner is
ever used. It is **not the active V2 path** in either captured launch.

Upstream reports provide hypotheses, not diagnoses of this request:

- [Issue #35288](https://github.com/vllm-project/vllm/issues/35288) reports MTP
  corruption under concurrency with CUDA errors in an older V1 runner. The
  recorded versions and execution path differ from this V2 deployment. The
  historical incident window had no captured matching backend error.
- [Issue #40080](https://github.com/vllm-project/vllm/issues/40080) discusses
  Gemma repetition with structured outputs. Maintainer replies question chat
  template usage and report a failed reproduction. It does not establish an
  MTP-specific defect or apply automatically to ordinary tool calling.
- [Issue #58390](https://github.com/vllm-project/vllm/issues/58390) reports loops
  at a forced thinking-budget transition on MiMo/DFlash, with MTP-off causality
  untested. That is a different model/proposer. Native Anthropic serving here
  does map `thinking.type=enabled` and `budget_tokens` to a thinking budget;
  without the original request, whether that mechanism was active is unknown.

Exact original request sampling fields, complete system/tools, and raw server
SSE were not recovered. Native Anthropic serving forwards request temperature,
top_p and top_k; absent values fall back through the chat sampling defaults.
The known server defaults are not proof of the effective original request.
The persisted 64,000-token repeated response excludes a mere terminal redraw,
but missing raw SSE limits attribution between generation and stream assembly.

## Next causal test

The GPU owner's converted-model MTP=2 versus MTP=off ablation is the next useful
step. It was **not run**: the user prioritized fast production restoration.
Keep model weights, quantization, KV format, full rendered token IDs,
request fields, thinking mode, concurrency and output budget fixed. Capture
actual SSE and repetition onset, not only final text and average acceptance.
Stochastic runs need several seeds; an equal seed does not guarantee the same
token trajectory across different samplers. If a loop recurs, compare target
logits and processed support at the same forced prefix around onset, then test
serial and concurrent requests. A short clean run cannot rule out a rare
32k-input/64k-output failure; a bounded reconstructed prompt is not an exact
historical replay.

No production code, service, environment, package or GPU state was changed by
this audit. There is no sampler patch proposed from this evidence.

## Reproduce

From the Batbox root, using the existing CPU-only environment:

```bash
/home/isnotreal/engine-env/bin/python \
  experiments/repetition_forensics/mtp_sampling_audit.py \
  --vllm-root /home/isnotreal/vllm-env/lib/python3.12/site-packages/vllm \
  --runtime-snapshot experiments/backend/20261023-qat-format/production-before.json \
  --runtime-manifest experiments/backend/20261023-qat-format/runtime-manifest.json \
  --historical-launch /home/isnotreal/model-tools/gemma31-qat-mtp-20261021/native-qat-tq4/trials/20261022T154216Z/native-launch.json \
  --output experiments/repetition_forensics/20261023-mtp-sampling-audit.json
```

The result records the harness SHA, source SHAs, CPU Torch version, selected
launch evidence and test outcomes. Artifacts contain no raw reasoning, prompts,
credentials or recovered transcript text.
