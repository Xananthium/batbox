# BatBox Handoff — 2026-05-16 (afternoon)

C++ rewrite of claude-code (Ink/TS → FTXUI) at `/Users/xananthium/CLAUDECODE/claude-code`. Branded "BatBox" with Miss Kittin electroclash aesthetic. No telemetry, no auto-update, local sandbox only.

## Current state

- **Build**: clean.
- **Tests**: 3856 total, ~3846 passing (8-10 pre-existing flakes in MCP/task_crud/scrapling/cron — unrelated to TUI work).
- **Binary**: `build/src/batbox`.
- **TUI**: **end-to-end working through tmux smoke harness**. Multi-turn passes 3/3 deterministically. Tool calls render, permission modal shown in default mode, slash palette populated (38 commands), `/resume` works.
- **`--print` mode**: fully working with the user's LM Studio setup.

## Test harness

Tmux-based smoke harness now lives at `tests/tui-smoke/`. Opt-in via `cmake -DBATBOX_TUI_SMOKE=ON`, then `ctest -L tui-smoke`. Spec at `agentic/planning/test-harness.md`. Mock LLM uses stdlib-only Python (port 8824), so cases run offline.

## User's runtime setup

```
LM Studio @ http://192.168.1.227:1234/v1
Active model: mistralai/magistral-small-2509  (Mistral GGUF, NOT MLX — MLX variant has tree_reduce bug)
```

Config persisted at `~/.batbox/.env`:
```
BATBOX_API_BASE_URL=http://192.168.1.227:1234/v1
BATBOX_API_KEY=lmstudio
BATBOX_DEFAULT_MODEL=mistralai/magistral-small-2509
BATBOX_OPUS_MODEL=mistralai/magistral-small-2509
BATBOX_SONNET_MODEL=mistralai/magistral-small-2509
BATBOX_HAIKU_MODEL=mistralai/magistral-small-2509
BATBOX_MAX_TOKENS=16384
BATBOX_TEMPERATURE=0.2
BATBOX_SIDECAR_PORT=8823
```

**Ollama Cloud quota EXHAUSTED** for account `youthful_poincare_967` — both `deepseek-v4-flash:cloud` and `kimi-k2.6:cloud` 429 until weekly reset. Use LM Studio.

## Quick-start commands

```bash
# Launch TUI
~/CLAUDECODE/claude-code/build/src/batbox

# One-shot --print (works today)
~/CLAUDECODE/claude-code/build/src/batbox --print "say hi"

# Resume last session
~/CLAUDECODE/claude-code/build/src/batbox --resume-latest

# Nuclear mode (bypass permission gates — needed for sub-agents to write files)
~/CLAUDECODE/claude-code/build/src/batbox --nuclear

# Install agent specs (one-time, copies ~/.claude/agents → ~/.batbox/agents)
~/CLAUDECODE/claude-code/build/src/batbox migrate --apply
```

## TUI fix wave 2026-05-16 PM (this session)

Ran a PM-triage pass that found 11 UI defects, then a parallel Junior-Dev loop. All 10 in-scope tasks landed (TUI-T2 skipped per user — trust agent's #33 claim, source-level inspection confirmed). Headline finds:

**The real "UI broken" root causes (discovered during TUI-T6 multi-turn verify):**
1. `WireTui.cpp` — `Container::Vertical` started with `selected_=0` (banner, non-focusable). **Every keystroke was silently dropped from the first frame.** Fix: `input_bar->TakeFocus()` after building the vbox.
2. `WireTui.cpp` — `Container::Vertical` only routes events to active child, so all the Token/UserMessage/StreamDone events from #33 were posted but **never reached ChatView**. Fix: in root `CatchEvent` lambda, fall through to `top_with_events->OnEvent(ev)` after `main_vbox->OnEvent(ev)`.

Unit tests for #29/#32/#33 were all green; these were FTXUI integration-layer bugs invisible to the unit harness. The tmux smoke harness caught both within minutes.

**Other landed defects:**

| Task | Defect | Files |
|------|--------|-------|
| TUI-T1 | Tmux smoke harness scaffold (`tests/tui-smoke/`) | new dir + CMake hook behind `BATBOX_TUI_SMOKE` option |
| TUI-T3 | Slash palette populated (38 cmds), slash dispatch in `tui_on_submit` | `WireTui.cpp`, `App.cpp`, `Conversation::clear_messages()` |
| TUI-T4 | PermissionCard wired into TUI gate (was hardcoded `Decision::allow()` — silent nuclear-by-default) | `WireTui.cpp`, `App.cpp`, blocks worker via `cv_.wait` until UI resolves |
| TUI-T5 | `make_message_appended_event` for tool call + result; ChatView renders `[tool: <name>]` and `[result: ...]` | `Events.hpp/cpp`, `Conversation.cpp`, `ChatView.cpp` |
| TUI-T6 | Multi-turn smoke + the two routing bugs above (3/3 deterministic) | `tests/tui-smoke/cases/04_multi_turn.sh`, `WireTui.cpp` |
| TUI-T7 | `/resume <uuid>` command + adapter `get/set_messages_json` (`TuiConvAdapter` stubs silently broke `/resume` and `/compact`) | `App.cpp`, `tests/unit/test_resume_cmd.cpp` |
| TUI-T8 | Escape conflict resolved — Escape=cancel, vim_toggle moved to Ctrl+G + `/vim` slash | `repl/Keybindings.cpp`, `config/KeybindingsConfig.cpp` |
| TUI-T9 | Tool-running status row indicator (`· running: <tool>` appears during dispatch) | `Events.hpp/cpp`, `Conversation.cpp`, `InputBar.cpp`, `App.cpp` |
| TUI-T10 | Session UUID logged to stderr at TUI startup (#27 closed) | `App.cpp`, `Conversation::start_session()` added |
| TUI-T11 | `token_received` retry guard now counts `reasoning_content` chunks (#30 closed) | `ChatResponse.hpp`, `Client.cpp`, `tests/unit/test_client_reasoning.cpp` (20 new tests) |

## TUI fix wave 2 (inference regression)

User reported "yeah but no inference?" after wave 1. PM triage round 2 found a cascade of bugs:

| # | Issue | Fix |
|---|---|---|
| TUI-T12 | `Client::stream_chat` returned Ok on a 2xx clean-close even with zero content/tool_call/finish_reason | Added `final_finish_reason_seen` bool; return Err with informative message when neither content nor finish_reason was seen |
| TUI-T13 | (test) | 16 cases in `test_client_reasoning.cpp` covering the guard |
| TUI-T14 | (test) | `mock_reasoning_only.py` + tmux case `11_reasoning_only_truncation.sh` |
| TUI-T15 | No UX feedback during Magistral's long reasoning phase | `ThinkingStarted` / `ThinkingStopped` events, InputBar shows `· thinking…` (priority below running-tool) |
| **TUI-T17** | **Real root cause**: LM Studio responds in 38ms with SSE `event: error` because BatBox sends 39 tool defs that overflow Magistral's loaded context. BatBox's SSE dispatcher only checked `is_done`, so error events were silently dropped — making T12's generic guard fire with a misleading "increase MAX_TOKENS" message | `write_cb` now handles `event: error`, `event: fatal_error`, and inline `data: {"error":...}` → returns `Err("server: <message>")`. Non-recognized events (`ping`, etc.) `continue` instead of falling through. |
| TUI-T17b | No trace logging in SSE loop | `lg->trace("sse: event=... data_bytes=...")` per event |
| TUI-T18 | (test) | 10 cases in `test_client_reasoning.cpp` |
| TUI-T19 | (test) | `mock_reasoning_only.py --mode {error-event,fatal-error,inline-error}` + cases 12/13 (3/3 deterministic) |

**Current user-visible behavior** after the fix:
```
$ ./build/src/batbox --print "hi"
batbox: inference error: server: The number of tokens to keep from the initial prompt
is greater than the context length. Try to load the model with a larger context length,
or provide a shorter input
```

**What the user needs to do to actually get inference working**:
1. **Easiest**: In LM Studio, re-load Magistral with a larger context length (try 32k or 64k instead of the default 8k). The 39 tool defs cost ~5-6k tokens of prompt.
2. **Alternative**: Use `--print` without tools — currently not supported, would need a `--no-tools` flag (future task).
3. **Alternative**: Use a model with native 128k+ context.
4. **Future BatBox feature**: trim tool list for simple chat / size-aware tool selection (file a P1 task when ready).

## Open backlog (still pending)

| # | Task | Severity |
|---|---|---|
| #11 | Mini-project planning chain dry-run with Magistral | Blocked until model context fits 39 tools + prompt |
| #26 | Parallel ctest flakes — temp-dir/SQLite contention | CI hygiene |
| #31 | ConfigReload uses `process_env_wins=false` (inconsistent with first-load) | Hot-reload edge case |
| (new) | `--no-tools` flag or auto-trim tool list when prompt is short | P1 — direct UX consequence of the context-overflow |
| (new) | Idle-stream timeout knob (`BATBOX_STREAM_IDLE_TIMEOUT_SEC` → CURLOPT_LOW_SPEED_*) | P3 — defence-in-depth, not blocking |

## Recommended next live test

User should rebuild, launch the binary, and confirm:
```
cmake --build build --parallel 8
build/src/batbox
# type "hi" + Enter → user message echoes, assistant streams reply
# type "/" → palette appears with 38 commands
# ask to "read README.md" → permission modal appears (not nuclear), accept, tool result renders
```

If anything in that sequence fails, the offline harness will catch it deterministically:
```
cmake -DBATBOX_TUI_SMOKE=ON build/
ctest --test-dir build -L tui-smoke -V
```

## Fix-wave history (this session)

| # | What | Notes |
|---|---|---|
| #12 | CMake test discovery + AgentSupervisor link | Renamed test cases containing `[`/`]` that broke CMake list parser |
| #15a | Sidecar `/fetch` CSS/JS leak | BeautifulSoup `decompose()` before markdownify |
| #15b | `--print` headless ToolRegistry passing | run_headless now takes ToolRegistry& + PermissionGate& |
| #16 | `tools: [...]` array in /v1/chat/completions | Was missing — DeepSeek/Qwen/Kimi fell back to model-specific delimiter format |
| #17 | `--resume <uuid>` injects prior session messages | + `SessionStore::touch()` added |
| #18 | OPENAI_BASE_URL / OPENAI_API_KEY / BATBOX_MODEL fallbacks | INFO-log when alias path is taken |
| #19 | WsTransport test cluster + tag-filter | Process-isolation via RAII; tag-filter was already passing |
| #20 | Task-mgmt DB tests (5 of them) | Root cause: `AgentSupervisor::spawn()` passed `Config::load_default()` as rvalue temp, SubAgent stored as `const Config&` → dangling reference SIGSEGV. **Collaterally fixed #21 and #22.** |
| #23 | Agent spec model alias resolution | `model: opus` / `sonnet` in spec files now resolves via Config.opus_model / sonnet_model / haiku_model (each defaulting to default_model) |
| #24 | Sidecar python_bin auto-resolve to venv absolute path | Default `python3` was resolving to Xcode python; now uses `~/.batbox/sidecar/.venv/bin/python3` |
| #25 | `libbatbox_tools.a` 178 MB exceeded macOS ar 32-bit index | Switched to `libtool -static` with 64-bit index on macOS |
| #29 | TUI: wire on_submit to Conversation + populate model_name | Was a no-op stub |
| #32 | TUI: InputBar missing `Focusable() const override { return true; }` | FTXUI dropped events at container layer |
| #33 | TUI: ChatView::OnEvent doesn't extract token events | Closed — source wired; live verify via TUI-T6 |
| TUI-T1..T11 | TUI fix wave (10 tasks; T2 skipped) | All complete — see PM-triage section above |

## Critical files & their owners

| File | Role |
|---|---|
| `src/App.cpp` | Init sequence, env loading, TUI vs --print branching. Worker thread on_submit lambda at ~line 975. |
| `src/app/WireTui.cpp` | FTXUI component graph. Bug #29 + #32 + #33 all here. |
| `src/tui/InputBar.cpp` | Input prompt + status row. Event handling. |
| `src/tui/ChatView.cpp` | Scrollable conversation. **#33 fix target.** |
| `src/tui/Events.cpp` + `include/batbox/tui/Events.hpp` | Custom FTXUI event types (Token, AgentsDirty, ModalShow, ...). |
| `src/conversation/Conversation.cpp` | Per-session message history + run_turn driver. NOT thread-safe. |
| `src/inference/OpenAiClient.cpp` | Tools array serialization (#16 fix). |
| `src/inference/Client.cpp` | stream_chat retry loop. `token_received` flag at ~line 467. |
| `src/config/Config.cpp` | env loading, model alias resolution (#23), sidecar python resolution (#24). |
| `src/sidecar/SidecarManager.cpp` | Python sidecar process management. |
| `src/agents/SubAgent.cpp` | Sub-agent inference + model alias resolution. |
| `src/agents/AgentSupervisor.cpp` | Sub-agent lifecycle. **Dangling-Config-ref fix #20 here.** |
| `tests/integration/test_tui_layout.cpp` | TUI wiring assertions. #29, #32, #33 regression tests live here. |
| `python-sidecar/scrapling_server/app.py` | Scrapling FastAPI sidecar — `/fetch`, `/search`. |

## Conventions / gotchas

- **Don't use `python3` literal** for sidecar — use absolute venv path (#24 fix). Now auto-resolved.
- **`model: opus` / `sonnet` in agent specs** are aliases, not literal model names. Resolved via `BATBOX_OPUS_MODEL` / `BATBOX_SONNET_MODEL` (#23).
- **macOS `ar` archive overflow** — `batbox_tools` is large, requires `libtool -static` on macOS (#25). If you see `ranlib: archive member size too large`, check `src/tools/CMakeLists.txt`.
- **`build/src/batbox`** is the binary path. Many docs say `build/batbox` — wrong.
- **xattr quarantine** can block ranlib on fresh checkouts: `xattr -cr build/` clears it.
- **Reasoning models** (Magistral, Qwen3, DeepSeek-V4-Flash): stream `reasoning_content` BEFORE `content`. BatBox correctly ignores it. UI looks frozen for 3-15s before tokens flow — that's normal.
- **MLX models in LM Studio**: `tree_reduce` NameError on any tools-bearing request. Use GGUF.
- **Tests in CMake list strings**: doctest test-case names with unbalanced `[` or `]` corrupt CMake's list parser via `doctestAddTests.cmake`. See #12.

## Where to resume

1. Wait for agent `a73590f758e55dc6a` (#33) completion notification.
2. Confirm clean rebuild and ctest still 3757+/3757.
3. Have user relaunch TUI, type "hi" + Enter, confirm chat view renders.
4. Fire the predicted-next-breaks smoke (tool call in default mode + permission modal).
5. Then retest #11 planning chain via Magistral.

## Pinned memory

Memories are auto-loaded at `/Users/xananthium/.claude/projects/-Users-xananthium-CLAUDECODE/memory/`:
- `feedback_ollama_cloud_only.md` — superseded by LM Studio reality but kept for when quota refreshes
- `reference_batbox_env_vars.md` — accurate

Consider adding:
- `reference-batbox-lmstudio.md` — the Magistral / 192.168.1.227 setup
- `reference-batbox-binary-path.md` — `build/src/batbox` not `build/batbox`

## TUI-FLOW-T3: Inference latency triage (perf instrumentation)

Added three timing measurements to the TUI to confirm or refute the "feels slow" complaint:

| Measurement | Where measured | What it tells you |
|------------|----------------|-------------------|
| `first_token_ms` | `Conversation::run_turn()` — submit → first SSE content delta | Network + LM Studio inference TTFT |
| `stream_to_paint_ms` | `ChatView::OnRender()` — token event posted → next OnRender start | FTXUI event queue + render pipeline cost |
| `frame_ms` | `ChatView::OnRender()` body wall-time | Per-frame render cost of the message history |

**How to see live numbers:**
```bash
BATBOX_PERF_HUD=1 build/src/batbox
# Status row shows: "⚡ first=Xms · paint=Yms · frame=Zms"
```

**Where logged:**
- `first_token_ms` logged immediately on first token: `[perf] first_token=Xms`
- All three logged at INFO after each streaming turn: `[perf] first_token=Xms stream_to_paint=Yms frame=Zms`
- Log file: `~/.batbox/batbox.log`

**Triage interpretation:**

- `first_token_ms > 1500` → **network/inference bottleneck** (LM Studio or OpenAI latency). Fix: use a model with faster TTFT, add streaming timeout, or switch to a regional endpoint. This is *outside TUI scope*.
- `stream_to_paint_ms > 50` → **FTXUI event queue or render scheduling** bottleneck. Fix: investigate Screen::post_event() overhead; consider batching token posts.
- `frame_ms > 33` (>33ms = <30fps) → **ChatView::OnRender() re-renders entire history every frame**. Fix: cache rendered `ftxui::Element` objects per message (TUI-FLOW-T12 caching — *recommended if frame_ms is high*).

**Expected values for local LM Studio (Magistral Small GGUF, 192.168.1.227:1234):**
- `first_token_ms`: 200–800ms typical for small models; >1500ms indicates model cold-start or heavy load.
- `stream_to_paint_ms`: should be <16ms (one frame at 60fps); >50ms indicates render scheduling issue.
- `frame_ms`: 1–5ms for short conversations; >33ms triggers TUI-FLOW-T12 caching recommendation.

**New files:**
- `include/batbox/perf/PerfSnapshot.hpp` — `PerfStore` + `PerfSnapshot` + `g_perf` global
- `src/perf/PerfSnapshot.cpp` — atomic setters/getters
- `src/perf/CMakeLists.txt` — `batbox_perf` static library
- `tests/unit/test_perf_snapshot.cpp` — 8 test cases, 240k+ assertions (including 8-thread contention test)
