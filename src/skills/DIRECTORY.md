# src/skills

Bundled skills registry implementation and build-time embedding infrastructure.

## Files

### BundledSkillsRegistry.cpp
`BundledSkillsRegistry::all()` implementation: calls parse_frontmatter() on each of the 13 embedded .md strings (injected by embed.cmake as constexpr string literals); converts to Skill structs with source="bundled"; logs WARN and skips malformed entries.

### embed.cmake
CMake configure-time script: reads each .md file in src/skills/data/; generates a C++ source file with constexpr string literals; wires generated source into the skills library target.

### CMakeLists.txt
Build rules for the skills static library; invokes embed.cmake.

## Subdirectories

### data/
Embedded skill .md source files:

- `hunter.md` — "hunter" skill: orchestrates multi-source web research on a topic
- `loop.md` — "loop" skill: iterates a prompt template across a list of inputs
- `schedule-remote-agents.md` — "schedule-remote-agents" skill: dispatches multiple sub-agents on a cron-like schedule
