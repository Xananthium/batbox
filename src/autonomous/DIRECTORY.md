# Autonomous runtime

`Engine.cpp` owns the persistent native engine lifecycle. It reuses `Conversation`,
`Client`, `SessionStore`, and the full registry constructed by `App::wire_tools`.
The engine has its own singleton lock, crash marker, checkpoints, issue inbox and
replies, generated-observation spool, and scoped HTTP adapters for the memory and
communication services. Native tool access uses unattended permission mode.

The session transcript is authoritative for model messages and tool receipts.
`state.json` stores cycle/input-delivery state; `memory-outbox/` holds independent
durable handoff records until the background memory service acknowledges them.
Malformed inbox files move to `rejected/`; they cannot block later valid input.

Backend-tokenized compaction is delegated to `Conversation` at 100000 tokens.
The generated workspace `BATBOX.md` is stable to preserve prefix-cache reuse;
letters, retrieved observations, crash evidence and live server context enter as
tail messages. Inherited notes remain attributed historical records.
