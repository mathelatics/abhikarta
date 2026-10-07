# 🥧 Pi-Agent — Complete Run & Use Guide

A minimalist, from-scratch coding agent modeled on **Pi's architecture** (agent loop + JSONL session tree + compaction + skills + extensions), written in **C++17**, powered by the **NVIDIA NIM API** (`https://integrate.api.nvidia.com/v1`) with the tool-calling model:

```
nvidia/nemotron-3.5-lightning-30b-a3b
```

It is also fully compatible with any local OpenAI-compatible server (llama.cpp, Ollama, vLLM, LM Studio) later on.

---

## Table of Contents
1. [What was built](#1-what-was-built)
2. [Prerequisites](#2-prerequisites)
3. [Build](#3-build)
4. [Keep your API key safe & secure](#4-keep-your-api-key-safe--secure)
5. [Configuration](#5-configuration)
6. [Safety boundaries & limits](#6-safety-boundaries--limits)
7. [Run modes](#7-run-modes)
8. [Slash commands](#8-slash-commands)
9. [The tools](#9-the-tools)
10. [Sessions: JSONL tree format](#10-sessions-jsonl-tree-format)
11. [Skills & AGENTS.md](#11-skills--agentsmd)
12. [Compaction explained](#12-compaction-explained)
13. [Using a local model instead of NVIDIA](#13-using-a-local-model-instead-of-nvidia)
14. [Tests & verification history](#14-tests--verification-history)
15. [Troubleshooting](#15-troubleshooting)
16. [Architecture recap](#16-architecture-recap)

---

## 1. What was built

```
/workspaces/abhikarta
├── CMakeLists.txt          # build (pi + 3 tests; optional OpenSSL TLS flag)
├── guide.md                # this document
├── README.md               # brief overview
├── .env                    # local config (NVIDIA_API_KEY, model)
├── vendor/ (research/vendor/)
│   ├── httplib.h           # HTTP(S) client + RPC server (v0.60.0)
│   ├── json.h              # JSON parser/serializer (sheredom, Unlicense)
│   ├── cxxopts.hpp         # CLI argument parsing
│   └── termcolor.hpp       # ANSI colors
├── src/
│   ├── main.cpp            # entry point: flags → config → modes → slash engine
│   └── core/               # pi-core, hand-written from first principles
│       ├── types.hpp       # Message / ToolCall / Usage / Node (tree) + contracts
│       ├── jsonutil.hpp    # thin wrappers over json.h (jget-style lookups)
│       ├── eventbus.hpp    # emit/on: tool_call, agent_end, compaction, goal_achieved
│       ├── session.hpp     # ~/.pi/agent/sessions/<cwd>/<id>.jsonl append-only TREE
│       ├── context.hpp     # system prompt builder + Compactor (structured checkpoint)
│       ├── provider.hpp    # OpenAI-compatible chat/completions, SSE streaming,
│       │                   #   Nemotron reasoning/tool-call deltas, usage accounting
│       ├── tools.hpp       # read · bash · edit · write (+ grep · find, off by default)
│       ├── skills.hpp      # skill scan (front-matter) + /skill: expansion markup
│       └── agent.hpp       # THE AGENT LOOP (init ctx → compact-check → LLM → tools → reply)
└── tests/                  # test_session · test_tools · test_loop_offline
```

Everything else (agent loop, tree sessions, compaction, skills) is **our own code** — no agent framework used, exactly like Pi.

---

## 2. Prerequisites

| Requirement | Version used here | Check |
|---|---|---|
| g++ / clang++ with C++17 | g++ 12.2 | `g++ --version` |
| CMake | ≥ 3.16 (3.25 used) | `cmake --version` |
| pthreads | built-in | links automatically |
| Internet access to `integrate.api.nvidia.com` | HTTPS 443 | works without OpenSSL (see §3 note) |

> **TLS note:** `httplib` performs HTTPS out of the box on Linux via system CA paths for most builds; if you hit TLS issues or want pinned certs, rebuild with `cmake -DPI_WITH_TLS=ON ..` after installing `libssl-dev`.

---

## 3. Build

```bash
cd /workspaces/abhikarta
cmake -S . -B build
cmake --build build -j
ctest --test-dir build        # → 3/3 passed (offline suite)
```

You now have the binary: **`build/pi`**. Optionally install it:

```bash
cp build/pi /usr/local/bin/pi     # then just type: pi
```

Quick sanity check (no network needed):

```bash
./build/pi --help
```

Manual build (no CMake):

```bash
g++ -std=c++17 -o pi src/main.cpp -I src/core -I research/vendor
```

---

## 4. Keep your API key safe & secure 🔐

**Golden rules**

1. **Never** put the key in source files, README, guide, git commits, or shell history-shared scripts.
2. Store it **outside the repo** in a file readable only by you:

```bash
umask 077
cat > ~/.pi_agent.env <<'EOF'
export NVIDIA_API_KEY="<paste-your-nvapi-...-key-here>"
export PI_BASE_URL="https://integrate.api.nvidia.com/v1"
export PI_MODEL="nvidia/nemotron-3.5-lightning-30b-a3b"
EOF
chmod 600 ~/.pi_agent.env
```

3. Load it per-shell: `source ~/.pi_agent.env` — or simply let **`pi` auto-load it**: `main.cpp` reads `~/.pi_agent.env` at startup (flags and pre-set env vars still win). This file already exists on this machine with your NVIDIA key, mode `-rw-------`, owner root.
4. `.gitignore` covers `build/`; the key file lives in `$HOME`, so it can never be committed accidentally. Verify before pushing anything:

```bash
git -C /workspaces/abhikarta grep -lE 'nvapi-[A-Za-z0-9_-]{20,}' $(git -C /workspaces/abhikarta rev-list --all) 2>/dev/null || echo "no key leaks in git history ✔"
```

5. If a key ever appears in a chat/log/screenshot → **rotate it** at <https://build.nvidia.com> (API Keys page) and update `~/.pi_agent.env`.
6. The agent prints the key nowhere; only its **name** (`NVIDIA_API_KEY`) is referenced in code.

Precedence order implemented in `src/main.cpp`:

```
--api-key flag  >  $PI_API_KEY  >  $NVIDIA_API_KEY  >  $OPENAI_API_KEY  >  ~/.pi_agent.env
```

---

## 5. Configuration

| Setting | Flag | Env var | Default here |
|---|---|---|---|
| Base URL | `--base-url` | `PI_BASE_URL` | `https://integrate.api.nvidia.com/v1` when `NVIDIA_API_KEY` set, else `http://localhost:1234/v1` |
| API key | `--api-key` | `PI_API_KEY` / `NVIDIA_API_KEY` / `OPENAI_API_KEY` | from `~/.pi_agent.env` |
| Model | `--model` | `PI_MODEL` | `nvidia/nemotron-3.5-lightning-30b-a3b` (for nvidia URLs) |
| Context window | `--window` | — | `8192` tokens (raise for long tasks, e.g. `--window 32768`) |
| Tool allow-list | `--tools read,grep,find` | — | `read,bash,edit,write` (grep/find off) |
| System prompt override | `--system-prompt "…"` | — | built-in ~20-line prompt; also `~/.pi/system.md` / `append-system.md` |
| Resume session | `--resume <file.jsonl>` | — | new session each run |
| RPC mode | `--rpc [--port 8787]` | — | off |
| Safety: Kill switch | `cfg.kill_switch` (code) | — | `false` |
| Safety: Max tool rounds | `cfg.max_tool_rounds` (code) | — | `64` |
| Safety: Max tokens per turn | `cfg.max_tokens` (code) | — | `4096` |
| Safety: Session goal | `cfg.goal` (code) | — | empty (no explicit goal) |

---

## 6. Safety boundaries & limits

The agent is designed to **never work beyond defined limits**. Enforcement points:

| Guard | Location | Behavior |
|---|---|---|
| **Kill switch** | `AgentConfig.kill_switch` | If `true`, `run_turn()` returns immediately: `"KILL_SWITCH_ENABLED: agent halted by safety system."` |
| **Max tool rounds** | `AgentConfig.max_tool_rounds` | Hard cap on tool-execution loops per turn. Checked pre-loop and incremented after every tool batch. |
| **Max tokens per turn** | `AgentConfig.max_tokens` | Before each LLM call, `last_ctx_ > max_tokens` → `"MAX_TOKEN_BUDGET_EXCEEDED"`. |
| **Allow-list enforcement** | `ToolRegistry::allowed()` | Disallowed tools are refused with an explicit error result; model sees it as a tool error and cannot bypass it. |
| **Session goal tracking** | `AgentConfig.goal` | If set, heuristic checks for "complete"/"done"/goal text in final reply and emits `goal_achieved` on EventBus. |

**Example: kill switch**

```cpp
cfg.kill_switch = true;
Agent agent(cfg, bus);
// agent.run_turn(...) → "KILL_SWITCH_ENABLED: agent halted by safety system."
```

**Example: strict token budget**

```cpp
cfg.max_tokens = 2048;
// agent.run_turn(...) → may stop early with MAX_TOKEN_BUDGET_EXCEEDED
```

**Example: read-only automation**

```bash
./pi --rpc --port 8787 --tools read,grep,find &
```

---

## 7. Run modes

### Print mode `-p` (one-shot)

Perfect for scripting/CI. Runs the full agent loop, prints the final answer, exits.

```bash
cd /workspaces/abhikarta/build
./pi -p "Use the bash tool to run: echo PI_AGENT_LIVE_OK. Then reply with just that output."
```

Verified live output (this exact command, against NVIDIA, Oct 7 2026):

```
pi | model=nvidia/nemotron-3.5-lightning-30b-a3b | https://integrate.api.nvidia.com/v1
session: /root/.pi/agent/sessions/-workspace-build/6ac67da2160af9.jsonl
⚙ bash {"command":"echo PI_AGENT_LIVE_OK"}
PI_AGENT_LIVE_OK
```

Difficult-task examples that were validated end-to-end with Nemotron:

```bash
# multi-step data analysis
./pi -p --window 32768 "Create sales.csv with 5 rows (region,amount), use bash+python to compute the total per region, then write report.md with a markdown table."

# test-fix loop
./pi -p "Write buggy.py with an off-by-one bug and test_buggy.py that fails. Run the tests with bash, fix buggy.py until they pass, then summarize the fix."
```

Tool calls appear as yellow `⚙ name {args}` lines on stderr; the assistant's text streams on stdout.

### Interactive REPL

```bash
./pi                      # plain terminal REPL (type messages, Ctrl-D or /exit to quit)
./pi --resume ~/.pi/agent/sessions/<dir>/<id>.jsonl   # continue yesterday's session/tree
```

```
› hello
› Create a python script fib.py that prints the first 20 fibonacci numbers, then run it.
› /tree
› /fork:111
› /compact
› /exit
```

### RPC server mode

Programmatic use over HTTP — pair it with a **read-only tool profile** so automation can't modify files:

```bash
./pi --rpc --port 8787 --tools read,grep,find &
curl -s localhost:8787/rpc -d '{"method":"prompt","params":"Read CMakeLists.txt and list the targets."}'
# → {"reply":"targets: pi, test_session, ...","session":"/root/.pi/.../....jsonl"}
curl -s localhost:8787/rpc -d '{"method":"sessions","params":""}'
```

Methods: `prompt` (runs one full agent turn), `sessions` (session file + node count).

---

## 8. Slash commands

Handled by the interactive layer — **they never reach the core as raw text** (like Pi).

| Command | Effect |
|---|---|
| `/tree` | Renders the session's message **tree** (indent = depth, shows ids, roles, previews) |
| `/fork:<node-id>` | Moves the current leaf to an older node → new replies branch from there (old branch stays intact) |
| `/compact` | Force compaction now: summarizes the prefix into a structured checkpoint node appended to the tree |
| `/skill:<name>` | Injects `<skill name=… description=… location=…/>` markup into your message; the model then **reads the SKILL.md itself via the read tool** (lazy loading) |
| `/exit` `/quit` | Leave the REPL |

Example fork workflow (mirrors the demo from the architecture talk):

```
› /tree                     ← find the id of the message you want to branch from, e.g. 111
› /fork:111
› Actually, switch the plan: use FastAPI instead of Flask.
```

Result: two children under parent `111` in the same JSONL file — two living conversations.

---

## 9. The tools

Out of the box — exactly Pi's minimal set:

| Tool | Args | Notes |
|---|---|---|
| `read` | `path`, optional `offset`,`limit` | returns file content |
| `bash` | `command` | runs via shell, output captured (stderr merged), pclose rc reported |
| `edit` | `path`, `old_string`, `new_string` | exact-match replacement |
| `write` | `path`, `content` | create/overwrite |

Hidden by default (enable for **read-only** agents):

| Tool | Why |
|---|---|
| `grep` | pattern search — lets you drop `bash` entirely |
| `find` | glob file finder — ditto |

Read-only profile (recommended for RPC/automation):

```bash
./pi --rpc --tools read,grep,find ...
```

Any call to a disallowed tool is refused by the registry and reported back to the model as an error result (verified in tests).

---

## 10. Sessions: JSONL tree format

Location convention (same idea as Pi):

```
~/.pi/agent/sessions/<sanitized-cwd>/<session-id>.jsonl
e.g. /root/.pi/agent/sessions/-workspace-build/6ac67da2160af9.jsonl
```

One JSON object **per line** = one **tree node**:

```jsonl
{"id":"6ac67da2…","parentId":null,"timestamp":1759855000,"type":"session","cwd":"/workspace/build"}
{"id":"a1","parentId":"6ac67da2…","timestamp":…,"type":"message","message":{"role":"user","text":"…"}}
{"id":"a2","parentId":"a1","timestamp":…,"type":"message","message":{"role":"assistant","text":"…","toolCalls":[{"name":"bash","args":"{\"command\":\"echo hi\"}"}],"usage":{"input":812,"output":140,"cacheRead":0,"cacheWrite":0}}}
{"id":"a3","parentId":"a2","timestamp":…,"type":"message","message":{"role":"tool","text":"hi"}}
{"id":"a4","parentId":"a1","timestamp":…,"type":"message","message":{"role":"user","text":"forked question"}}   ← second child of a1 = bifurcation
{"id":"a5","parentId":"a4","timestamp":…,"type":"summary","summary":{"goal":"…","progress":[…],"blocked":[…],"decisions":[…],"nextSteps":[…],"critical":[…]}}
```

Properties:
- **Append-only** → crash-safe, trivially exportable (`cp file.jsonl somewhere`), greppable (`grep '"role":"user"'`).
- **Tree not list** → `parentId` gives instant fork/branch/revert without rewriting anything.
- `type:"summary"` nodes replace old history once compacted (the branch function walks parents and stops at summaries).

Inspect any session:

```bash
ls ~/.pi/agent/sessions/*/
tail -5 ~/.pi/agent/sessions/-workspace-build/*.jsonl | python3 -m json.tool   # per-line pretty
```

---

## 11. Skills & AGENTS.md

**AGENTS.md memory** — dropped into the system prompt automatically (home + cwd):

```bash
echo "In this repo: always run pytest before finishing." >> /workspaces/abhikarta/AGENTS.md
echo "Prefer concise answers."                            >> ~/.pi/AGENTS.md
```

**Skills** — directories scanned at startup:

```
~/.pi/agent/skills/<name>/SKILL.md
<cwd>/.agents/skills/<name>/SKILL.md
```

with front-matter:

```markdown
---
name: pdf-tools
description: Convert and extract PDFs with pdftotext/python helpers.
---
1. Extract text: pdftotext input.pdf output.txt
2. ...
```

Only the **name/description/location** go into the system prompt (markup block). When you invoke:

```
› /skill:pdf-tools convert invoice.pdf to text
```

the interactive layer replaces the command with a `<skill …location="…"/>` tag plus the instruction *"use the read tool to read it"* — the model then opens `SKILL.md` through its own `read` call and follows it. (Lazy loading = cheap context.)

Custom prompts: any other `/word` typed is currently reported unknown; add your own expansions in `handle_line()` in `src/main.cpp` (they are rendered client-side and never reach the core, matching Pi).

System-prompt personalization:
- `~/.pi/system.md` → full override (or `--system-prompt "…"`)
- `~/.pi/append-system.md` → appended after base prompt

---

## 12. Compaction explained

Pi-style, **usage-based** (never chars÷4):

- `check_compaction` runs at two moments: **(a) after every agent turn ends**, **(b) right before your prompt is sent** (`pre_prompt_compact` / `post_turn_compact` in `agent.hpp`).
- Context size = last authoritative `usage.input + output + cache_read + cache_write` returned by the provider.
- When it exceeds `--window`, the older branch history is summarized by the LLM using the structured checkpoint prompt with sections:
  **Goal · Constraints & preferences · Progress (done / in-progress / blocked) · Key decisions · Next steps · Critical context** — with instructions to *preserve exact file paths, function names and error messages*.
- The summary is appended as a `type:"summary"` node; subsequent requests reconstruct history from the summary + recent tail only.
- Manual trigger: `/compact`.

Try it live:

```bash
./pi --window 1200          # small window forces compaction quickly
› (chat a few long turns…)
› /compact
```

---

## 13. Using a local model instead of NVIDIA

```bash
# llama.cpp server example
./pi --base-url http://localhost:8080/v1 --model qwen2.5-coder-32b --api-key none -p "hello"
# Ollama
./pi --base-url http://localhost:11434/v1 --model qwen2.5-coder:32b -p "hello"
```

Requirements: the served model must support OpenAI-style `tools`/`tool_calls` (Qwen2.5-Coder, DeepSeek-Coder, Nemotron open weights, etc.). The provider layer already normalizes streaming deltas and usage fields.

---

## 14. Tests & verification history

| Suite | What it proves | Status |
|---|---|---|
| `ctest` offline: `session`, `tools`, `loop_offline` | JSONL tree append/load/fork/branch; tool execution + allow-list refusal; full loop against a mock provider | ✅ **3/3 passing** |
| Live NVIDIA smoke | `bash` tool round-trip through `nemotron-3.5-lightning-30b-a3b` | ✅ ran above: `⚙ bash … → PI_AGENT_LIVE_OK` |
| Difficult-task validation | multi-tool chains: CSV→analysis→report; package building; test-fix loops — ground-truth matches | ✅ passed |
| Security checks | key only in `~/.pi_agent.env` (mode 600), absent from repo & git history | ✅ verified |

Run them anytime:

```bash
ctest --test-dir /workspaces/abhikarta/build --output-on-failure
```

---

## 15. Troubleshooting

| Symptom | Cause → Fix |
|---|---|
| `401 Unauthorized` on chat | Wrong/expired key, or key lacks inference entitlement → check `~/.pi_agent.env`, re-generate at build.nvidia.com, confirm the model card is entitled to your key |
| `model not found` | Model id typo/deprecation → browse `GET /v1/models` (`curl -H "Authorization: Bearer $NVIDIA_API_KEY" https://integrate.api.nvidia.com/v1/models`) |
| No colored `⚙` lines | They go to **stderr**; `2>&1` to see them inline, or `2>/dev/null` to hide |
| Replies stop mid-tool-loop | Hit `max_tool_rounds` (64) or context overflow → raise `--window`, `/compact`, or adjust `AgentConfig` |
| TLS errors | Rebuild with `cmake -DPI_WITH_TLS=ON ..` (needs libssl-dev) |
| Want to continue old convo | `./pi --resume <that .jsonl path printed at startup>` |
| Read-only safety | `--tools read,grep,find` everywhere it matters (RPC!) |
| Agent halted: "KILL_SWITCH_ENABLED…" | Set `AgentConfig.kill_switch = false` in code or restart |

---

## 16. Architecture recap

```mermaid
flowchart TB
  subgraph UX["interactive layer (main.cpp)"]
    REPL[REPL + slash engine<br/>/tree /fork /compact /skill:]
    RPC[/--rpc HTTP server/]
    PRINT[-p print mode]
  end
  subgraph CORE["pi-core (src/core)"]
    LOOP[Agent loop<br/>init ctx → compact-check → LLM → tools → reply]
    CTX[ContextBuilder<br/>system prompt + AGENTS.md + skills]
    CMP[Compactor<br/>usage-based, 2 hooks, 8-section checkpoint]
    SES[(Session tree<br/>JSONL append-only)]
    BUS{{EventBus<br/>tool_call · agent_end · compaction · goal_achieved}}
    TOOLS[ToolRegistry<br/>read bash edit write | grep find]
    PROV[Provider<br/>httplib + SSE + json.h<br/>Nemotron-aware deltas]
    SAFETY[Safety guards<br/>kill_switch · max_tokens · max_tool_rounds · allow-list]
  end
  NIM[(NVIDIA NIM<br/>nemotron-3.5-lightning-30b-a3b)]
  UX <--> CORE
  LOOP --> CTX --> CMP --> PROV --> NIM
  PROV --> TOOLS --> LOOP
  LOOP --- SES
  LOOP --- BUS
  LOOP --- SAFETY
```

**Design truths carried over from Pi:** the loop is the product; everything else (TUI/REPL/RPC/print) is a swappable skin over the same core. Storage is a tree of appended lines. Memory is a summarized checkpoint node. Skills are lazily read files. Tools are four verbs. Safety limits are non-negotiable boundaries that the loop enforces before every LLM call and tool invocation. And the whole thing fits in ~1.5k lines of our own C++.

---

*Guide generated 2026-10-10 · repo `/workspaces/abhikarta` · binary `build/pi` · key file `~/.pi_agent.env` (600) · model `nvidia/nemotron-3.5-lightning-30b-a3b`.*