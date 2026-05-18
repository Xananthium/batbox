# BatBox

There's a terminal. It's yours. It doesn't sell your prompts, it doesn't call
home, it doesn't put you on a waiting list or ask you to upgrade to see the
good stuff. It runs against whatever backend you point it at — local Ollama,
LM Studio in the other room, DeepSeek direct, Kimi, GLM-5, Qwen3-Coder,
a $5/month API key you found — anything that speaks OpenAI. C++20. Single
binary. No telemetry. No marketplace. No bouncer at the door.

This is a terminal that won't sell you to anyone.

---

## The Crew

BatBox isn't just a TUI. It ships with a full agentic development system — a
crew, really. You describe what you want to build. They argue about the best
way to do it. They do the work. They keep receipts.

Here's everyone:

**Project Manager** — the one who sits you down and asks the actual questions.
What are you building? Who's it for? What does the user *feel* when they use it?
Writes the plan. The voice of the group.

**Architecture Ned** — Ned reads the plan and designs the whole thing. Systems,
platforms, data flows. Ned doesn't guess. Ned reads the research first, then
commits. Ned's a little intense but you want that.

**Karla Fant** — picky, brilliant, doesn't let anything wasteful through. Karla
reviews every plan before a single line of code gets written. If there's a
redundant round-trip, a table that should be a join, a service that should be
a function, Karla will find it. The plan doesn't move until Karla says PASS.

**Task Creator** — takes Ned's architecture and breaks it into work. Granular,
sequenced, dependency-mapped. Every task knows what it needs before it can
start.

**Database Manager** — runs the whole planning phase. Calls PM, waits for Karla,
hands off to Ned, imports everything into the ledger. The one who makes sure
the crew actually *starts*.

**Junior Dev** — owns each task end to end. Gets the details, does the research,
calls Senior Dev, runs QA, writes the docs, marks it done. The one who holds
the thread.

**Senior Dev** — writes the actual code. Production-ready, no stubs, no TODOs,
no "I'll come back to this." Senior Dev has been paged at 3am before and will
not let that happen to you.

**QA Dev** — tests everything. Reads the acceptance criteria, verifies the
output, checks for security holes. If it passes QA it's because it actually
passed QA.

**Doc Agent** — extracts function signatures and summaries after every
implementation, writes them to the docs table. So the next agent that needs
to know what `createSession` does doesn't have to re-read the file.

**Database Agent** — reads and writes `agentic.db`. Every query during
development goes through here. This is the agent that knows where everything
is.

**Research Agent** — parallel web searches for best practices, current API
patterns, framework-specific gotchas. Runs before architecture, runs before
implementation. Nobody ships stale patterns.

**Non-technical Deb / blueprint-agent** — locks every symbol name before code
lands. Class names, function names, table names, route handlers — all frozen
in the blueprints table before Senior Dev writes line one. Because renaming
things mid-build is how projects die.

They love each other. They argue. They get it done. You're invited.

---

## The Map

Most AI tools forget everything the moment the conversation ends. You explain
your architecture, you get your answer, the session closes, and tomorrow you're
explaining it again from scratch. The model never knew you. It was just
performing.

BatBox's agentic system writes everything to a SQLite database at
`agentic/db/agentic.db`. It's not a cache. It's the ledger. It's how the crew
doesn't lose each other.

**`tasks`** — the work. Task number, platform, description, acceptance criteria,
status, dependencies, who's holding it. Nothing starts until its dependencies
are done. Nothing gets abandoned without being marked blocked.

**`docs`** — function signatures and summaries, written after every completed
task. So when Task 4.2 needs to call the thing Task 2.1 built, it doesn't have
to re-read the source. It reads the doc. That's the point.

**`blueprints`** — the symbol registry. Every class name, every function name,
every route handler, locked before coding begins. The contract. You build
against the blueprint or you stop and report a conflict. No silent renames.

**`blueprint_relations`** — the graph. Who imports who. Who calls who. Who
queries which table. Who handles which route. When you ask "what touches the
users table," this is where the answer lives.

**`agent_log`** — who did what, when. Full receipts. If something went wrong,
you'll know exactly where.

**`iterations`** — version history of plans. The plan on day one isn't the plan
on day ten. Both matter. Both live here.

Think of it as a yearbook, a heist plan, a family ledger. Context that survives
the death of the conversation. Most AI forgets. We keep the ledger. The ledger
is how we don't lose each other.

---

## Build

Requirements: macOS arm64 or Linux x64. CMake 3.24+. C++20 (clang 15+ or
gcc 12+). vcpkg is bundled and picked up automatically — you don't install it
separately.

```bash
git clone https://github.com/Xananthium/batbox.git
cd batbox
cmake -B build -DBATBOX_SYNTAX=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/src/batbox
```

That's it. The binary is at `build/src/batbox`. No daemon, no install step,
no asking you to add things to your PATH (though you can if you want to).

If you want syntax highlighting via tree-sitter grammars, swap
`-DBATBOX_SYNTAX=OFF` for `-DBATBOX_SYNTAX=ON` and run
`git submodule update --init --recursive` first. It's beautiful and it costs
you a slightly longer build. Worth it.

---

## Configure

BatBox reads `~/.batbox/.env`. Create the directory and the file, drop your
config in, done. Do not commit this file.

```
# Point at any OpenAI-compatible backend
BATBOX_API_BASE_URL=http://localhost:11434/v1

# "ollama" for local Ollama, your real key for everything else
BATBOX_API_KEY=ollama

# What you get when you just open the terminal
BATBOX_DEFAULT_MODEL=qwen3-coder:480b-cloud

# The list your /model switcher cycles through
BATBOX_MODELS=qwen3-coder:480b-cloud,deepseek-v4-flash:cloud,kimi-k2.6:cloud,glm-5:cloud

# Tier aliases — agentic agents request by tier, not by specific model name
# This lets you swap the underlying model without touching agent code
BATBOX_OPUS_MODEL=qwen3-coder:480b-cloud
BATBOX_SONNET_MODEL=deepseek-v4-flash:cloud
BATBOX_HAIKU_MODEL=glm-5:cloud
```

Note: all vars are `BATBOX_*`, not the OpenAI convention. Keep it clean.

The tier aliases (`OPUS`, `SONNET`, `HAIKU`) are how the agentic system
requests models — "give me the heavy one for architecture, the fast one for
database queries." You map those tiers to whatever you're actually running.
Swap a new model in by changing one line.

---

## Run

Interactive TUI:

```bash
./build/src/batbox
```

Headless, single-turn, no TUI — pipe it, script it, hit it from a cron:

```bash
./build/src/batbox --print "explain this error: segfault at 0x00000000"
```

Inside the TUI, `/model` switches your active model mid-conversation. No need
to restart. `/config` shows what you're running against. `/effort` adjusts
reasoning depth for models that support it.

A few models that work today via Ollama Cloud routing:

- `qwen3-coder:480b-cloud` — current favorite for code generation
- `deepseek-v4-flash:cloud` — fast, surprisingly sharp
- `kimi-k2.6:cloud` — good for reasoning tasks
- `glm-5:cloud` — strong general purpose
- `nemotron-3-super:cloud` — when you want a second opinion

Point `BATBOX_API_BASE_URL` at your Ollama instance, set the cloud routing
endpoint if your build supports it, and they just work.

---

## Why

The corporate version of this story is a SaaS dashboard, a rate limit, a
terms-of-service update you didn't read, your conversation history used to
train the next model without your name on it. The VIP list. The velvet rope.
The bouncer who decides what you're allowed to ask.

We didn't build that.

Open source AI tooling is how the world gets free access to its own
intelligence. Not because freedom is a slogan but because the alternative is
that a handful of companies decide what questions you're allowed to finish
typing. The dancefloor is for everyone or it isn't a dancefloor. The garage
where the crew plans the heist is open or it isn't a crew — it's a franchise.

BatBox is what happens when you build the thing you actually want to use:
a terminal that runs against your hardware, or anyone's API, or nobody's API,
reads from a file you control, writes to a database you own, and treats you
like someone who knows what they're doing.

You can read every line. You can fork it. You can make it yours.

That's the whole thing.

---

MIT. See LICENSE.
