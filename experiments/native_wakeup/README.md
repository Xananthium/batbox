# Request-aware waking and repetition recovery

The autonomous engine now records native request lifetimes and local work phases
in `activity.json`. Streaming generation, non-streaming compaction and tokenizer
requests have scoped start/end records, including retries and cancellation.
Coordinator and worker identities share one process monitor. The monitor does
not treat an open keepalive socket or a timer tick as an inference request.

The external watcher runs every 15 seconds. After 30 seconds of eligible idle
time it can queue the owner's wake message for the existing coordinator. The
command includes the process ID, run ID and activity revision. Native acceptance
rechecks these against the in-memory monitor while holding its mutex, persists
the wake checkpoint, then acknowledges the command. An interrupted wake resumes
through the existing durable turn machinery. It does not create another engine
or replay a completed wake. The default cooldown is 300 seconds.

Quiet requests and tool work remain protected within finite deadlines. Expired
requests cannot suppress the 2,100-second stalled-process recovery forever.
The watcher rechecks service identity, queued systemd jobs, heartbeat and request
activity before a restart. Invalid wake files are rejected without preventing a
later valid wake.

## Generation quality

The [evening production audit](../repetition_forensics/20261023-autonomous-followup.md)
found real repeated-word and short-phrase degeneration. Autonomous conversations
now reject the narrow detected pattern as a failed attempt, preserve the raw
response with rejection metadata, and use a recovery marker in active context.
Existing backoff applies. Restoration handles both newly marked rejections and
old repetitive messages, including compacted checkpoints.

The detector requires a short English prose phrase to repeat consecutively at
least 64 times, span at least 128 tokens and occupy at least one fifth of the
response. Code fences, quoted/indented data, numbers and common serialized values
are excluded. Token-limit endings alone are accepted. This is a heuristic, not a
general correctness or progress assessment; intentional unmarked repetition can
look the same as degeneration.

Rejected generated observations are excluded from new memory submission.
Previously queued repetitive observations are quarantined locally. Historical
database records remain intact; recall filtering applies only to explicitly
tagged generated observations. User, tool and source records are preserved.
Full-content validation receipts preserve accepted code/data through later
chunking, where the original fences may no longer accompany each chunk.

These changes address persistence and recovery, not the initial model/backend
cause. They do not establish a quantization or speculative-decoding defect.
Serving context, compaction threshold, sampling, speculative decoding and cache
configuration were not changed.

## Validation

Focused tests cover lifecycle tracking, cancellation, silent requests, tool
execution, idle wake delivery, stale/duplicate commands, service races, and
crashes before and after wake preparation. Repetition tests cover raw transcript
preservation, restored checkpoints, retry backoff, memory handoff, and legitimate
length-ended/code responses. Existing four-connection worker, shutdown,
autonomous lifecycle, API and deployment tests also ran against the change.

These tests use the real native executable with local deterministic HTTP
fixtures. They establish harness behavior, not model answer quality or GPU
throughput. The accompanying validation record distinguishes these tests from
the installed production observation. Raw conversations and credentials remain
outside the repository.

[Validation record](20261023-validation.json) lists the tested cases and binary.
[Production activation](20261023-live.json) confirms the same coordinator and
worker identities, four active native requests, an unchanged backend process,
and the watcher correctly protecting those requests. A naturally idle wake was
not observed in that production snapshot; deterministic native tests exercised
delivery and restart recovery. Updated current-context sources were available
through CPU recall while their HippoRAG refresh continued in the background.
