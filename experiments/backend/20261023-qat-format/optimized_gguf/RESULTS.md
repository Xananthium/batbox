The final upstream PR #141 trial matched the converted model’s serving speed while loading the original Hauhau GGUF. The custom Tensor Core candidate improved prefill but did not achieve decode parity. No production checkpoint or installed Python package was overwritten.

| Serving configuration | C1 tokens/s | C4 tokens/s | Cold 63,460-token first output | Short / long / behavior checks |
|---|---:|---:|---:|---|
| Converted native baseline | 90.25 | 242.18 | 111.04 s | 10/10 / 5/5 / 7/7 |
| Stock original GGUF | 59.24 | 72.84 | 445.05 s | 10/10 / 4/5 / 7/7 |
| Custom fast-v1 GGUF | 48.79 | 102.03 | 109.44 s | 10/10 / 4/5 / not run |
| Upstream PR #141 GGUF | 91.41 | 251.87 | 93.83 s | 10/10 / 5/5 / 7/7 |

PR #141 shared-prefix four-client retrieval passed 4/4, with 190,272 cached tokens out of 253,851 queried and zero preemptions. Together with the cold retrieval that is 5/5 long answers. The stock and custom 4/5 results returned correct times but omitted the required UTC suffix. Vision checks use red/blue images; tool checks include a failed-format recovery workflow.

All configurations retained TP2, 200,000 configured context, TQ4 KV, 6 GiB GPU KV allocation per GPU, 80 GiB CPU KV offload, prefix caching, MTP2, native Gemma reasoning/tool parsers, vision projector and V2 runner. Sampling was temperature 0.6, top_p 0.9, top_k 64, min_p 0.05, repetition_penalty 1.1; the matched main suite used seed 20261023. Benchmark scripts and fixture bytes were unchanged. `long_suite.py` continued the same long fixture after the short-only phase without repeating performance tests.

Source pins and build:

- Plugin: [PR #141](https://github.com/vllm-project/vllm-gguf-plugin/pull/141), commit `e62877c30519305f3c82dc0d616cf62b5ed67133`.
- Pinned llama.cpp: `002a12ad25503a93501b2e188c360029830a241a`.
- Hauhau source model: `9654466e82d83f5ebfe1518a369bc5900873abb1`.
- Extension SHA256: `e3cefbcc99a4008b96d52daeaddae770dad20b28f27432e18a4fdc894a912548`.
- `upstream-pr141-compatibility.patch` preserves the existing Gemma MTP adapter, assistant’s separate config, vision OIHW→HWC correction and compatible Transformers imports. It changes no CUDA kernels.
- The measured extension is an incremental SM86 build: successful upstream objects from nvcc13.3 plus six remaining objects compiled with 13.0.88 against runtime 13.0. It is not a clean all-13.0 build. `pr141-build-manifest.json` records the failed toolchain attempts and final build; 10 GPU smoke cases passed, including full odd-shaped outputs and sampled actual TP shards.

To reproduce the package, fetch the pinned PR commit and pinned submodule, apply the saved compatibility patch, and build the extension for SM86 in an isolated environment. A clean uniform-toolchain rebuild must receive its own numerical and serving validation. `probe_pr141.py` records graph timings, selected upstream methods, full-output finiteness and FP64-source error. Launch arguments are in `../original-gguf-args.json`; the tested upstream selector is `VLLM_GGUF_CUDA_KERNEL=upstream`. `whole-model-pr141/benchmark.py --variant NAME --short-only`, then `long_suite.py` and `check_behaviors.py`, reproduce the test stages on a fresh process using the supplied fixtures.

Durable release: `/home/isnotreal/model-tools/gguf-pr141-releases/20261023-e62877c-e3cefbc`. Its startup launcher checks every recorded artifact hash. The only new persistent service file is `/etc/systemd/system/qwen-vllm.service.d/zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-gguf-pr141-release.conf`; the trial `/run` override is removed. The 15 preexisting persistent unit files are preserved and snapshotted.

Rollback command: `/home/isnotreal/model-tools/gguf-pr141-releases/20261023-e62877c-e3cefbc/restore-converted.py`. This new helper removes both the persistent PR override and transient trial override, atomically restores all 15 saved converted service files, reloads systemd and starts converted MTP2. The older rollback helper is untouched. `--check` verifies snapshots without mutation.

Persistent activation and post-restart reasoning/tool smoke passed; see `promoted-release-verification.json`.

Limits: the small 1.3% C1 and 4.0% C4 advantage establishes comparable measured speed, not statistical superiority. The long suite tests 63k input, not a new 200k request. Original weights remain original, but A8 activation quantization and floating-point arithmetic still affect outputs. The historical repetition cause remains unknown; the prepared MTP2/off ablation is deferred. Existing PR #141 deserves the kernel-port credit; our complementary results are Ampere validation and Gemma compatibility evidence. No upstream PR or comment has been published from this work.
