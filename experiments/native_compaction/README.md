# Native 100k compaction smoke test

The final October 23 live run **passed the compaction, retention, transcript,
stable-prefix, and restart checks**, with an offline correction to a harness
false positive described below. It used installed native binary SHA
`7775ff66c98a9f6ef0b98ca2339059dc9e304013852e9a6dcdc1f0cb19636aa7`
against the promoted original Hauhau Gemma4 GGUF backend: 200k context, TQ4,
TP2, MTP2, default thinking, normal 4,096-token answer limit, and native
20,000-token summary limit. The backend release and extension hashes are in
[the sanitized result](20261023-passed-result.json).

- The exact native prompt crossed the 100,000-token trigger at **110,526**
  tokens. Two large summary chunks and a model-generated merge reduced the
  active prompt to **8,604** tokens.
- The durable fact and Python compatibility requirement survived in the
  checkpoint and both answers. All **16 original messages** remained unchanged
  in the persisted transcript; the fixture project remained unchanged.
- A second native process used the same checkpoint, without another summary
  call. Its prompt was **8,734** tokens. System and all **50 tool schemas** had
  identical hashes throughout.
- The initial request contained all **3,400 complete archive rows**. Every
  compacted/restarted request contained **zero** complete rows and no full
  original archive text.
- Phase one took **808.72 seconds** and restart **4.51 seconds** under shared
  production load. Production workers and background memory work ran
  concurrently, including a brief production-engine stop/restart. These are
  correctness-run timings, not isolated performance measurements.

The original harness finished both native phases and then failed a final
substring assertion: a valid summary mentioned `ARCHIVE_ROW_` labels twice.
The corrected predicate detects complete synthetic records. The original
private report, requests, responses, and execution hash are preserved;
`validate_completed.py` rechecked the saved evidence without making any new
inference requests. The result records the original failure, corrected harness
and validator hashes, and exact archive evidence. A focused regression covers
both complete rows and summary label mentions.

Both answers contained the correct JSON fields inside Markdown fences.
Therefore the separate “JSON only” formatting instruction was **not** obeyed;
`strict_answer_format_passed` is false. The pass above concerns native
compaction and preservation of the requested fact and compatibility constraint.

To repeat the offline checks on the preserved private fixture:

```bash
python3 -m unittest discover -s experiments/native_compaction -p 'test_archive_detection.py' -v
python3 experiments/native_compaction/validate_completed.py \
  --directory /path/to/completed/private-fixture \
  --output /path/to/revalidated-result.json
```

The earlier October 23 live run was interrupted at the user's request to give the GPU
to kernel profiling. It is **not a completed pass or a compaction failure**.
Native preflight counted 110,534 tokens with 50 tools and a 200,000-token server
context. The first model summary completed: 51,290 input tokens, 901 output
tokens including 655 reasoning tokens, and both test facts retained. Its wall
time was 356.87 seconds under shared-service load; that is not a kernel timing.
The next segment was counted, but no checkpoint or restart check completed.
See `20261023-interrupted-result.json`. The harness subsequently gained explicit
signal/interruption reporting; the result records the hash actually executed.

`live_100k.py` exercises the installed Batbox autonomous binary against a live
model. It seeds a valid persisted session, lets native token counting trigger
compaction at 100,000 tokens, and starts a second native process to check that
the saved checkpoint is used on restart.

Preparation uses the deployed model's `tokenizer.json` locally. The synthetic
archive contains at least 102,000 tokens of disposable sensor records. A durable
access phrase and a Python compatibility requirement appear only in an earlier
user message, before that archive. The recent conversation and the two recall
questions do not repeat their values. Assertions check the summary and both
answers, preservation of all seeded transcript messages, a single persisted
checkpoint, the 200,000-token model context, and stable system/tool hashes.

Run preparation and live inference separately, with a fresh private directory:

```bash
/home/isnotreal/vllm-env/bin/python experiments/native_compaction/live_100k.py \
  --directory "$TEST_DIRECTORY"
/home/isnotreal/vllm-env/bin/python experiments/native_compaction/live_100k.py \
  --directory "$TEST_DIRECTORY" --run
```

The default paths target this deployment; `--binary`, `--tokenizer`, `--model`,
and `--env-file` allow another installation. The environment file supplies the
upstream API address and authentication without recording credentials. Native
requests retain the full tool schemas and system prompt. Thinking is left at
the provider default, normal answers allow 4,096 tokens, and the native
compactor's own settings remain unchanged. The fixture runs zero child workers
and one successful cycle per process.

A local proxy forwards request bodies unchanged and records them privately.
Memory ingestion and recall use an empty local stub. Responses are buffered;
any new tool intent is rejected before native dispatch. This makes the test
unsuitable for measuring streaming latency or real memory-service behavior.
The large fixture, state files, requests, responses, and logs stay outside Git.
Only the harness and a sanitized result belong here.

The test checks two concrete retained facts in one synthetic history. It does
not establish general summarization accuracy, concurrency throughput, or
long-running stability. Native textual checkpoint compaction is separate from
the experimental KV chunk bank.
