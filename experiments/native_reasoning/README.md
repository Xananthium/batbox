# Modern vLLM reasoning progress

The installed vLLM protocol emits structured reasoning as `delta.reasoning`.
Batbox recognized only the older `delta.reasoning_content` spelling. It therefore
received bytes without routing that reasoning through its progress callbacks.
Visible content remained a separate field. The installed protocol/serving source
hashes and the reproducer are recorded in `20261023-modern-field.json`.

The parser now normalizes either spelling into its existing isolated reasoning
channel. A valid modern string takes precedence, including an empty string;
missing, null or malformed modern values can fall back to the legacy field.
It never concatenates duplicate aliases or treats reasoning as visible output.

The modern-field native heartbeat test failed on release `500fc` and passed on
release `8a4dee`. Validation passed 140 unit cases / 487 assertions and two native
worker integration cases, including a silent-stream control. This is protocol
and lifecycle validation; it does not measure model quality or inference speed.

Under shared production load, two active streams reached the 600-second total
deadline after receiving 419,151 and 421,336 bytes. The deployed native deadline
is now 1,800 seconds, with a 600-second idle limit. The watchdog's effective
allowance is 2,100 seconds so it does not preempt a bounded non-streaming summary
request. Cancellation and systemd crash recovery remain active. Backend context,
sampling, speculative decoding and cache configuration were not changed.

The architecture map now refers to the live deployment record for mutable
binary, revision, PID and timeout values. Changing a release no longer requires
re-embedding repeated copies of those values throughout that map.
