#!/usr/bin/env python3
"""Verify the installed vLLM Anthropic sampling fallback without inference.

Run with CUDA_VISIBLE_DEVICES='' and VLLM_PLUGINS='' in the serving environment.
"""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    from vllm.entrypoints.anthropic.protocol import AnthropicMessagesRequest
    from vllm.entrypoints.anthropic.serving import AnthropicServingMessages
    import vllm.entrypoints.anthropic.protocol as ap
    import vllm.entrypoints.anthropic.serving as asp
    import vllm.entrypoints.openai.chat_completion.protocol as cp

    request = AnthropicMessagesRequest(model="sampling-probe",
        messages=[{"role": "user", "content": "READY"}], max_tokens=64000)
    messages = [{"role": "user", "content": "READY"}]
    defaults = {"temperature": 0.6, "top_p": 0.9, "top_k": 64,
                "min_p": 0.05, "repetition_penalty": 1.1}

    def sampler(req):
        chat = AnthropicServingMessages._build_base_request(req, messages)
        result = chat.to_sampling_params(64000, defaults)
        return {key: getattr(result, key) for key in defaults}

    actual = sampler(request)
    assert actual == defaults, (actual, defaults)
    overrides = {"temperature": 0.2, "top_p": 0.8, "top_k": 32}
    explicit = sampler(request.model_copy(update=overrides))
    assert explicit == dict(defaults, **overrides)
    result = {"description": "CPU test of installed Anthropic request conversion and actual SamplingParams fallback",
              "omitted_client_fields": actual, "explicit_client_fields": explicit,
              "source_files": {str(Path(m.__file__)): hashlib.sha256(Path(m.__file__).read_bytes()).hexdigest()
                               for m in [ap, asp, cp]},
              "historical_request_reconstructed": False, "gpu_used": False}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
