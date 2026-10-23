# Runtime and model evidence

Experiments keep measured outcomes separate from implementation hypotheses.
Raw user transcripts, credentials, model weights, and live session state stay
outside the repository. Local third-party checkouts are experimental build
inputs, not authored Batbox code.

* [Native shutdown](native_shutdown/README.md): cancellation regression and
  restart tests, including preservation of worker IDs and transcripts.
* [Memory recovery](memory/20261023-graph-recovery.json): reconciliation of an
  interrupted legacy graph insert; checkpoint/replay tests live in
  `python-sidecar/tests/memory`.
* [Repetition forensics](repetition_forensics/README.md): actual historical
  counts, missing evidence, and isolated sampling checks.
* `native_compaction/live_100k.py`: a live installed-native compaction fixture
  with a 200,000-token serving window, 100,000-token trigger, original
  transcript retention, stable system/tools, and restart verification.
  Preparation is CPU-only; `--run` explicitly enables inference. A completed
  passing report is required before claiming the live check passed.
* `backend/20261023-qat-format`: paired format, fidelity, cache, quality, and
  kernel experiments. Aggregate concurrent throughput and per-request speed
  are distinct measurements. Kernel microbenchmarks are not serving speed.
* [KV-CACHE-CHUNKING](https://github.com/Xananthium/KV-CACHE-CHUNKING): the
  dedicated research repository for immutable shared KV chunks, generated
  reasoning summaries, and the Gemma4/TurboQuant adapter contract. CPU
  prototypes do not establish support in the production backend.

The runtime's installed commit and current service state are recorded in the
private deployment manifest. A previously successful test does not imply
that a service is running while a maintenance experiment is active.
