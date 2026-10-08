# Pi-Agent Security & Architecture Audit Report

*Prepared: October 8, 2026*

## Executive Summary

This audit reviewed the `pi` terminal agent (C++ core, cpp-httplib, json.h, session-tree JSONL, tool runner, RPC mode). The current design is coherent and preserves the project's intended direction (minimal C/C++ core, no RAG, durable session-tree memory). However, **the execution boundary is insufficiently hardened**.

**Verdict:** **Pre-release**. Hardening is required before any public or multi-user deployment. Do not expose RPC with the current tool set.

### Priority Ranking (Team Suggested)

| Priority | Area | Rationale |
|---|---|---|
| **P0** | `bash` tool sandbox (execution layer) | Arbitrary shell execution is the only practical full-compromise path. Current implementation lacks timeouts, process-group control, rlimits, env scrubbing, canonicalization, and an audit trail. Also interacts directly with untrusted model-generated strings. |
| **P1** | Session-tree durability & recovery | Append-only JSONL is the source of truth for history/branching. Gaps: no `schema_version`, no fsync/atomic write, corrupt lines silently dropped (`session.hpp:98`), directory/permissions hygiene, and no integrity checks. Loss of history or silent corruption degrades auditability. |
| **P2** | C++ runtime/TUI concurrency model | Interjection queue is mutexed and atomics are used; overall discipline is reasonable. Main risks: approval-stdin-EOF (blocking), lifetime/worker join on exits, signal handling, and repaint race conditions under resize. Fewer high-impact issues per effort. |

---

## 1. Scope, Methodology & Framing

### Scope
- **Source inspected**: `src/core/agent.hpp`, `src/core/tools.hpp`, `src/core/session.hpp`, `src/core/console.hpp`, `src/core/http.hpp`, `src/core/client.hpp`, `src/core/context.hpp`, `src/core/config.hpp`, `src/main.cpp`, tests (`test_*.cpp`)
- **Evidence by file:line** (cited throughout). Focus: tool authorization/execution, path safety, session I/O, RPC exposure, approval flow, subprocesses, concurrency.
- **Not reviewed**: third-party dependencies (cpp-httplib/json.h) beyond their integration points; provider-side IAM/policies; network egress beyond API calls; packaging/distribution.

### Methodology
Code review + static grep for subprocesses/timeouts/rlimits/signals/fsync, RPC/approval paths, and test coverage. Findings mapped to:
- **NIST AI RMF** (Govern/Map/Measure/Manage) and generative-AI profile
- **OWASP AI Agent Security Cheat Sheet** (execution-layer authorization, call-bound approvals, untrusted data handling, adversarial testing)

### Severity Definitions
| Sev | Definition |
|---|---|
| **Critical (S0)** | Exploitable RCE, workspace escape, secret exfiltration, persistent auth bypass. Must fix before any release. |
| **High (S1)** | High-impact abuse (shell injection, tokenless RPC with tools, approval replay/unauthorized execution). Must fix before release. |
| **Medium (M1)** | Reliability/auditability or partial hardenings (durability, fsync, schema). Must fix before release unless explicitly risk-accepted. |
| **Low (L1)** | UX/observability, polish, minor resilience gaps. Post-v1 acceptable if tracked. |
| **Informational (I1)** | Architecture recommendation, test coverage expansion. |

## 2. Expert Panel → Suggested Audit Areas (Team Alignment)

The user-proposed 12-role panel mapped to concrete work against the observed codebase. The team’s recommendation: audit **bash sandbox** first, **session durability** second, **runtime/TUI concurrency** third.

| Expert Role | Proposed Focus | Deliverable (Week 1 target) |
|---|---|---|
| Agent-runtime architect | State machine, turn loop, cancellation, interjections, tool-round limits | Runtime state diagram (states: idle/running/busy-waiting-approval/stopping), invariants (exactly-one-turn active, interjection queue semantics), validate max_tool_rounds=64 behavior. |
| C++ systems engineer | RAII, ownership, threads, signals, subprocesses | Subprocess safety (popen→execve analysis), signal review (SIGINT/SIGWINCH), lifetime/joins, potential leaks; recommendations for rlimits/timeouts. |
| LLM/provider engineer | OpenAI-compatible, streaming, tool schemas, timeouts/retries | Provider adapter contract; review http.hpp/client.hpp for timeouts/retry/backoff (currently none obvious); compatibility matrix. |
| Terminal/TUI engineer | Raw mode, repaint, Unicode, resize, bracketed paste | TUI failure modes (resize race, CR/LF, long lines), confirm Console::read_line/ask behavior, test clipboard/paste boundaries. |
| **Security engineer (lead, P0)** | Sandbox escape, injection, secrets, RPC exposure | **Threat model** for bash+paths+RPC, reproductions (symlink, injection, RPC abuse), concrete hardening patches. |
| AI-agent safety engineer | Prompt injection (indirect/direct), confused deputy, tool chaining | Call-bound approval spec (nonce/expiry/binding+consumption), untrusted data policy (AGENTS.md/SKILL.md/tool output), policy engine placement. |
| **Storage/reliability engineer (P1)** | JSONL durability, crash recovery, branching | Schema v0.1 + fsync/O_APPEND+atomic rename, recovery scanner (don’t silently drop), corruption corpus, mkdirs/permissions (0640/0600). |
| DevOps/SRE engineer | Logging, observability, CI, SBOM, release gates | CI with ASan/UBSan/TSan, sanitizer jobs, reproducible TLS build, incident/security response checklist, SLO ideas. |
| QA/fuzzing engineer | Parsers, terminal input, JSONL, paths, tool args | Fuzz targets (json.h inputs, JSONL recovery, escape sequences, path traversal, tool args), integration corpus. |
| Privacy/compliance reviewer | Keys, transcripts, retention, export, RPC | Data classification, retention/export guidance, secret handling (masking exists), RPC local-only posture + auth. |
| Power users (C++, researcher, sysadmin, accessibility) | Real workflows | Usability: approve gates, busy states, interjections, `/abort`, recipes validation. |
| Documentation engineer | Accuracy, copy-paste, versioning, executable docs | Keep `docs/user-guide.md` fact-correct, add security notes, `/doctor` reference, examples tagged. |

## 3. High-Priority Findings (Evidence + Repro + Remediation)

### S1: `sh -c` subprocess execution with no safety controls — **Critical (S0–S1)**

**Evidence:**
- `src/core/tools.hpp:111` `FILE* pp = popen(full.c_str(),"r");` for `bash(cmd)` (and 150,161 use `popen` with concatenated strings)
- No `alarm/timeout`, no `setpgid/killpg`, no `setrlimit` (CPU/mem/FD), no `PATH` sanitization, no environment allow-list, no cwd enforcement beyond tool entry, no audit record (command/cwd/duration/exit/digest)

**Impact:** Prompt injection → arbitrary command execution as invoking user; can access SSH, API keys, `.env`, config, sessions, workspace symlinks, network. Also unkillable on hang.

**Repro (concept):**
1. In `-p` or REPL, model/poisoned content causes tool args `cmd` to be `"rm -rf /tmp/foo; curl ..."` via indirect injection (documents/AGENTS). 
2. `grep`/`find` take `pattern/path` from untrusted strings and interpolate into shell command (string concatenation). 

**Must fix (pre-release):**
- **Principle:** Prefer `execve` with fixed argv for structured ops (`grep` → spawn `rg`/`grep` with `--` + args; `find` → native walker or `find` with explicit argv). 
- If general `bash` remains (elevated), enforce: timeout (e.g. 60s default, configurable), `setpgid` + `killpg` on timeout, `setrlimit` (RLIMIT_AS/RLIMIT_CPU/RLIMIT_FSIZE/NOFILE), sanitized `PATH` (minimal), env allow-list (strip tokens except needed), strict cwd = workspace root, audit log entry (ts, sess, node, cmd, cwd, exit, sig, bytes, duration), output cap (currently reader chunks but no hard kill). 
- Add `--` separators for user-supplied filenames/patterns; refuse empty/blank commands; block absolute commands? at least log+policy. 
- Consider `seccomp` on Linux later; for v1 process-group + rlimits + timeout + canonical paths are mandatory.

**Post-v1:** seccomp-bpf allow-list, syscall auditing.

### S2: Path traversal/symlink escape via lexical normalization only — **Critical (S0)**

**Evidence:**
- `src/core/tools.hpp:31` `static bool path_in_workspace(const std::string& path, const std::string& workspace_root) {` uses `std::filesystem::lexically_normal` (not `std::filesystem::weakly_canonical`/`canonical`/`realpath`). 
- Called from `read/write/edit/grep/find` before execution/IO (`96,124,133,148,159`).

**Impact:** Symlink inside workspace points outside → bypasses workspace sandbox; read/write arbitrary files, exfiltrate secrets, overwrite configs.

**Repro:**
- Create `ln -s / /workspaces/abhikarta/escape` or `ln -s /etc/passwd rel`
- Request `read("escape/etc/shadow")` or edit under symlink → `path_in_workspace` may pass lexical check depending on traversal; canonicalization not performed. 

**Must fix:**
- Canonicalize both sides: `std::filesystem::canonical(workspace_root)` (or weakly_canonical with error handling), and `canonical(resolved_path)` with base check `std::filesystem::path(p).lexically_normal().make_preferred()` + `starts_with` against canonical root. 
- Handle broken symlinks safely (resolve parent dirs). 
- Reject paths whose canonical form leaves workspace, return explicit denial. 
- Add unit tests for `../../`, `.`/`..`, symlinks in/out, absolute paths. 

Note: `std::filesystem::canonical` throws on non-existent paths; use `weakly_canonical` + existence checks, or resolve parents.

### S3: Shell injection via concatenated args (grep/find) — **High (S1)**

**Evidence:**
- `tools.hpp:150–156` `grep`: builds `"grep -rHn '" + pattern + "' " + path` and `popen`s it
- `tools.hpp:161–167` `find`: builds `"find " + path + " -name '" + pat + "'"` and `popen`s it
- Pattern/path come from tool args (untrusted model input) and are single-quoted but quoting is not injection-proof if `pattern` contains `'` or shell metacharacters; also `path` untrusted. 

**Impact:** `grep` arg `pattern` like `foo'; cat /etc/passwd #'` injects commands; `find -name` injection similar or via semicolons.

**Must fix:**
- Do **not** interpolate untrusted strings into `sh -c` shell command for these ops. Spawn `grep` (or `rg`) with argv: `{"grep","-rHn", pattern, path,...}` via `posix_spawn`/`execve` (no shell). 
- If shell required, use `bash -c` only with a fixed template + argument array passed via `-c` and `$1` (avoid constructing one big shell string). 
- Enforce allow-list chars for `pattern` if must, but not sufficient; prefer argv form. 
- For `find`, same: use `find` with `-path`/fixed predicates and `--` + paths, or native filesystem walk in C++ (smaller attack surface). 

**Test cases:** `a'b`, `$(id)`, backticks, newlines, `;`, `|`, `&`, spaces.

### S4: RPC mode exposed with tools, no auth, no approval path — **High (S1)**

**Evidence:**
- `src/main.cpp:453–487`: `--rpc` binds `127.0.0.1:<port>` (loopback correct), `POST /rpc`, methods: `prompt/session.prompt`, `sessions`, `compact`, `tree`, `fork`, `kill`. 
- `approval_waiting`/`approve-tools` only exist in interactive REPL path (`173–205`, `515`). No approval gate, no auth token, no UDS option. 
- `set_payload_max_length(8u<<20)` present. Tool set determined by `--tools`/config (defaults include `read,bash,edit,write` unless restricted). 
- `svr.listen("127.0.0.1", ...)` blocks; no rate limits, no session isolation across callers.

**Impact:** Any local process can POST to `/rpc` and trigger `session.prompt` (agent turn) which may call `bash/write/edit` with current config/approval policy. Without `approve-tools` active (or in RPC context where stdin answers never arrive), dangerous tools execute. Also `kill` and `fork`/tree manipulation possible.

**Must fix:**
- **Default safe:** RPC tool set read-only by default (e.g. `--tools read,grep,find` disabled or `--rpc-tools` minimal). Never allow `bash,write,edit` in RPC unless explicitly opted in + authenticated. 
- **Auth:** random bearer token (per-run or configured), or Unix Domain Socket (`AF_UNIX`) with socket permissions (0600). Reject missing/wrong auth with 401 + no body leakage. 
- **Policy in executor (not UI):** policy engine must run for RPC requests too — approval callback must not require interactive stdin. Require call-bound approvals or disable dangerous tools in headless RPC. 
- **Limits:** rate limiting (per-IP/path), max concurrent prompts, request timeout, response-size caps, method allow-list per role. 
- **Exposure guard:** warn if `rpc` enabled with dangerous tools; log all RPC calls (method, params summary, ip, duration, status). 
- **Future:** localhost is local-only but multi-user systems share it; token+UDS preferred.

**Post-v1:** mTLS between local clients, capability-based tokens.

### S5: Conversational approval is unauthenticated, unbound, non-atomic — **High (S1)**

**Evidence:**
- `src/main.cpp:194–205`: `set_approval([&](name, args_json){ approval_waiting=true; print [approve] allow <name> <args_json>? [y/N]:; getline(std::cin,line); approval_waiting=false; return line starts y/Y/yes/YES; })` 
- Only interactive REPL; blocks worker on `LineBus` answer (`515: if (approval_waiting.load()) { linebus.push(line); continue; }`)
- No nonce, session id, expiry, tool+args hash, call id, or “consume once” (replay possible if UI reused). Approval binds to no specific invocation/round; any subsequent approval prompt can consume any waiting answer in that simple loop (timing-dependent). 
- `src/core/tools.hpp:81–83`: checks `approval_(name, args_json)` once at tool entry; if denied returns error; no re-approval.

**Impact (OWASP-aligned):** Approvals must be execution-layer, call-bound, atomic. Conversational “yes” is vulnerable to injection/redirection and cannot be safely exposed to RPC or scripted callers; also confused deputy risk.

**Must fix:**
- **Call-bound approval record:** include `session_id`, `turn_id`/`node_id`, `tool_call_id`, `tool_name`, canonicalized `args_json` (normalized), nonce, created_at+expires_at, user_id. 
- **Verify+consume atomically:** policy returns allow/deny once per invocation id; nonce invalidated after use. 
- **Headless-safe:** in non-interactive/RPC, approval callback must not block on stdin — require explicit policy or deny dangerous tools. 
- **Change detection:** any parameter change requires new approval (do not trust “same tool”). 
- **Least privilege:** prefer per-tool allow-lists + argument constraints (paths under workspace, read-only, command prefixes) over global y/N. 
- **Audit:** log approval prompts/decisions with full binding (no args that may contain secrets; mask values). 

**Post-v1:** TOCTOU hardenings, approval cache by hash with expiry.

### M1: Session-tree durability, schema integrity, corruption handling — **Medium (M1)**

**Evidence:**
- `src/core/session.hpp:86–88`: append uses `std::ofstream f(file, std::ios::app); f << to_jsonl(n) << "\n"; f.flush();` — no `fsync` (OS page cache, crash after flush before disk). `O_APPEND` not explicit (default open mode). 
- `src/core/session.hpp:95–101`: `while (std::getline(f,line)) { if empty continue; if (!d.parse(line)) continue; ... }` — **silently drops corrupt lines** (no warning/counter/repair). Broken parent references not detected. 
- `src/core/session.hpp:30–38`: `mkdirs` does `::mkdir(..., 0755)` and ignores EEXIST — session dir 0755, files opened with default flags (likely 0644). No `umask`/permissions hardening. 
- `src/core/session.hpp:149–165`: JSONL fields: `parentId` null for root, no `schema_version`, no content hash, no `tool_call_id` at node level (tool calls inside message), no event subtype/versioning. 
- `src/core/session.hpp:26–29`: `sanitize_path(cwd)` maps any non-alnum to `-` (over-sanitization) but fine for session dir names. 

**Impact:** Crash-recovery: last appended nodes may be lost on power loss (missing fsync). Corruption: partial line/write → load() drops it silently, tree becomes inconsistent (branch() breaks). Forensics/auditability reduced.

**Must fix:**
- **Atomic append + durability:** open with `O_WRONLY|O_APPEND|O_CREAT, 0600` (POSIX) or ensure `ofstream` uses appropriate mode; after write: `f.flush(); f.rdbuf()->pubsync()` or `fsync(fd)`; on new file creation set mode. Prefer atomic rename for header writes if any. 
- **Schema versioning:** add `schema_version` (e.g. `"schema_version": 0.1`) to `session` header and validate on load. Node required fields: `id,parentId,timestamp,type,schema_version?` plus optional integrity fields (`node_hash`, `content_hash`, `prev_hash`, `tool_call_id`). 
- **Robust recovery:** do not silently drop parse errors — log count `corrupt_lines=N`, last good offset/line, keep quarantined `file.corrupt.N.jsonl`, or stop/resume with error. Validate: root exists, parentIds resolve or mark `orphan`, cycle detection in `branch()`. 
- **Permissions:** `mkdirs` 0700 for `~/.pi/` subtree (`~/.pi/agent/sessions/...`), session files 0600 (transcripts may contain sensitive tool output). Use `umask(077)`. 
- **Integrity (optional v1):** append chain hash (`prev_hash`) to detect truncation/tampering; content_hash over body. 
- **Load robustness:** detect trailing partial line (no `\n`), EOF mid-write. 

**Post-v1:** compaction must record which node IDs were compressed (explicitly lossy) and keep a compaction manifest.

### M2: Testing, CI, sanitizers, HTTP resilience — **Medium (M1)**

**Evidence:**
- Tests: `tests/test_config.cpp`, `tests/test_loop_offline.cpp`, `tests/test_session.cpp`, `tests/test_tools.cpp` — small suite (4 files). No `.github/workflows/` directory. 
- No `-fsanitize=address,undefined` in build; no fuzz targets, no adversarial tests. 
- HTTP: `src/core/http.hpp`/`client.hpp` — grep shows no obvious `CURLOPT_TIMEOUT`, `set_read_timeout`, retry/backoff (timeouts/retry section returned empty). Provider timeouts/partial streams may hang. 
- Signals: no `sigaction`/`SIGWINCH` resize handler visible (console redraws on read but resize not explicitly handled); `SIGINT` handled implicitly by raw mode/REPL loop. 
- Approval-in-RPC path untested. 

**Must fix (pre-release):**
- **CI (week 1):** add minimal workflow: build TLS (`-DPI_WITH_TLS=ON`) + plain, run `ctest`, clang/gcc with `-Wall -Wextra -Wpedantic`, `ASan`, `UBSan` (Linux). Fail on warnings where feasible. 
- **Unit tests:** path normalization/symlink cases, grep/find injection resistance (argv form), JSONL recovery/corruption, approval binding/consumption, cancellation (`/abort`, kill), RPC 401/token, tool allow-list. 
- **Integration:** timeouts, process hang (timeout kill), disk full, partial stream, fork/compact, provider switching. 
- **HTTP resilience:** add connect/read timeouts, max response size, retry with jitter for idempotent GETs, backpressure, circuit-breaker later. 
- **Fuzz (post-v1 foundation):** libFuzzer targets for json parser, JSONL lines, terminal escape sequences, pasted input, path strings, tool args. 

**Post-v1:** TSan for worker+TUI, SBOM/license scan, reproducible builds, coverage gates.

### L1: Additional Observations (low/informational)

| Observation | Evidence | Recommendation |
|---|---|---|
| Interjections thread-safety | `agent.hpp:85–107, 280–341` uses `std::mutex` + `std::atomic<bool> stop_requested_` | Reasonable. Document invariants (queue never lost while running). Consider bounded queue (drop/notify on overflow). |
| Compaction policy | `agent.hpp:18, 154, 231, 0.85*window` usage-based auto-compact | Explicitly lossy — record compressed node IDs in compaction node/manifest; `/compact` user-visible. Preserve decisions/errors. |
| Config precedence visibility | Not exposed | Add `/config` or `/doctor` command showing effective source of each setting (flag/env/profile/default), masking keys. |
| Console raw mode | `console.hpp:41–184` enables/disables bracketed paste, cursor visibility | Good. Add SIGWINCH handler to recompute screen_rows/cols and redraw pinned input; handle EIO/termios errors on restore. |
| Skills scan once | Skills loaded at startup (per docs) | Explicit in guide — keep “restart after editing skills” note (already present). |
| `agent_end` dedupe | `main.cpp` no reprint | As implemented; test non-TTY vs TTY. |
| `/abort` idle message | `main.cpp:264–267` “nothing running — /abort only stops an active turn” | Correct UX (matches code). |
| Non-slash → network | `handle_line` sends non-slash to start_turn (NETWORK) | Important testing note (PTY tests use slash). |


## 4. Team-Suggested Areas Expansion (What to Expand)

The audit panel’s suggestion is to **separate concerns** so the agent decides *what* it wants; policy/executor decides *what may happen*. Expand project boundaries to enforce that split:

| Expansion | Purpose | Files/Components | Priority |
|---|---|---|---|
| **Policy Engine** | Centralize authorization (tools, paths, env, RPC). Remove policy from UI (`main.cpp` approval callback). | `src/core/policy.hpp` (new), `tools.hpp` use policy, `main.cpp` inject IPolicy | P0 (unblocks S2–S5) |
| **Typed Tool API + Schema Validation** | Represent tool calls as typed structs; validate args against JSON schema before exec; normalize args; canonicalize paths. Avoid shell-string policy. | `src/core/tool_api.hpp`, per-tool validators | P0 |
| **Execution Executor (sandboxed)** | Isolate subprocesses: argv spawn, timeouts, pgrp, rlimits, env-scrub, audit log sink. | `src/core/executor.hpp` (new), replace `popen` with executor | P0 (S1–S3) |
| **Versioned JSONL Schema + Recovery** | `schema_version`, integrity fields, recovery scanner (don’t drop silently), orphan/cycle checks, corruption quarantine. | `session.hpp` types+load/recover | P1 (M1) |
| **RPC AuthN/Z + Safe Defaults** | Bearer token or UDS+0600, per-method allow-list, `rpc-tools` minimal, rate limits. | `main.cpp` RPC handler | P0 (S4) |
| **Approval Binding (call-bound)** | Nonce+expiry+binding+atomic consume; headless denial. | `policy.hpp`, `agent.hpp` tool_call_id propagation | P0 (S5) |
| **Observability/Audit Log** | Append-only audit for: tool_exec, approval, bash, RPC, compaction, kill. No secrets. | `src/core/audit.hpp` | P0–P1 |
| **`/doctor` (or `/config`)** | Show effective config sources + mask keys; workspace root canonical; build flags. | `main.cpp` command | P2 (L1) |
| **Deterministic Replay (metadata-only)** | Record request/response metadata + policy results + provider id; never secrets/sensitive full content. | `session.hpp` + agent events | Post-v1 |
| **CI + Sanitizers + Fuzz** | ASan/UBSan/TSan, ctest, fuzz harnesses (jsonl, terminal, paths). | `.github/workflows/ci.yml` | P0 (M2) |


## 5. Validation Gates (Must Pass Before “Production-Ready”)

| Gate | Requirement | Status (observed) |
|---|---|---|
| Unit | Path normalization (symlink/..), JSONL parse/recovery, approval binding/consume, cancellation, provider errors | **Partial** (4 tests; missing traversal/corruption/approval/RPC) |
| Fuzz | json, JSONL, terminal escapes, paste multiline, tool args, path handling | **None** |
| Integration | Timeouts, partial streams, malformed tool calls, process hang, disk-full, interrupted append, fork/compact, provider switch | **Partial** (offline loop tests) |
| Adversarial | Direct/indirect prompt injection, secret exfiltration, workspace escape, dangerous cmd construction, approval replay, poisoned skills/AGENTS | **None** (not present) |
| Safety/Hardening | ASan/UBSan in CI; strict warnings; dependency license/SBOM; reproducible TLS build; security response process | **None** (no CI) |
| RPC Safety | Token/UDS, read-only default, rate limits, audit, headless policy | **Missing** |
| Durability | fsync + atomic append, schema_version, non-silent recovery | **Missing** |

## 6. Sequenced Plan (Team Recommendation)

### Week 1 — Must-fix first pass (P0)
- [ ] **Day 1–2 (Security lead):** Threat model + repros for S1–S5 against live build; write failing tests. 
- [ ] **Day 2–3 (Systems):** Executor prototype: `execve` argv for grep/find, `bash` gated (timeout+setpgid+killpg+rlimits+env-scrub). Path canonicalization (`canonical`/`weakly_canonical`) + workspace check. 
- [ ] **Day 3–4 (Policy/Safety):** Call-bound approval spec + atomic consume; remove UI-only policy; enforce in executor. RPC auth (token or UDS) + `rpc-tools` minimal + 401. 
- [ ] **Day 4–5 (Reliability):** JSONL: `schema_version`, non-silent recovery (quarantine corrupt), fsync+O_APPEND+0600 perms. 
- [ ] **Ongoing (DevOps):** Add CI with ASan/UBSan, build TLS/plain, ctest. 
**Week 1 exit criterion:** No new symlink/injection exploits; RPC requires auth or dangerous tools disabled; ASan/UBSan clean on tests.

### Week 2 — Concurrency, tests, polish (P1/P2)
- [ ] Signal handling (SIGWINCH resize), worker join/cleanup on all exit paths (approval-stdin-EOF case), shutdown order. 
- [ ] Expand unit tests (traversal, JSONL recovery, approval, cancellation). Add basic fuzz harnesses (jsonl+paths). 
- [ ] `/doctor` command (effective config sources, masked keys, workspace root canonical, build). 
- [ ] Audit log implementation (append-only, no secrets). 
- [ ] HTTP timeouts/retry for client. 
**Week 2 exit criterion:** All existing ctest still pass; new hardening tests pass; sanitizer clean.

### Must-Fix vs Post-v1 Split

| Category | Items | Notes |
|---|---|---|
| **Must fix before release** | S1–S5, M1 (durability/recovery minimum: fsync+non-silent+schema_version+perms), M2 (CI+ASan/UBSan minimal) | Execution layer is non-negotiable. Session integrity required for auditability. |
| **Post-v1** | seccomp-bpf, TSan, full fuzz corpus, SBOM, deterministic replay, approval cache, mTLS, bounded queue, compaction manifest, TOCTOU refinements | Valuable but can follow if week1–2 complete with tracked issues. |

## 7. Answers to “Which area should the team audit first?” 

**Team answer: `bash` tool sandbox (execution layer) — P0.**

Rationale (code-based):
1. `popen("sh -c", ...)` with concatenated strings (`tools.hpp:111,150,161`) + no timeout/rlimits/killpg/env-scrub.
2. `path_in_workspace` uses `lexically_normal()` only (no `realpath`/`canonical`) → symlink escape (`tools.hpp:31`).
3. Approval is interactive-only and unbound (`main.cpp:195`); RPC has no approval/auth (`main.cpp:453`) — execution-layer controls missing.
4. Fixes localize to `tools.hpp` + policy/executor + RPC handler, high leverage, block release.

**Then:** session-tree durability (P1), runtime/TUI concurrency (P2). **In parallel:** add CI with ASan/UBSan (cheapest coverage for all three).

---
