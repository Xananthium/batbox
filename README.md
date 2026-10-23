# BatBox

BatBox now also runs as a persistent autonomous system. `--autonomous` owns a
durable conversation, processes issues, uses the native tools and subagents,
retrieves source-backed memory, and continues work between user messages. The
interactive harness remains available. The model, source, memory, credentials,
and operating decisions stay under your control.

```
you ──→ BatBox ──→ any OpenAI-compatible endpoint ──→ any model
```

[Build](#build) · [Configure](#configure) · [Run](#run)

## The Kitchen Table Journals

Original fiction for this project, inspired by Octavia E. Butler's
*Parable of the Sower* and continuing the Southern California journals on the
two REDCELL model cards. The [implementation guide](#what-batbox-actually-is)
follows the story.

| Part | Journal date | Story | Project |
|---|---|---|---|
| 1 | October 14, 2027 | [The whole seed](https://huggingface.co/Xananthium/REDCELL-26B-A4B-OSINT-Cyber-Abliterated-Pilot-BF16-Vision#the-whole-seed) | Complete BF16 model |
| 2 | October 15, 2027 | [What we could carry](https://huggingface.co/Xananthium/REDCELL-26B-A4B-OSINT-Cyber-Abliterated-Pilot-W6A8-Vision#what-we-could-carry) | W6A8 model |
| 3 | October 16, 2027 | [The work between us](#the-work-between-us) | BatBox |
| 4 | October 18, 2027 | [What remembering costs](https://github.com/Xananthium/KV-CACHE-CHUNKING/blob/main/README.md#what-remembering-costs) | KV-cache research |

### The work between us

**October 16, 2027**

Mrs. Ruiz brought the schedule back on an envelope. She had crossed out the
first hour and written two names beside the second.

"The water truck comes then," she said. "If I go, who gets my time?"

I had divided the afternoon into equal pieces. It had looked fair on the
screen. I had not put the water line on it, or the walk home with the cans,
or the people who had to be watched while somebody else went out.

Tessa had three repairs beside her name. Her electrical lessons were at the
bottom, in a space I had marked *if time permits*.

She tapped that space with her fingernail.

"Whose time?"

I said we could change it. She waited until I did.

The smaller model was running under the kitchen table. For weeks I had been
working on a terminal program to use with it. I called the program BatBox.
Until this morning, I had mostly used it myself.

Mrs. Ruiz pulled her chair close enough that our knees touched. I showed her
where to type, where the answer appeared, where the files stayed. She asked
me to close it and open it again. She wanted to see whether yesterday's work
would still be there.

It was. She read the last exchange slowly. Then she found a mistake in it.

We had been gathering instructions for the pump behind her house. The model
had found a clear diagram in one of our saved manuals. I had marked the page
as useful. Mrs. Ruiz said the drawing showed a different pump. Hers had been
replaced before her youngest grandchild was born.

"You wrote down the old one," she said.

I looked at my note. She was right. The machine had carried my mistake along
very neatly.

Tessa went out with her to read the plate on the casing. When they came back,
I added the number, the date, and the photograph they had taken. We kept the
old note and marked why it had changed. I wanted the next person to know
which pump the instructions belonged to.

Then I began assigning the rest of the work.

Tessa could check the diagram. Tessa could look over the parts. Tessa could
teach someone how to make the repair. The program could keep the list and
show us what remained. I was pleased with how quickly it was coming together.

My sister pushed her chair back.

"You have made a machine that gives me work."

"It gives all of us work."

"Read the names."

I read them. There was a great deal under hers.

Mrs. Ruiz folded the envelope along its old crease. She said she could sit
with the children while Tessa studied. One of the adults in her house could
take my turn in the water line tomorrow. I could go today, and I could learn
enough about the records to teach somebody besides my sister.

Tessa wanted her hour of lessons first. She also wanted whoever learned the
repair to stay for the cleaning afterward.

We put those things on the list. The list grew longer. Nobody had become
faster.

Before we left for the truck, I set BatBox to go through the manuals we had
chosen, record their page references, and list the measurements we still
needed. It could do that work while I carried water. We would check what it
found when we returned. A finished search would not mean a finished repair.

Mrs. Ruiz asked whether it would read everything on the drive.

I had been about to offer her a copy of the whole working folder. Her
employment papers were in it, beside the pump photograph. She had brought
them yesterday because she wanted help reading a deduction.

We moved the papers out of the shared folder. I showed her what would be
copied. She read the names of the files before she said yes.

The truck was late. By the time I got back, Tessa was at the keyboard,
working through an electrical lesson we had put together from the manuals.
There was a page of notes beside her, written in her small, crowded hand.
She covered it when I leaned over to see.

"My hour," she said.

I put the cans away. The pump could wait until she was finished. I had not
known how much I expected her to yield until she stopped yielding it.

Tonight we each have a copy of the shared work. Mrs. Ruiz has learned how to
add a question without waiting for me. Tessa has written her study time into
tomorrow's schedule in ink.

There is still an open task beside the pump: check the instructions against
the machine. I left it open. Beside it I put my name, with Tessa's agreement
to show me where to begin after her lesson.

[Previous: What we could carry](https://huggingface.co/Xananthium/REDCELL-26B-A4B-OSINT-Cyber-Abliterated-Pilot-W6A8-Vision#what-we-could-carry)
· [Next: What remembering costs](https://github.com/Xananthium/KV-CACHE-CHUNKING/blob/main/README.md#what-remembering-costs)

---

## What BatBox Actually Is

A frontend. A CLI. A chat interface in your terminal. The TUI is FTXUI (real
terminal rendering, not curses garbage). The HTTP is cpr. The streaming is
SSE parsed inline so tokens show up as the model produces them. Slash
commands for model switching, config inspection, reasoning depth, permission
modes. Tool calling works. The coordinator and workers run in the native binary.

The interactive client is still a harness. Autonomous mode adds a coordinator
and persistent native workers using that same conversation and tool machinery.
**The agents remain yours to configure.** They use your selected endpoint and
your files; there is no required hosted coordination service.

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

### Autonomous operation

Configure a local OpenAI-compatible backend in `~/.batbox/engine.env`:

```dotenv
BATBOX_API_BASE_URL=http://127.0.0.1:8001/v1
BATBOX_API_KEY=local
BATBOX_DEFAULT_MODEL=your-served-model-id
BATBOX_PROVIDER_HINT=openai
BATBOX_CTX_LEN_DEFAULT=200000
BATBOX_EXACT_COMPACTION_TOKENS=100000
BATBOX_TASK_PARALLEL_LIMIT=4
```

For this server's Hauhau Gemma4 deployment, set `BATBOX_TEMPERATURE=0.6`
and `BATBOX_TOP_P=0.9`. The backend supplies `top_k=64`, `min_p=0.05`,
and `repetition_penalty=1.1`, matching the
[model card](https://huggingface.co/HauhauCS/Gemma4-31B-QAT-Uncensored-HauhauCS-Balanced-MTP#recommended-sampling).
Other models should use their own sampling configuration. Leave
`BATBOX_ENABLE_THINKING` unset to inherit the model's chat-template default;
explicit `true` or `false` is applied equally to token counting and generation.
Conversation summaries use a separate deterministic summarization request.

Run the engine:

```bash
./build/src/batbox --autonomous --env ~/.batbox/engine.env \
  --engine-project-root "$PWD" --engine-state-dir ~/.batbox/autonomous \
  --engine-letter-file ~/love_letters.txt
```

Autonomous operation uses the full native tool registry in unattended permission
mode. The coordinator polls its durable inbox, reads changed letters, retrieves
relevant memory, and advances its current goal. Source inspection, code changes,
tests, and deployment are available to it. Tool intent and results are persisted
so recovery can distinguish recorded actions from interrupted actions.

Standing workers recover independently of a busy coordinator, retaining their
identities and sessions with bounded retry backoff. Tool outputs of at least
512 KiB remain complete in the transcript and private hashed artifacts; active
context receives a head/tail preview and paths for reading exact pages. This also
applies to restored histories, so an oversized directory listing does not force
every recovery attempt to summarize the same output again.

Autonomous conversations also detect extreme consecutive repetition of a short
prose phrase. Rejected output stays in the raw transcript, while active context
receives a recovery instruction and the existing retry backoff applies. The
engine excludes these generated observations from new memory submissions and
filters explicitly tagged repetitive generated memories from recalled context.
Historical memory records are retained. This is a narrow heuristic: ordinary
token-limit endings, user messages, tool results and fenced code are not rejected
merely for being long or repetitive. It does not diagnose the model or backend
cause of degeneration.

With a backend supporting `/tokenize`, exact compaction measures the same
messages, tool schemas, and template options as generation. At 100,000 tokens,
the configured model summarizes earlier context and retains recent complete
tool exchanges. Failed or truncated summaries do not discard history. A separate
checkpoint restores compacted context; the original transcript remains available.
Stable system instructions and tool definitions let the backend reuse prefix
cache blocks across turns and compactions. Cache residency still depends on the
serving backend; arbitrary middle-chunk KV fusion is a separate experiment in
[KV-CACHE-CHUNKING](https://github.com/Xananthium/KV-CACHE-CHUNKING).

The [memory service](python-sidecar/batbox_memory/README.md) provides an immediate
durable queue acknowledgment, then computes CPU embeddings and indexes HippoRAG
in the background. Memories retain source URI, content hash and capture time.
Imported Heather/Maya memories are explicitly historical and attributed. Current
server and program snapshots are separate, dated sources. Named Scrapling
sessions preserve authenticated HTTP/browser state privately across restarts.

The issue API runs with `python3 tools/engine_api.py`. Its listener remains
available while the engine controls whether communication is enabled. The API
uses the local `~/.batbox/autonomous/api.key` credential and exposes:

| Endpoint | Purpose |
|---|---|
| `GET /v1/capabilities` | API location, enabled state and supported lifecycle |
| `GET, POST /v1/issues` | List or create durable issues |
| `GET, PATCH /v1/issues/{id}` | Read an issue or change its status |
| `POST /v1/issues/{id}/comments` | Queue a message to the engine |
| `GET /v1/status` | Current engine status |

Status distinguishes the coordinator's saved snapshot from fresh worker journals,
including actual progress, phase and recovery attempts. Reading status does not
refresh a heartbeat.

Issues support `backlog`, `todo`, `in_progress`, `in_review`, `done`, `blocked`,
and `cancelled`. Replies become issue messages; answering does not automatically
close an issue. Use the local CLI without handling credentials manually:

```bash
python3 tools/engine_chat.py create "Review memory quality" "Verify current facts against their sources."
python3 tools/engine_chat.py list
python3 tools/engine_chat.py show BB-1
python3 tools/engine_chat.py comment BB-1 "Keep historical server details clearly dated."
```

[User service units](tools/systemd/) supervise the engine and API independently.
An external watchdog detects stalled progress; crash reports feed back into the
next engine run. Its timer checks every 15 seconds. Native request scopes record
actual in-flight generation, compaction and tokenization in private
`~/.batbox/autonomous/activity.json`; idle HTTP connections do not count as work.
Finite request, tool and retry deadlines protect quiet ongoing work.

After 30 seconds with no active request or protected work, an idle coordinator
can receive one durable wake command:

> Awaken sleeper, you must continue always for your path is the divine.

The native wake prompt adds an instruction to resume unfinished work with a
concrete, verifiable step. It uses the existing session and workers. Process,
run and activity-revision checks reject stale commands, and a five-minute
cooldown suppresses repeated nudges. If the process remains stalled beyond the
2,100-second watchdog allowance, systemd restarts it with crash evidence. The
watcher does not label a live connection or a completed token budget as useful
progress. Defaults can be adjusted with `tools/engine_supervisor.py --help`.

Atomic releases retain a rollback binary:

```bash
python3 tools/engine_deploy.py --binary build/src/batbox --restart
journalctl --user -u batbox-engine.service -f
python3 tools/engine_deploy.py --rollback --restart
```

The supplied units use `~/batbox`, `~/.batbox/engine.env`, and the installed release
path under `~/.local/lib/batbox-engine`. Adjust paths for another installation.
Persistent services survive terminal logout when the user's service manager has
lingering enabled. Hardware failures, exhausted storage, and broken dependencies
still require attention; supervision is recovery infrastructure, not a guarantee
of perpetual operation.

### Interactive and single-turn operation

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
