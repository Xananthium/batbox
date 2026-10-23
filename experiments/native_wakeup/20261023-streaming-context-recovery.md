# Streaming and restored-context follow-up

The first generation guard waited for the final response. Subsequent private
wire capture confirmed all four original native streams emitted repeated `our`
content and reached their 4,096-token limits. This was real backend SSE content,
not a display or parser artifact. Two saved tool calls were also inspected:
`./build/test_session_file` failed because the tools run in the private workspace;
`find build -name "test_session_file"` was valid JSON and returned no matches.
The latter's truncated display preview did not establish malformed arguments.

The captured coordinator had roughly 96,000 input tokens. Its generated recall
payload echoed a previous repetitive response in a top-level `query` field,
inside a user-role native operating prompt. Filtering assistant messages alone
could not remove that path. An independently captured worker request contained
no whole-word `our` in its message contents, yet also repeated the word in its
response. Prompt contamination therefore does not fully explain the backend
failure. Separate controlled speculative-decoding measurements follow this
investigation; no causal conclusion is inferred from acceptance counters alone.

## Harness changes

Visible streams now use the existing narrow prose classifier at bounded byte
intervals. A detected loop cancels only the current inference request. Exact
received partial content is saved with rejection and stream-abort metadata,
then the existing retry backoff applies. Completed tool results remain durable;
an aborted incomplete tool intent is not executed. Ordinary user cancellation,
code/data output and token-limit endings retain their existing behavior.

Retrieval queries no longer append a previously rejected repetitive final.
Native prompt metadata identifies generated recall byte ranges through durable
messages and compaction checkpoints. Those ranges discard the service's echoed
query and filter explicitly tagged generated loops while preserving actual
user, tool and source records. Legacy repair requires an autonomous session and
the exact native envelope; it does not search arbitrary user messages for text
to remove. Prepared-tail comparison uses equally repaired copies, preserving
append-once recovery without rewriting raw history.

New cycle prompts identify the tool workspace and absolute source/build paths.
They do not change the process working directory or the model/context settings.

## Validation

Binary SHA-256:
`ee9f6cd28aff2284fb40f7343a1181fb96ccbb3c24443ae3e456aac77cb0187a`.

| Check | Result |
|---|---|
| Generation quality/context unit tests | 8 cases, 94 assertions passed |
| Streaming unit tests | 3 cases, 10,247 assertions passed |
| Generation quality/context native integration | 11 cases passed |
| Streaming native integration | 3 cases passed |
| Existing affected worker, shutdown and lifecycle tests | 16 cases passed |
| Native wake/crash-boundary regressions | 10 cases passed |

The streaming fixture disconnected before 512 sent fragments and in under four
seconds. It verified exact saved partial text, at least nine seconds of retry
backoff, one execution of an earlier Bash action, accepted fenced/serialized
data, and ordinary SIGTERM handling. This is fixture timing, not production
generation speed.

An isolated copy of the real private production state was replayed against a
local deterministic endpoint without executing tools. Its one outgoing model
request contained no detected loop text after repair; all 145 raw records,
10 recalled records and five ordered tool-result correlations were preserved.
Two native recall prompts were repaired. The three central synthetic regression
cases failed on the previous binary before passing on the new one.

The private wire captures and real state copy remain outside this repository.
These tests validate harness cleanup and cancellation, not a cure for the
underlying model/backend degeneration or a speed advantage from speculation.
