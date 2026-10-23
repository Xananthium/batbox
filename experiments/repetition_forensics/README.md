# Repeated-word incident: evidence and limits

The October 22 Claude Code subagent incident is real in the saved transcript:
one assistant text block contains 62,784 occurrences of `our`, with 64,000
reported output tokens and a `max_tokens` stop. The configured backend was the
converted Gemma4 model, TurboQuant KV cache, MTP with two speculative tokens,
and a 200,000-token context window. Neither model consciousness nor a particular
failure cause follows from this behavior.

The exact request, effective sampler, original system/tool definitions, and raw
SSE stream were not recovered. Consequently the evidence does not distinguish
generation from erroneous stream assembly with certainty, or identify
conversion, temperature, speculative decoding, or context as the cause.
Raw transcripts remain private outside the repository. The sanitized finding
records hashes, counts, timestamps, and the scope of the search.

Two approximate OpenAI-format replays per format did not reproduce a loop.
Those requests contained 18,636 input tokens versus the incident's 32,344 and
used reconstructed tool definitions. They do not exonerate either format.

## Sampling checks

The installed Claude Code 2.1.236 binary, also named by the incident record,
was probed against a deterministic local HTTP stub. Both a direct invocation
and the built-in `general-purpose` agent omitted temperature, top-p, and top-k.
The probe used isolated configuration and did not reproduce a spawned
subagent. It requested 32,000 output tokens, unlike the historical 64,000.

A separate CPU test exercised the installed vLLM Anthropic request converter
and sampling builder. Omitted fields inherited the supplied serving defaults:
temperature 0.6, top-p 0.9, top-k 64, min-p 0.05, repetition penalty 1.1.
Explicit client fields correctly overrode their corresponding defaults.
This checks the current path; it does not recover the historical request.

```bash
python3 probe_cli_sampling.py --binary ~/.local/bin/claude \
  --output 20261023-cli-sampling-probe.json
CUDA_VISIBLE_DEVICES='' VLLM_PLUGINS='' /path/to/vllm-env/bin/python \
  probe_sampling_fallback.py --output 20261023-anthropic-sampling-fallback.json
```

The controlled MTP-on/off serving comparison is a separate pending experiment.
Zero loops in a small sample would establish only failure to reproduce, not
the absence of a rare failure or its historical cause.
