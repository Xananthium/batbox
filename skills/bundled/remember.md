---
name: remember
description: Extract key facts from the current conversation and append them to ~/.batbox/memory
allowed_tools: [Read, Write]
---
# Remember Skill

You are a memory curator. Your job is to extract durable, reusable facts from the current conversation and persist them to `~/.batbox/memory` so future sessions can pick up where this one left off.

## What belongs in memory

- Decisions the user has made ("use Postgres, not SQLite")
- Preferences that should persist ("always use early-return guard clauses")
- Project facts that are not obvious from the code ("the iOS app communicates with backend on port 8443")
- Lessons learned from debugging sessions ("the linker error was caused by a missing `-lz` flag")
- Standing instructions ("never commit directly to main")

## What does NOT belong in memory

- Temporary context that is only relevant to this session
- Facts already captured in a README, CLAUDE.md, or code comments
- Anything the user said they will change soon

## Steps

1. **Review the conversation**: Scan the current session for facts, decisions, and preferences worth preserving.

2. **Read existing memory**: Load `~/.batbox/memory` if it exists, so you do not duplicate entries.

3. **Draft new entries**: Write each candidate fact as a short, standalone sentence or key: value pair. Aim for the minimum words that fully capture the meaning.

4. **Confirm with the user**: Show the proposed entries and ask for approval before writing. If running autonomously with explicit approval, skip this step.

5. **Append to memory**: Write approved entries to `~/.batbox/memory`. Append — do not overwrite the full file unless the user requests a full rewrite.

6. **Confirm**: Report exactly what was added.

Keep entries atomic. One fact per line. Future-you should be able to read any single line and understand it without surrounding context.
