# include/batbox/skills

Bundled skill registry: parses the 13 embedded .md skill files compiled into the binary at build time.

## Files

### BundledSkillsRegistry.hpp
Registry of the 13 bundled skills embedded at CMake configure time.

- `BundledSkillsRegistry::all() -> vector<Skill>` — parses all 13 embedded .md strings via FrontmatterParser; logs WARN and omits any with malformed frontmatter; returns Skills with source=="bundled"
