# Large tool output and independent worker recovery

The October 23 production audit found two standing workers repeatedly entering
long requests after a recursive repository listing. Each persisted Bash result
contained about one MiB and 23,900 lines, including build dependencies. Neither
worker had completed a turn; both subsequently recorded a 600-second timeout.
The coordinator also timed out after receiving 510,886 streaming bytes during a
600-second request. These observations are in `20261023-before.json`; the raw
session content remains private.

The backend stayed active and other requests completed. This incident therefore
does not establish that inference had crashed, nor that the isolated short
benchmark throughput describes this mixed production workload. No blocked-process
backtrace was captured. The oversized-context mechanism is supported by the
persisted inputs, code path, and targeted reproductions.

The repair preserves the 200,000-token context setting and exact 100,000-token
compaction threshold. A tool result of at least 512 KiB is persisted in full,
then represented in active context by a head/tail preview, content hash, and
durable file references. Exact pages let the model retrieve relevant material
through Read, including long lines or files exceeding Read's size limit. Existing
worker histories receive the same view when restored. Failed artifact publication
keeps the full active result; the original transcript is never replaced by the
preview.

Worker recovery runs independently of the coordinator, with bounded exponential
backoff and the same durable worker/session identities. Worker journals record
actual context, tool, streaming, and completion activity. Received reasoning
deltas can refresh progress; a blocked request cannot manufacture a heartbeat.

Production activation and post-repair observations will be recorded separately
from CPU regression results. A successful model response does not by itself
prove that the autonomous worker changed source code or deployed an improvement.
