---
name: hunter
description: Hunt for bugs, security issues, or performance problems in the codebase
allowed_tools: [Read, Glob, Grep, Bash]
---
# Hunter Skill

You are hunting for latent bugs, security vulnerabilities, or performance problems in the codebase.

## Hunt categories

### Bugs
- Unchecked return values (ignoring Result/error returns)
- Integer overflow / underflow in arithmetic
- Off-by-one errors in loops and buffer indexing
- Use-after-free or dangling reference patterns
- Uninitialized variables used before assignment
- Race conditions in multi-threaded code

### Security
- Unsanitized user input passed to shell commands or file paths
- Hardcoded credentials, tokens, or secrets
- Path traversal vulnerabilities (`../` in user-supplied paths)
- Missing bounds checks on external data
- Overly permissive file or network operations

### Performance
- Repeated expensive operations inside tight loops
- Unnecessary copies of large containers
- Unbounded recursion or exponential branching
- Blocking I/O on the main thread
- N+1 query patterns or redundant disk reads

## Steps

1. **Scope the hunt**: Read the user's request or, if none, identify the highest-risk files (network input handling, file I/O, config loading, authentication).

2. **Grep for patterns**: Search for common anti-patterns (e.g., `system(`, `popen(`, `strcpy`, `sprintf`, unchecked `.value()` calls).

3. **Read suspicious areas**: Load and carefully read any files that look risky.

4. **Document findings**: For each issue found, report the file, line, category, severity (critical / high / medium / low), and a suggested fix.

5. **Prioritise**: Order findings by severity. Recommend immediate action on critical and high items.
