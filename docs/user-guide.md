# Pi-Agent — User Guide

Everything you can do with `pi`, how each feature works, and worked examples you can
copy-paste. Built for the terminal: a pinned input line, streaming replies above it,
slash commands, an agent with real tools, sessions stored as a JSONL tree.

> Binary used in all examples: `pi` (build it to `build/pi` or install to
> `/usr/local/bin/pi`). Transcripts match the real binary's output; paths, ids and
> counts are illustrative.

---

## Table of Contents

1. [Feature at a Glance](#1-feature-at-a-glance)
2. [Quick Start](#2-quick-start)
3. [Configuration & Providers](#3-configuration--providers)
4. [Run Modes](#4-run-modes)
5. [The Terminal UI & Keyboard](#5-the-terminal-ui--keyboard)
6. [Slash Commands — Full Reference](#6-slash-commands--full-reference)
7. [Working While the Agent Runs (Interjections, Abort, Approvals)](#7-working-while-the-agent-runs)
8. [The Tools](#8-the-tools)
9. [Skills, AGENTS.md & System Prompt](#9-skills-agentsmd--system-prompt)
10. [Sessions, the Tree, Forking & Export](#10-sessions-the-tree-forking--export)
11. [Context Window & Compaction](#11-context-window--compaction)
12. [Recipes — Worked Examples](#12-recipes--worked-examples)
13. [Quick Reference Cheat Sheet](#13-quick-reference-cheat-sheet)
14. [Troubleshooting](#14-troubleshooting)

---

## 1. Feature at a Glance

| Area | What you get |
|---|---|
| Modes | Interactive REPL, one-shot `-p`, HTTP `--rpc` server, piped/non-TTY |
| TUI | Input line pinned to the bottom; replies, tool output and status render above it and scroll |
| Editing | Ctrl+A/E/B/F/K/U/W/T/L/D/C, Alt+B/F/D word motions, Tab, Delete, Home/End, arrows |
| History | ↑ / ↓ walks submitted lines (in-memory, 200 entries, duplicates skipped) |
| Multiline | Ctrl+J or Shift+Enter starts a new input line; renders as a `│` continuation block |
| Paste | Bracketed paste enabled automatically — pasted text lands verbatim (Enter inside a paste never submits) |
| Live turns | Type while the agent works → queued as an interjection; `/abort` stops it |
| Providers | `/login` profiles: openai · nvidia · hf · ollama · lmstudio · llamacpp · custom |
| Models | `/model` lists, `/model <n>` picks, switch provider+model live, persisted |
| Tools | `read` `bash` `edit` `write` (on) + `grep` `find` (opt-in), workspace sandbox, optional y/N approvals |
| Skills | Lazy-loaded `SKILL.md` files invoked with `/skill:<name>` |
| Sessions | Append-only JSONL **tree** — `/tree`, `/fork:<id>`, `/new`, `/resume:`, `/export:` |
| Compaction | Auto at 85% of `--window` (usage-based), or `/compact` on demand |
| Safety | Tool allow-list, workspace path sandbox, kill switch, max tool rounds, token budget |

---

## 2. Quick Start

### Build

```bash
cd /workspaces/abhikarta
cmake -S . -B build_tls -DPI_WITH_TLS=ON     # TLS build — required for https endpoints
cmake --build build_tls -j
ctest --test-dir build_tls                   # 3/3 passed
cp build_tls/pi /usr/local/bin/pi            # optional: install
```

> Always build with `-DPI_WITH_TLS=ON` if you talk to `https://…` endpoints
> (NVIDIA, OpenAI, Hugging Face). Plain `cmake -S . -B build` builds without TLS
> and can only reach `http://` (local llama.cpp / Ollama / LM Studio).

### First session

```bash
pi
```

```
pi: starting agent config
  profile=nvidia
  base_url=https://integrate.api.nvidia.com/v1
  model=nvidia/nemotron-3.5-lightning-30b-a3b
  tools=
  window=8192
pi | model=nvidia/nemotron-3.5-lightning-30b-a3b | https://integrate.api.nvidia.com/v1
session: /home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
commands: /help /login /model — while a turn runs, type to queue an interjection, /abort to stop
›
```

Type a prompt and press **Enter**:

```
› read CMakeLists.txt and tell me what the build targets are
```

The reply streams above your input line. Your prompt line stays pinned at the bottom —
you can keep typing while the agent works.

Quit with `/exit`, **Ctrl-D** on an empty line, or **Ctrl-C** twice
(first Ctrl-C clears what you typed, second exits).

---

## 3. Configuration & Providers

### Where settings come from (precedence)

```
CLI flag  >  environment variable  >  active profile (~/.pi/config.json)  >  built-in default
```

| Setting | Flag | Env var | Default |
|---|---|---|---|
| Base URL | `-b, --base-url` | `PI_BASE_URL` | NVIDIA URL if a key env exists, else `http://localhost:1234/v1` |
| API key | `-k, --api-key` | `PI_API_KEY` → `NVIDIA_API_KEY` → `OPENAI_API_KEY` | active profile's key |
| Model | `-m, --model` | `PI_MODEL` | `nvidia/nemotron-3.5-lightning-30b-a3b` (NVIDIA URLs), else `local-model` |
| Context window | `--window <n>` | — | `8192` |
| Tool allow-list | `-t, --tools read,bash` | — | `read,bash,edit,write` |
| System prompt | `--system-prompt "…"` | — | built-in prompt |
| Resume session | `-r, --resume <file.jsonl>` | — | new session |

Legacy `~/.pi_agent.env` (shell `KEY=value` lines) is auto-loaded at startup for any
variable not already set. Flags always win.

Keep keys out of git: store them in `~/.pi_agent.env` (mode 600) or in the profile
file — never in the repo.

### Provider profiles — `/login`

Profiles live in `~/.pi/config.json` (directory `~/.pi` is `0700`, file is `0600`).
Keys are masked in every listing and never echoed while typing.

```
› /login
profiles (* = active), keys masked:
  * nvidia  https://integrate.api.nvidia.com/v1  key=nvapi-********************-MBZI
    ollama  http://localhost:11434/v1            key=(none)
```

Connect a provider (prompts walk you through it):

```
› /login nvidia
API key [****-MBZI, Enter=keep, -=clear]: nvapi-…
probing https://integrate.api.nvidia.com/v1/models ... 90 models
logged in: profile=nvidia … (saved to /home/you/.pi/config.json)
```

Known preset names (anything else = *custom*, asks for a base URL):

| Name | Base URL |
|---|---|
| `openai` | `https://api.openai.com/v1` |
| `nvidia` | `https://integrate.api.nvidia.com/v1` |
| `hf` / `huggingface` | `https://router.huggingface.co/v1` |
| `ollama` | `http://localhost:11434/v1` |
| `lmstudio` | `http://localhost:1234/v1` |
| `llamacpp` / `llama.cpp` | `http://localhost:8080/v1` |
| any other name | asks for the base URL |

Details:

- Local endpoints (`localhost` / `127.0.0.1`) skip the API-key prompt
  (`local endpoint — no API key needed`).
- If the model probe fails you're asked `save profile anyway? [y/N]` — answer `n`
  to leave the config untouched.
- Keys are hidden while typing (no echo, no render).
- Remove a profile with `/logout <name>` (the currently used endpoint stays).

### Switching model / provider — `/model`

```
› /model                     # list models at the current endpoint (numbered, * = current)
› /model 7                   # pick entry 7 from that listing
› /model qwen2.5-coder:32b   # set a bare model id on the current endpoint
› /model ollama              # switch to the "ollama" profile (keeps its model)
› /model nvidia:nvidia/nemotron-3.5-lightning-30b-a3b   # provider:model in one go
› /model:deepseek-v4.1-flash # legacy form, same effect
```

Switching writes a `model_change` node into the session tree and persists the choice
into the active profile, so the next run starts with it. If the live model list fails
but you have a cached list, `pi` uses the cache (`live list failed (…); using cached list`).

---

## 4. Run Modes

### 4.1 Interactive REPL (default)

```bash
pi                     # start in the current directory
pi --window 32768      # bigger context window for long tasks
pi --resume ~/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
```

- Replies stream above the pinned input line.
- Plain text = send a prompt; `/…` = slash command (never sent to the model raw).
- While a turn runs, plain text is queued as an **interjection** (see §7).

### 4.2 Print mode `-p` (one-shot, for scripts/CI)

Runs one full agent turn (tools included), prints the answer, exits:

```bash
pi -p "Use bash to run: echo PI_AGENT_LIVE_OK and reply with just the output."
```

```text
pi | model=nvidia/nemotron-3.5-lightning-30b-a3b | https://integrate.api.nvidia.com/v1
session: /root/.pi/agent/sessions/-workspace-build/6ac67da2160af9.jsonl
⚙ bash {"command":"echo PI_AGENT_LIVE_OK"}
PI_AGENT_LIVE_OK
```

Tool calls appear as yellow `⚙ name {args}` lines on **stderr**; the assistant text
streams on **stdout** — split them cleanly:

```bash
answer=$(pi -p "Summarize README.md in 3 bullets" 2>/dev/null)
```

### 4.3 Piped / non-TTY input

When stdin is not a terminal (`echo … | pi`, cron, CI) the fancy TUI is replaced by
plain line reading:

```bash
printf '/help\n/exit\n' | pi          # run commands from a pipe, exit cleanly
```

Each line is one command or one message — a pasted multi-line block becomes several
messages, so keep piped prompts on a single line (or use `-p`).

### 4.4 RPC server mode

```bash
pi --rpc --port 8787 --tools read,grep,find &
```

Listens on `127.0.0.1:8787` (never expose it publicly). One endpoint, `POST /rpc`,
JSON body `{"method": "...", "params": "..."}`:

```bash
curl -s localhost:8787/rpc -d '{"method":"prompt","params":"Read CMakeLists.txt and list the targets."}'
# {"reply":"targets: pi, test_session, …","session":"/home/you/.pi/agent/sessions/…/….jsonl"}

curl -s localhost:8787/rpc -d '{"method":"sessions"}'          # file + node count
curl -s localhost:8787/rpc -d '{"method":"tree"}'              # tree dump
curl -s localhost:8787/rpc -d '{"method":"compact"}'           # force compaction
curl -s localhost:8787/rpc -d '{"method":"fork","params":"a3"}' # move leaf to node a3
curl -s localhost:8787/rpc -d '{"method":"kill"}'              # kill switch: halt the agent
```

Aliases work too (`session.prompt`, `session.list`, `session.tree`, `session.fork`,
`session.compact`, `agent.abort`). Pair RPC with a read-only tool list
(`--tools read,grep,find`) so automation cannot modify files.

---

## 5. The Terminal UI & Keyboard

### Layout

```
… scrolling history of replies, tool output, status rows …
  ⚙ bash {"command":"ls"}
  README.md  src/  build/
pi: agent finished
› keep typing here — this line never moves_
```

- **Input line** is pinned to the bottom; output accumulates above it.
- Startup banner and every status message (running, queued, aborted…) are rows above
  the input.
- The whole screen repaints on activity (throttled to ~30 fps), so nothing flickers.

### Editing keys

| Key | Action |
|---|---|
| **Enter** (CR) | Submit the input buffer |
| **Ctrl+J** | Insert a newline (start a new input line) |
| **Shift+Enter** | Insert a newline too — if your terminal sends CSI-u (`ESC[13;2u`); otherwise use Ctrl+J |
| ← / →, **Ctrl+B** / **Ctrl-F** | Move cursor left / right |
| **Ctrl+A** / **Ctrl+E** | Home / End of the line |
| **Ctrl+W**, **Alt+B** | Delete previous word / move word back |
| **Alt+F** | Move word forward |
| **Alt+D** | Delete word under the cursor |
| **Alt+A** / **Alt+E** | Home / End (same as Ctrl+A / Ctrl+E) |
| **Alt+U** / **Alt+K** | Kill to start / kill to end (same as Ctrl+U / Ctrl+K) |
| **Backspace** | Delete character before cursor |
| **Delete** / **Ctrl+D** | Delete character under cursor (**Ctrl+D on an empty line = EOF/exit**) |
| **Ctrl+K** | Kill to end of line |
| **Ctrl+U** | Kill to start of line |
| **Ctrl+T** | Transpose the two characters before the cursor |
| **Ctrl+L** | Redraw the screen |
| **Ctrl+C** | Clear what you typed; **on an empty line, quit** |
| **Ctrl+X** style keys | not bound |
| **Tab** | Insert two spaces |
| **Home** / **End** | Start / end of line |

### History (↑ / ↓)

- Every submitted line is remembered (in-memory for this run, capped at 200,
  consecutive duplicates skipped).
- **↑** walks backwards, **↓** walks forwards; going past the newest entry restores
  whatever you were in the middle of typing.
- History is not persisted between runs — sessions (the JSONL tree) are the durable record.

### Multiline input

```text
› explain the difference
│ between these two loops
│ and give me a table
```

- **Ctrl+J** (or Shift+Enter) adds a line; continuation rows are drawn dim with `│ `.
- The caret stays on the active (last) row; the block grows upward from the pinned prompt.
- **Enter submits everything at once** as one message with embedded newlines.
- If the block is taller than the screen, older input rows collapse to `… N more line(s)`
  (nothing is lost — only the display is trimmed).

### Paste

Bracketed paste is enabled automatically (`ESC[?2004h`). Paste freely:

- Text is inserted **verbatim**, including spaces and punctuation.
- Newlines inside a paste become input-line breaks — **Enter inside a paste never
  submits**; you press Enter once, when you're ready.
- Works even without bracketed-paste support: pasted LF characters start new input
  lines instead of submitting (Enter/CR is the only submit key).

---

## 6. Slash Commands — Full Reference

`/help` prints this list in-app:

```
› /help
Slash commands:
  /help
  /login               list provider profiles (keys masked)
  /login <name>        connect: openai|nvidia|hf|ollama|lmstudio|llamacpp|<custom>
  /logout <name>       remove a saved profile
  /model               list models of the active endpoint (then /model <n>)
  /model <id|n|profile>|profile:id   switch model/provider at any time
  /session
  /new
  /resume:<path>
  /name:<name>
  /export:<path-or-empty>
  /copy
  /usage
  /tree
  /fork:<node-id>
  /compact
  /skill:<name>
  /abort               stop the running turn (while running: type to queue, /abort)
  /exit, /quit
```

Detailed behavior + examples:

### `/help`
Prints the list above.

### `/login`, `/login <name>`, `/logout <name>`
Provider profiles — see [§3](#3-configuration--providers).

```text
› /login ollama          # local Ollama, no key needed
› /logout ollama
```

### `/model` (and `/model:<id>`)
List / pick / switch models — see [§3](#3-configuration--providers).

### `/session`
Everything about the current session:

```text
› /session
id=6ac799905c9927
file=/home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
model=nvidia/nemotron-3.5-lightning-30b-a3b
base_url=https://integrate.api.nvidia.com/v1
cwd=/workspaces/abhikarta
nodes=12
leaf=a7
name=refactor-plan        ← only if you set one with /name:
```

### `/new`
Starts a fresh session file (the old one stays on disk — resume it later with
`/resume:` or `--resume`).

```text
› /new
new session: /home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac8012f.jsonl
```

### `/resume:<path>`
Load a previous session file and continue from its last node:

```text
› /resume:/home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
resumed: /home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
```

Tip: `/session` prints the current path — copy it now to resume tomorrow.
`/export:` with no path also prints it.

### `/name:<name>`
Label the session (recorded in the tree, shows up in `/session`):

```text
› /name:bugfix-null-ptr
named session as 'bugfix-null-ptr'
```

### `/export:<path>`
Copy the session JSONL somewhere (share/backup):

```text
› /export:/tmp/session-backup.jsonl
exported to /tmp/session-backup.jsonl
› /export:                 # no path → just print the current session file
session file: /home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799….jsonl
```

### `/copy`
Print the last assistant reply again (handy for copy-paste after it scrolled away):

```text
› /copy
Here is the refactored function…
```
`no assistant reply yet` if nothing answered so far.

### `/usage`
Token usage of the last assistant turn:

```text
› /usage
input=4210 output=388 cache_read=3072 cache_write=0
```
`no usage recorded` before the first reply.

### `/tree`
Render the current branch of the session tree (indent = depth; ids, roles, previews):

```text
› /tree
* [session] 6ac799905c9927
  * [message] a1  user: Create a python script fib.py…
    * [message] a2  assistant: I'll create the script…
      * [message] a3  tool: wrote 180 bytes to fib.py
      * [message] a4  assistant: Now running it… 1, 1, 2, 3, 5…
```

Use the ids to fork — see below.

### `/fork:<node-id>`
Move the active leaf back to an older node. The next prompt branches **from there**;
the newer work stays intact in the file (a real branch, not a delete):

```text
› /tree
* [message] a1  user: use Flask for the server
  * [message] a2  assistant: … Flask app …
    * [message] a3  user: add authentication
      * [message] a4  assistant: … added JWT …
› /fork:a1
forked at a1
› Actually, use FastAPI instead of Flask.
```

Now two children hang off `a1` — two alternative histories in one file. `/tree` shows
the branch you're on.

### `/compact`
Force context compaction now (see [§11](#11-context-window--compaction)):

```text
› /compact
{"goal":"…","progress_done":["…"],"next_steps":["…"], …}
```

### `/skill:<name> [args]`
Invoke a skill — see [§9](#9-skills-agentsmd--system-prompt):

```text
› /skill:pdf-tools convert invoice.pdf to text
```

### `/abort`, `/stop`
Stop the running turn (works only while a turn is running):

- Idle: `nothing running — /abort only stops an active turn`
- Running: `aborting at the next step...` → the worker halts at the next safe step.

### `/exit`, `/quit`
Leave the REPL. If a turn is running it is stopped first, then `pi` exits cleanly
(exit code 0). **Ctrl-D** on an empty line does the same.

> Unknown `/word` → `unknown command (try /help)`. While a turn is running, every
> slash command except `/abort`, `/stop`, `/exit`, `/quit` is answered with
> `[busy] agent running — /abort or /exit only` (see §7).

---

## 7. Working While the Agent Runs

Starts are asynchronous — the input line stays alive for the whole turn:

```text
› Refactor parser.cpp to use std::string_view and run the tests
(running — type to queue an interjection, /abort to stop)
  ⚙ read {"path":"parser.cpp"}
  ⚙ edit {"path":"parser.cpp", …}
```

### Interjections — just keep typing

Text typed during a run is **queued** and injected at the model's next step (you are
not interrupting, you're adding context):

```text
› Refactor parser.cpp and run the tests
(running — type to queue an interjection, /abort to stop)
also mention a lighthouse in the summary
[queued] injected at the next LLM step: also mention a lighthouse in the summary
```

Slash commands while busy (except the four allowed ones) are rejected:

```text
› /model
[busy] agent running — /abort or /exit only
```

### Abort

```text
› /abort
aborting at the next step...
ABORTED                                    ← turn ends, input is free again
```

Idle `/abort` just reminds you: `nothing running — /abort only stops an active turn`.

### Exiting mid-turn

`/exit` (or Ctrl-D) while running stops the turn, joins the worker, restores the
terminal, exits 0.

### Tool approvals — `--approve-tools`

```bash
pi --approve-tools
```

Before a guarded tool runs you get:

```text
[approve] allow bash {"command":"rm -rf build"}? [y/N]:
```

Answer `y` / `yes` (anything else = deny; deny is reported to the model as an error
so it adapts). Guarded: `bash`, `write`, `edit`, `grep`, `find` — `read` never asks.
Your answer is read by the same input loop, so you can answer while a turn is running.

---

## 8. The Tools

The model can call these (descriptions + JSON schemas are sent to the LLM):

| Tool | Arguments | Notes |
|---|---|---|
| `read` | `path`, `offset?` (default 1), `limit?` (default 2000) | numbered lines, only within the workspace |
| `bash` | `command` | runs in the session cwd, stderr merged, output capped at 64 KB, `[exit N]` appended |
| `edit` | `path`, `oldText`, `newText` | **exact, unique** replacement — fails if `oldText` is missing or appears twice |
| `write` | `path`, `content` | create/overwrite, workspace only |
| `grep` | `pattern`, `path?` | literal (fixed-string) recursive search, first 200 hits — **off by default** |
| `find` | `glob`, `path?` | `find -name` style glob, first 200 hits — **off by default** |

### Allow-lists

```bash
pi --tools read,bash,edit,write      # default-ish set
pi --tools read,grep,find            # read-only research agent (recommended for RPC)
pi --tools bash                      # shell-only
```

Any call outside the allow-list is refused and reported to the model as
`ERROR: tool 'x' is not enabled (allow-list enforced)` — the model cannot bypass it.

### Workspace sandbox

`read` / `write` / `edit` / `grep` / `find` refuse paths outside the directory `pi`
started in: `path outside workspace root: /etc/passwd`. `bash` runs in that directory
too — start `pi` where you want the agent to work.

### Seeing tool activity

Tool lines go to **stderr** in color:

```
⚙ bash {"command":"pytest -q"}
⚙ result:
3 passed in 0.02s
```

Redirect as needed: `2>/dev/null` to hide them, `2>&1` to interleave with the reply.

---

## 9. Skills, AGENTS.md & System Prompt

### AGENTS.md — always-on project memory

Read at startup from **two** places and appended to the system prompt (each under a
`## <path>` header):

```bash
# project rules (this repo)
cat >> /workspaces/abhikarta/AGENTS.md <<'EOF'
Always run cmake --build build_tls && ctest --test-dir build_tls before finishing.
Keep answers concise; use file:line references.
EOF

# personal rules (all projects)
cat >> ~/AGENTS.md <<'EOF'
Prefer tables over prose when listing options.
EOF
```

### System prompt files

| File | Effect |
|---|---|
| `~/.pi/system.md` | **replaces** the built-in system prompt entirely |
| `~/.pi/append-system.md` | appended after the built-in (or `system.md`) prompt |
| `--system-prompt "…"` | one-shot override from the command line |

### Skills — lazy-loaded knowledge

A skill is a markdown file with front matter. Scanned at startup from:

```
~/.pi/agent/skills/<name>/SKILL.md     # directory style
~/.pi/agent/skills/<name>.md           # flat style
<cwd>/.agents/skills/<name>/SKILL.md   # per-project
<cwd>/.agents/skills/<name>.md
```

Create one:

```bash
mkdir -p .agents/skills/pdf-tools
cat > .agents/skills/pdf-tools/SKILL.md <<'EOF'
---
name: pdf-tools
description: Convert and extract text from PDFs using pdftotext and python.
---
1. Extract text:  pdftotext input.pdf output.txt
2. Pages only:    pdftotext -f 1 -l 3 input.pdf out.txt
3. If pdftotext is missing: python3 -c "import pypdf; …"
EOF
```

Only `name` + `description` (+ location) enter the system prompt as a one-line marker.
When you invoke it, `pi` injects a `<skill … location="…"/>` block and the model
**reads the file itself with the read tool** — context stays cheap:

```text
› /skill:pdf-tools convert invoice.pdf to text
```

Rules:
- `name` and `description` are required (invalid files are skipped with a warning).
- Names must be unique across roots (duplicates are skipped with a warning).
- Skills are scanned **once at startup** — restart `pi` after adding or editing one.
- List what's available: search the system prompt marker block, or just ask
  `what skills do you have?` — the model sees `<available_skills>`.

---

## 10. Sessions, the Tree, Forking & Export

### Storage

```
~/.pi/agent/sessions/<sanitized-cwd>/<session-id>.jsonl
e.g. ~/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
```

One JSON object per line = one **tree node** (append-only, crash-safe, greppable):

```jsonl
{"id":"6ac799…","parentId":null,"type":"session","cwd":"/workspaces/abhikarta"}
{"id":"a1","parentId":"6ac799…","type":"message","message":{"role":"user","text":"…"}}
{"id":"a2","parentId":"a1","type":"message","message":{"role":"assistant","text":"…","toolCalls":[{"name":"bash","args":"{…}"}],"usage":{"input":812,"output":140}}}
{"id":"a3","parentId":"a2","type":"message","message":{"role":"tool","text":"…"}}
{"id":"mc","parentId":"a3","type":"model_change","summary_json":"{\"model\":\"…\"}"}
{"id":"s1","parentId":"a2","type":"summary","summary_json":"{\"goal\":\"…\"}"}
```

`parentId` makes branching free: nothing is rewritten, branches just coexist.

### Everyday flow

```text
› /name:tue-refactor
named session as 'tue-refactor'
… work …
› /session                     # note the file path
› /export:/tmp/tue-refactor.jsonl
exported to /tmp/tue-refactor.jsonl
› /new                         # clean slate, old tree preserved
```

### Tomorrow

```bash
pi --resume ~/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
# or inside the REPL:
› /resume:/home/you/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
```

Resuming restores the last leaf (and records a `model_change` node if you now run a
different model, so the history stays honest).

### Branching experiments

```text
› /tree
* [session] 6ac799…
  * [message] a1  user: implement cache with an LRU
    * [message] a2  assistant: … LRU version …
› /fork:a1
forked at a1
› Instead implement an LFU cache.
```

Inspect files directly anytime:

```bash
tail -3 ~/.pi/agent/sessions/*/*.jsonl | python3 -m json.tool
grep '"role":"user"' ~/.pi/agent/sessions/-workspaces-abhikarta/*.jsonl
```

---

## 11. Context Window & Compaction

- `--window <n>` = context budget in tokens (default **8192**).
- Context size is measured from **provider-reported usage**
  (`input + output + cache_read + cache_write`) — not by counting characters.
- At **85% of the window** compaction kicks in automatically (before your prompt and
  after each turn); you can always run it yourself:

```text
› /compact
```

- Compaction asks the model to summarize the older history into a structured
  checkpoint — **Goal · Constraints · Progress (done / in progress / blocked) ·
  Key decisions · Next steps · Critical context** (exact file paths, function names
  and error messages are preserved) — appended to the tree as a `summary` node.
  From then on, requests rebuild history from the summary + the recent tail.
- Watch consumption:

```text
› /usage
input=4210 output=388 cache_read=3072 cache_write=0
```

Force the issue for testing:

```bash
pi --window 1200        # small window → compaction arrives quickly
```

Safety limits enforced by the loop: kill switch, **max 64 tool rounds** per turn,
a cumulative token budget (default 1,000,000; `0` = unlimited), and the tool allow-list.

---

## 12. Recipes — Worked Examples

### A. First coding session

```text
› /login nvidia
› /name:build-cleanup
› Read CMakeLists.txt, list every target, then propose how to split the test targets.
› (read the reply) /copy          ← reprints it for copying
› Do target 2 first — build and run only test_config.
› /usage
› /exit
```

### B. Read-only research agent (safe for automation)

```bash
pi --tools read,grep,find
```

```text
› grep the repo for "TODO" and group the hits by file
```

`bash`/`write`/`edit` are not in the allow-list — calls come back as
`ERROR: tool 'bash' is not enabled` and the model works around them.

### C. One-shot scripting with `-p`

```bash
# answer only, tool noise discarded
pi -p "Summarize the 5 biggest files under src/ by line count (use bash)." 2>/dev/null

# let the agent write a file for you (write tool), no TTY needed
pi -p "Create a Makefile in this directory with all/nightly/test targets for the C++ sources in src/." 2>/dev/null
ls Makefile
```

### D. Long task with automatic context management

```bash
pi --window 32768
```
```text
› Work through the checklist in TODO.md item by item; after each item run the tests.
(running — type to queue an interjection, /abort to stop)
skip item 3, it is obsolete            ← typed mid-run
[queued] injected at the next LLM step: skip item 3, it is obsolete
```
Compaction keeps the context under control automatically; `/usage` shows where you are.

### E. What-if branching

```text
› /tree                    → pick node a1
› /fork:a1
› Re-do that change using std::variant instead of std::any.
```
Two approaches now live side by side in one JSONL file; `/tree` shows the active branch.

### F. Custom skill

```bash
mkdir -p ~/.pi/agent/skills/release-notes
cat > ~/.pi/agent/skills/release-notes/SKILL.md <<'EOF'
---
name: release-notes
description: Generate release notes from git log since the last tag.
---
1. git describe --tags --abbrev=0
2. git log --pretty=format:'- %s (%an)' $(git describe --tags --abbrev=0)..HEAD
3. Group into: Features, Fixes, Chores.
EOF
```
```text
› /skill:release-notes draft the notes for the current branch
```

### G. Provider-hopping

```text
› /login ollama                 # local, no key
› /model                        # what does Ollama serve?
› /model 3                      # pick #3
› … work offline …
› /model nvidia                 # back to the cloud profile
› /model nvidia/nemotron-3.5-lightning-30b-a3b
```

### H. Automation over RPC (read-only)

```bash
pi --rpc --port 8787 --tools read,grep,find &
curl -s localhost:8787/rpc -d '{"method":"prompt","params":"List the exported symbols in src/core/tools.hpp."}'
curl -s localhost:8787/rpc -d '{"method":"tree"}'
curl -s localhost:8787/rpc -d '{"method":"kill"}'
```

### I. Approval-gated destructive run

```bash
pi --approve-tools
```
```text
› Delete build/ and rebuild from scratch.
[approve] allow bash {"command":"rm -rf build"}? [y/N]: y
[approve] allow bash {"command":"cmake -S . -B build"}? [y/N]: y
```

### J. Continue yesterday

```bash
pi --resume ~/.pi/agent/sessions/-workspaces-abhikarta/6ac799905c9927.jsonl
```
```text
› Where did we leave off? /tree
```

---

## 13. Quick Reference Cheat Sheet

### Keyboard

| Keys | Meaning |
|---|---|
| Enter | submit |
| Ctrl+J / Shift+Enter | newline in the input |
| Ctrl+A · Ctrl+E | home · end |
| Ctrl+B · Ctrl+F / ← → | move left · right |
| Ctrl+W · Alt+B · Alt+F | kill word · word left · word right |
| Alt+D | delete word |
| Ctrl+K · Ctrl+U | kill to end · kill to start |
| Ctrl+T | transpose |
| Ctrl+D | delete char (empty line = quit) · Ctrl+C clear (empty line = quit) |
| Ctrl+L | redraw · Tab = two spaces |
| ↑ / ↓ | history (previous / next) |

### Slash commands

```
/help  /login [name]  /logout <name>  /model [spec]  /session  /new
/resume:<path>  /name:<name>  /export:[path]  /copy  /usage  /tree
/fork:<id>  /compact  /skill:<name>  /abort|/stop  /exit|/quit
```

While running only: `/abort` · `/stop` · `/exit` · `/quit` — everything else
answers `[busy]`. Plain text during a run = queued interjection.

### Flags

```
-b, --base-url URL        endpoint                 -k, --api-key KEY      key
-m, --model ID            model                    -p, --prompt TEXT      one-shot
-r, --resume FILE.jsonl   resume session           -t, --tools a,b,c      allow-list
--system-prompt TEXT      override prompt          --window N             context (8192)
--rpc [--port N]          HTTP server              --approve-tools        y/N gate
-h, --help                usage
```

### Environment

```
PI_API_KEY / NVIDIA_API_KEY / OPENAI_API_KEY     PI_BASE_URL     PI_MODEL
~/.pi_agent.env                                  (auto-loaded KEY=value file)
```

### Files

```
~/.pi/config.json                profiles (0600)
~/.pi/agent/sessions/…/*.jsonl   session trees
~/.pi/agent/skills/…             personal skills
<cwd>/.agents/skills/…           project skills
~/AGENTS.md  <cwd>/AGENTS.md                    memory injected into the prompt
~/.pi/system.md  ~/.pi/append-system.md   prompt override / append
```

---

## 14. Troubleshooting

| Symptom | Cause → Fix |
|---|---|
| `unknown command (try /help)` | typo'd slash command → `/help` for the list |
| `[busy] agent running — /abort or /exit only` | a turn is running — queue plain text instead, or `/abort` |
| `nothing running — /abort only stops an active turn` | you aborted while idle (expected, harmless) |
| `401 Unauthorized` | wrong/expired key → `/login <name>` again, or fix `PI_API_KEY`; rotate at build.nvidia.com |
| `cannot list models: …` | endpoint unreachable → `/model` falls back to the cached list if it has one |
| `path outside workspace root: …` | the file tool is sandboxed to the directory `pi` started in → `cd` there and restart |
| `oldText not found` / `not unique` | `edit` needs an exact, unique match → include more context |
| `ERROR: tool 'x' is not enabled` | tool not in `--tools` allow-list |
| TLS / SSL errors on https | rebuild: `cmake -S . -B build_tls -DPI_WITH_TLS=ON && cmake --build build_tls` |
| Reply stops mid-loop | hit the 64-round tool cap or the window → `/compact`, raise `--window`, or split the task |
| Nothing but noise in scripts | tool lines are on **stderr** → `2>/dev/null` |
| Typed before the prompt appeared / piped input acted strange | in a TTY, wait for the `›` prompt; with pipes each line is one command (use `-p` for one-shots) |
| Want an old conversation back | `pi --resume <file.jsonl printed by /session>` |
| Terminal looks garbled after a crash | `reset` — or just rerun `pi` (it restores the terminal on every exit path) |
| Key visible in a log/screenshot | rotate it immediately, then update `~/.pi_agent.env` / the profile |

---

*Pi-Agent user guide · binary `pi` · profiles `~/.pi/config.json` (0600) ·
sessions `~/.pi/agent/sessions/`.*
