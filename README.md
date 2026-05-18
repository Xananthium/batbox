# BatBox

Okay so listen. You use Claude Code. Or Codex. Or Cursor. Or Aider. They're
good. They're, like, genuinely good. But every single one of them is a
frontend that wires to exactly one company's API, and that company decides
what you can ask, how fast you can ask it, and whether your prompts get used
to train the next model. And like — that's fine, that's their business model,
nobody's mad — but what if you didn't want that? What if you wanted the
frontend without the company?

That's BatBox. It's a terminal that talks to AI. It does what those tools do.
It just doesn't belong to anybody.

```
you ──→ BatBox ──→ any OpenAI-compatible endpoint ──→ any model
```

That's the whole thing. Local Ollama on your laptop. LM Studio on the machine
in the closet. DeepSeek. Kimi. GLM. Qwen. OpenAI if you really want. A janky
$3/month API key you found on the internet. If it speaks OpenAI-compatible
HTTP, BatBox speaks back. C++20. Single binary. No daemon, no account, no
phone-home, no marketplace, nobody between you and the model.

This is the version of the tool where you own the wire.

---

## What BatBox Actually Is

A frontend. A CLI. A chat interface in your terminal. The TUI is FTXUI (real
terminal rendering, not curses garbage). The HTTP is cpr. The streaming is
SSE parsed inline so tokens show up as the model produces them. Slash
commands for model switching, config inspection, reasoning depth, permission
modes. Tool calling works. The binary is ~4MB.

It does not ship agents. It does not pretend to have opinions about your
codebase. It does not run a coordination layer for you. **The agents are
yours to build.** BatBox is the frontend they talk through.

What BatBox *does* ship is a schema. A pattern. A way of tracking what's in
your codebase so the agents you write don't have to re-read everything every
time they wake up.

That's the map.

---

## The Map (Build Your Own Agents, Give Them This)

Here's the thing nobody talks about. Every AI coding tool has the same
amnesia. You explain your architecture, you get your answer, the session
ends, the next session is square one. The model never knew you. It was
performing. Whatever the agent figured out about your codebase yesterday is
gone today.

BatBox ships a SQLite schema for **mapping your codebase**. Not the model's
memory — your project's memory. A database your agents can query.

It lives at `agentic/db/agentic.db` in any project you wire it up to. The
tables are opinionated:

**`blueprints`** — every function, class, table, route in your codebase.
File path, symbol name, signature, short pseudocode summary. The shape of
what exists.

**`blueprint_relations`** — the graph. Who imports who. Who calls who. Who
queries which table. Who handles which HTTP route. "What touches the users
table?" becomes one SQL query.

**`docs`** — function signatures plus terse summaries, written after each
function is implemented or audited. So an agent that needs to call
`create_session()` reads the doc, not the source.

**`tasks`** — work the user wants done. Number, description, acceptance
criteria, status, dependencies. Nothing starts until blockers are done.

**`agent_log`** — receipts. Who did what, when.

**`iterations`** — plan history. The plan on day one isn't the plan on day
ten. Both matter.

This isn't bundled agents. This is a **shape for context** your agents can
use. Build a PM agent and have it write to `tasks`. Build an ingest agent
and have it populate `blueprints`. Build a Q&A agent and have it query
`blueprint_relations` when the user asks "where do we hash passwords."

Or don't. Just use BatBox as a chat client. The DB is there if you want it.

---

## Why Not Just Use Claude Code

Use Claude Code. I use Claude Code. It's great. So is Codex. So is Cursor.
This is not a hit piece. This is the part where I tell you what's different.

Every one of those tools is wired to one vendor. The agent's behavior — when
to read a file, when to run a command, how to break down work — is theirs.
You can system-prompt around it but you can't really change it. The model
list is theirs. The pricing is theirs. The roadmap is theirs.

BatBox wires to whatever. The source is on your disk. Want to add a slash
command? `src/commands/`, copy the next one over, ~100 lines. Want to change
how tool cards render? `src/tui/ChatView.cpp`. Want a different streaming
protocol? `src/inference/Client.cpp`, ~200 lines, you can read all of it in
one sitting. The agents you build against the map are yours and they do
what you tell them to do.

It's also fast because it's C++ and not Electron. But that's not the pitch.
The pitch is: it's yours.

---

## Build

macOS arm64 or Linux x64. CMake 3.24+. C++20 (clang 15+ or gcc 12+). vcpkg
is bundled and picked up automatically.

```bash
git clone https://github.com/Xananthium/batbox.git
cd batbox
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/src/batbox
```

That's it. Binary at `build/src/batbox`. No install step, no PATH surgery
required, you can copy it anywhere.

Syntax highlighting in code blocks uses hand-rolled lexers (cpp, python, js,
ts, rust, go, html, css, json). No external grammar dependencies. No
submodules to init. We don't pull in libraries when C++ can do it.

---

## Configure

BatBox reads `~/.batbox/.env`. Make the directory, drop the file in, you're
done.

```
# Any OpenAI-compatible endpoint
BATBOX_API_BASE_URL=http://localhost:11434/v1

# "ollama" for local, your real key for hosted
BATBOX_API_KEY=ollama

# What you get on cold start
BATBOX_DEFAULT_MODEL=qwen3-coder:480b-cloud

# The list /model cycles through
BATBOX_MODELS=qwen3-coder:480b-cloud,deepseek-v4-flash:cloud,kimi-k2.6:cloud,glm-5:cloud
```

See `.env.example` in the repo for 7 working configurations (local Ollama,
Ollama Cloud, LM Studio, OpenAI, DeepSeek, Moonshot, OpenRouter). The env
var prefix is `BATBOX_`, not `OPENAI_` — that was intentional, keep the
namespaces clean.

---

## Run

Interactive TUI:
```bash
./build/src/batbox
```

Headless one-shot — pipe it, script it, cron it:
```bash
./build/src/batbox --print "explain this stack trace: <paste>"
```

Inside the TUI:

| Command | Effect |
|---|---|
| `/model` | Switch model mid-conversation, no restart |
| `/config` | Show current backend + active model |
| `/effort` | Reasoning depth for models that support it |
| `/nuclear` | Bypass permission prompts (use carefully) |
| `/clear` | Reset conversation |

Tool calling works against any model that supports OpenAI tool-call format.
If your model emits its tool calls in a finish_reason loop, BatBox handles
the whole multi-turn dance.

---

## Why

I think about this a lot. There's a version of every AI tool you've ever
loved where some company eventually sits between you and the model. They
cache your prompts. They train on your code. They deprecate the model that
worked best for you. They rate-limit you to upsell you to a tier. They get
acquired and the new owner changes the terms and your old work is part of
a different company's training corpus now.

That's not a conspiracy. That's just what happens. The economics push every
hosted AI tool toward that endpoint. It's not anybody's fault. It's just
where the gravity is.

BatBox is what happens if you build the version that doesn't go there. The
version that's a binary on your disk that talks to whatever endpoint you
point it at and does literally nothing else. The version where the database
of what's in your project is in *your* project. The version where you build
your own agents because nobody else gets to decide how they think.

Open source AI tooling is how this actually stays good for people. Not the
keynote-friendly version of "open" where the weights are downloadable but
the only frontend is the company's. Real open — the source on your disk,
the binary you compiled, the agents you wrote, the database you own, the
model you picked.

You can read every line. You can fork it. You can break it. You can make
it do whatever you want.

That's the whole thing. Welcome.

---

MIT. See LICENSE.
