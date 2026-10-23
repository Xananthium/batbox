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

CPU validation passed: 4 archive cases / 158 assertions, 22 conversation cases /
326 assertions, 2 worker lifecycle cases, 9 autonomous lifecycle cases, 3 shutdown
cases, and 6 API cases. The actual 1,048,580-byte worker result became a
17,285-byte active view with 17 exact pages. These are byte measurements, not
backend token counts. See `20261023-cpu-validation.json`.

Production release `500fc1120de216b337315501b4b1409d8d0a06934118f87260caa74ce71e95ac`
was activated at 17:58 UTC. Batbox/API restart took 2.069 seconds and left the
inference backend process unchanged. Both formerly stalled workers resumed
durable tool execution with their original identities: five maintainer receipts
and three verification receipts by 18:03. Their first receipts arrived 108.5 and
126.7 seconds after restart. Original transcript hashes and private artifacts
matched; all pages reconstructed the exact outputs.

`20261023-after-worker-recovery.json` records those observations. Neither worker
had finished its full turn at that snapshot. The server was handling additional
proxy traffic: four native, one memory, and seven proxy connections, with 10–12
running inference requests in nearby metrics. These recovery timings are under
shared production load, not isolated serving benchmarks. Tool receipts establish
execution; they do not prove an autonomous source improvement or deployment.
