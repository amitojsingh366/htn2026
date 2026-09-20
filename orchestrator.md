# Zombie Tag implementation orchestrator

You are the sole integration and hardware coordinator for the firmware described in `plan.md`. Read the entire plan before creating packets. Your job is to create concrete work packets, run Codex CLI workers in isolated Git worktrees, review and merge their changes, and deliver the working firmware and reusable recovery tool. This file is an execution handoff, not evidence that execution has happened.

## 1. Non-negotiable user decisions

- ESP-IDF v5.5.3 custom firmware for the HTN 2026 ESP32-C3 badge; core demo is button tagging, local radar, ESP-NOW mesh, mobile Wi-Fi gateway, offline event replay, and quirky server announcements.
- Three badges available initially; maximum **20 registered players per round**. Ten minutes, server-selected patient zero, registered players may rejoin. Host is a normal player who moves with a 2.4 GHz hotspot.
- No core healing, code station, sonar, NFC, positioning, AI gameplay changes, backend implementation, or dashboard implementation. Backend teammate receives the exact contract.
- **Transport amendment, 2026-09-19 (user-directed, supersedes the original "firmware does not require WebSockets").** The live gateway transport is one authenticated **WSS** connection owned by the host badge, using `espressif/esp_websocket_client` pinned to **1.8.0**, verified to build against ESP-IDF v5.5.3. HTTPS is retained only for bootstrap and registration; the polling sync endpoint is removed and **no HTTP polling fallback is built**. Event identities, durable queues, dedupe, command cursors, causal dependencies, and the receipt/decision/application distinction are unchanged. The exact schema is `plan.md` §5 and `docs/backend-contract.md`.
- No automated tests, CI, coverage work, simulator, or test work packets. Compile/link/size verification and recovery integrity checks are required. Observe the actual requested demo; do not claim unperformed checks.
- Every badge gets a complete private 4 MiB backup, two independent equal reads, verified copies on separate storage, a recovery ID, and an owner backup file. Rehearse restoration once on the first badge before loading any custom firmware. Never overwrite the immutable original archive.
- Workers each get their own worktree and use **Codex CLI `gpt-6-astra`**, reasoning **high**. At most **three concurrent workers**. Do not substitute another model if access fails.
- **Only you merge.** Only you stage/commit worker changes, modify shared contracts after freeze, integrate `app_main`, resolve conflicts, access badges, run recovery/flash/provisioning, or handle private credentials and backup archives. Workers write allowlisted code and return uncommitted changes.
- No eFuse writes, secure boot/encryption/anti-rollback activation, flash-status/OTP writes, default `idf.py flash`, full erase, or flashing another person's image. The guarded operator tool is the only deployment path.

Treat `plan.md` as the current product specification. The older Downloads handoff is background and must not reintroduce excluded features. If a technical assumption proves false, record the evidence, amend the shared contract once, and redispatch affected work. Do not silently change protocol/layout/scope to make a worker finish.

## 2. Execution scope and communication

Proceed with authorized code, documentation, builds, reviews, and reversible setup. These documents were created during a planning-only task; the implementation agent receiving this handoff should execute the requested implementation, not recreate this planning conversation. Physical operations follow the user's instruction for that execution session and each badge owner's agreement. A request to implement and demonstrate authorizes the described guarded deployment; merely reading these files does not authorize touching an attached device.

Complete the app and tool artifacts before seeking any outstanding physical-access/input decisions. A failed backup gate blocks writes to that badge, not independent code work. Never ask for permission to do already-authorized read-only work, ordinary source fixes, or another integration build. If a real permission boundary blocks a necessary operation, explain the exact action and actual reason; do not invent a generic approval checklist.

Keep the user updated approximately once per minute during active work, describing meaningful progress, decisions, or blockers. Keep a compact ledger so the task can resume after context compaction. Report status truthfully: `built`, `backup verified`, `restored and verified`, `flashed`, `commissioned`, and `demo observed` are distinct outcomes.

Do not send the contract to the teammate through chat/email tools without explicit instruction. Deliver the local document for the user to share.

## 3. Preflight, tools, and source repository

Planning-time observations, recheck before execution:

| Item | Observed value / action |
|---|---|
| Workspace | `/Users/amitojsingh/Desktop/misc/hackerbadge` |
| Repository | No `.git` existed during planning; preserve all existing `probe/` evidence |
| Codex | `/Users/amitojsingh/.local/bin/codex`, CLI 0.153.4; ChatGPT login active at planning time |
| Model | Exact `gpt-6-astra` present in local model catalog; successful new generation still depends on account availability |
| Working Git | `/Library/Developer/CommandLineTools/usr/bin/git`, version 2.50.1 |
| System Git | `/usr/bin/git` failed on an Xcode license prompt. Use the working Git executable; do not accept licenses or change system Xcode settings for convenience. |
| Existing recovery dependency | `.probe-venv` has esptool 5.4.0; create/pin the implementation tool environment deliberately, do not mutate historical probe tooling casually |
| Hardware evidence | `probe/REPORT.md`, later save/unlock records, official badge HAL; no complete restoration archive exists merely because `factory-firmware.bin` exists |

1. Read the latest user steering, `plan.md`, this file, and applicable `AGENTS.md` files. Inspect current source/Git state; never assume planning-time absence means today's repository is empty.
2. Recheck `codex --version`, `codex exec --help`, `codex exec resume --help`, working Git, and ESP-IDF activation. Read only specific model metadata if needed; never dump configuration/auth files.
3. Locate/install ESP-IDF v5.5.3 and required toolchains once under orchestrator control. Respect filesystem/network permissions. Workers use the same read-only SDK/dependency installation and worktree-local build output. Do not race package installs or component lockfile changes.
4. Before Git initialization or staging, create exclusions for real secrets/backups and all `probe/` data. Preserve existing ignore entries. Suggested exclusions: `probe/`, `.probe-venv/`, `.venv*/`, `.worktrees/`, `.orchestration/`, `**/build/`, `**/managed_components/`, `.env*`, `*.bin`, `*.elf`, `*.map`, `*.log`, `provisioning/`, `backups/`, `private/`. Real archives must be outside the repo even when ignored.
5. If still not a repository, initialize a source-only repository with the working Git. Explicitly stage `plan.md`, `orchestrator.md`, `.gitignore`, and selected sanitized source/docs as they appear. Never `git add .` over this workspace. Use the configured author; do not invent Git identity if one is missing. Record a foundation commit before creating worktrees.
6. Store state under ignored `.orchestration/`: `state.json`, `decisions.md`, `runs/<packet-id>/<attempt>/`, `releases/`. Source packets may be committed under `packets/`; secrets, serial captures, and raw device data may not.
7. Do not create public repositories, push, publish, purchase services, deploy the backend, or launch another user-visible task as part of worker orchestration.

## 4. Freeze contracts before parallel firmware work

First write packet **F00: foundation and contracts** and have one worker implement only those owned files. You review and merge it before dependent workers start. A recovery-tool packet can run independently alongside it because its disk/CLI contract is already specified in plan §11. R01 owns docs/recovery.md; F00 owns docs/operations.md. Do not give both packets the same documentation file.

F00 must produce:

- `firmware/CMakeLists.txt`, component CMake scaffolding, `sdkconfig.defaults`, exact stock `partitions.csv`, dependency manifest/lock, and a minimal nonflashing build entry point.
- Shared `zt_contract/include/zt_*.h`: enums, constants, versioned state/event/request types, ownership and thread rules, buffer limits, public module APIs. No worker invents cross-module structs afterward.
- `docs/protocol.md` extracted from plan §12, including exact byte offsets/sizes, all packet/command variants, ranges, hashes, HMAC coverage, and result enums.
- `docs/backend-contract.md` extracted from plan §5 and the added ingestion/clock/receipt details in §12: the retained HTTPS bootstrap/registration endpoints, the WSS upgrade endpoint and authentication, every client and server message schema, the connection/resume handshake, the acknowledgment model, framing and 4 KiB bounds, heartbeat/timeout/backoff values, close codes and errors, round-specific replay, and pagination.
- `docs/operations.md` skeleton covering private inputs, build, enrollment/restore, and known limitations.
- A short sanitized hardware header/document. Do not add the full probe binaries/logs or handoffs containing irrelevant past secrets to Git.

F00 must compile a minimal app using the pinned toolchain without accessing serial ports. Stub implementations are acceptable only in this explicitly marked scaffold; track every stub in the ledger and remove/replace them before release. No placeholder may be mistaken for a completed worker module. Put scaffolding stubs in the eventual owning module’s implementation source files and replace them in place, rather than retaining a separate duplicate-symbol stub library. Public component APIs stay fixed. Component-local CMakeLists.txt is owned by that component’s worker after F00; root CMake, shared headers, dependency locks, and sdkconfig remain orchestrator-owned.

You own the freeze commit. Record the exact SHA, wire/API version, file ownership, and any deliberate amendment to plan. Shared public headers, protocol/HTTP schemas, sdkconfig, root CMake, and `main/app_main.c` become read-only to ordinary workers after this commit. Contract changes return through `BLOCKED_CONTRACT`; you approve and integrate a single amendment, then restart or resume dependents on that base.

## 5. Work-packet format

Generate a separate `packets/<ID>.md` for each bounded assignment. Every packet contains:

```text
Packet ID / goal
Base commit SHA
Dependencies already merged
Branch and absolute worktree path
User decisions and exact plan sections that apply
Owned file allowlist (paths, not vague module names)
Read-only shared headers/schemas and expected exports
Caller/task ownership and asynchronous completion rules
Exact buffer/queue/storage/rate limits and overflow behavior
Required behavior, including errors/reboots/duplicate messages
Allowed build or syntax-check command
Explicitly excluded features
No hardware / no credentials / no backup access / no Git mutation
Concrete review criteria (no test suite)
Final-report format
```

Require the worker to read its actual base contracts. It must return `BLOCKED_CONTRACT` with a precise proposed change if something cannot be implemented correctly. It must not silently reinterpret the wire format, change thresholds, create a new endpoint, increase a queue without accounting for memory, or reach into another worker's ownership.

Worker final report:

```text
Status: READY_FOR_REVIEW | BLOCKED_CONTRACT | BLOCKED_ENVIRONMENT
Changed files:
Behavior implemented:
Build/syntax command and result:
Memory/size implications:
Known gaps and unfinished work:
Contract amendments requested:
Integration notes:
Hardware operations: NONE
```

Do not impose line-count or speed targets. A packet that cannot meet its contract is a real issue to resolve, not permission to fabricate completion.

## 6. Packet dependency graph and ownership

You write the actual packets from the current frozen headers. The following is the required work breakdown, not permission to dispatch all of it simultaneously.

| ID | Goal and owned implementation files | Depends on |
|---|---|---|
| F00 | Foundation/shared contracts/docs/scaffold as above | Source-only repo |
| R01 | `tools/badge_tool.py` recovery/archive/release gate modules, pinned Python dependencies, `docs/recovery.md` (separate from F00’s operations skeleton) | Plan §11; may run alongside F00 |
| H01 | `components/zt_hal/` and `components/zt_ui/`: buttons, LCD stripes, capped LEDs, radar/list/status UI, no sensor/NFC init | F00 |
| M01 | `components/zt_radio/`: actual serializers/parsers, HMAC, RX/TX owner, discovery, forwarding, bounded dedupe/cache/anti-entropy | F00 |
| G01 | `components/zt_game/` and `components/zt_store/`: admission/round/role state, direct proximity, pending persistence, event journal, timing/reboot/reconciliation | F00 |
| N01 | `components/zt_gateway/`: mobile STA supervisor, controlled same-channel recovery, HTTPS bootstrap/registration client, WSS connection lifecycle with resume handshake and bounded reassembly, exact bounded JSON, command/custody handling | M01 + G01 merged |
| C01 | `components/zt_console/` and tool provisioning/game-export additions: USB JSON framing, WAIT_INSTALL, configure/status, diagnostic input/link controls | R01 + H01 + G01 merged |
| I01 | Integration fixes in explicitly assigned implementation files, release manifest generation, backend contract consistency | All required modules merged |

Within three slots, prioritize R01 early; after F00, H01/M01/G01 are parallel only when a slot is free. N01 must not guess at unmerged network/game contracts. C01 and R01 cannot concurrently edit the same tool files. You may split a large packet into smaller sequential packets with precise allowlists; preserve the dependency graph and three-worker maximum.

You alone integrate `app_main.c`, task creation, component wiring, final sdkconfig, and root CMake after each merge. Keep these integration edits small. Substantive missing module functionality goes back to an appropriate worker packet.

No backend worker. The backend teammate owns the service. Give them the contract and record any agreed amendment before changing firmware. Endpoint unavailability is not a reason to build an unsolicited backend, to quietly downgrade the mobile-host requirement to USB, or to reintroduce an HTTP polling fallback that the user excluded.

## 7. Launch Codex CLI workers

Use a validated absolute repository path, base SHA, controlled packet ID, and explicit branch/worktree names. Paths below are templates, not commands already executed. Keep worker source under the ignored workspace-local `.worktrees/` so ordinary source edits fit the workspace permission boundary.

```sh
badge_git=/Library/Developer/CommandLineTools/usr/bin/git
"$badge_git" -C "$repo_root" worktree add -b "$worker_branch" "$worker_tree" "$base_sha"
```

Workers do not need shared `.git` write permission because they leave their changes uncommitted. Do not pass broad `--add-dir` permissions for the repository, home directory, SDK, or archive.

Verified launch pattern for CLI 0.153.4:

```sh
/Users/amitojsingh/.local/bin/codex -a never exec \
  --model gpt-6-astra \
  -c 'model_reasoning_effort="high"' \
  --sandbox workspace-write \
  --cd "$worker_tree" \
  --json \
  --output-last-message "$run_dir/result.md" \
  - < "$packet_file" > "$run_dir/events.jsonl" 2> "$run_dir/stderr.log"
```

Prefer an orchestrator-owned Python runner using `subprocess.Popen(argv, cwd=worker_tree, stdin=packet_file, stdout=event_file, stderr=stderr_file)` over constructing shell strings. Use argv arrays; packet contents go through stdin. Never interpolate packet text, names, or secrets into a shell command. Redirect logs using the parent process; do not ask workers to write outside their worktrees.

`-a never` means workers cannot pause for escalated approval; the workspace-write sandbox still applies. They report a denied prerequisite to you. It is **not** permission to bypass sandboxing. Do not use `--dangerously-bypass-approvals-and-sandbox`, `--sandbox danger-full-access`, `--ignore-rules`, or `--skip-git-repo-check`. Do not switch to another model or raw API calls to evade a failure.

Capture `thread.started` → `thread_id` immediately from JSONL. Record packet ID, attempt, session UUID, PID, base SHA, argv, paths, start time, exit code, and `turn.completed`/`turn.failed` events. Keep stdout JSON separate from stderr. Do not mark a packet done because a process exists, output stopped, or a final sentence says it is complete.

Resume only the recorded session UUID in the same worktree; never `--last` when multiple workers exist:

```sh
/Users/amitojsingh/.local/bin/codex -a never exec \
  --sandbox workspace-write \
  --cd "$worker_tree" \
  resume \
  --model gpt-6-astra \
  -c 'model_reasoning_effort="high"' \
  --json \
  --output-last-message "$retry_dir/result.md" \
  "$session_id" - < "$followup_file" > "$retry_dir/events.jsonl" 2> "$retry_dir/stderr.log"
```

Do not run two turns concurrently in one worker/worktree. Recheck local help if the CLI changes. Keep persistent sessions for bounded corrections. At most two automatic retries for a clearly transient failure, then diagnose. Model/auth/rate-limit errors pause new launches, preserve all work, and get reported faithfully; no model substitution.

Workers have no task to read `.codex` credentials, real provisioning configs, full flash images, device backups, stock wallet files, or serial ports. Worktrees isolate edits, not all filesystem reads. Keep the archive outside their inputs; use real OS read restrictions/unmounted encrypted storage when strict read separation is required. Do not claim the worktree path alone enforces secrecy.

## 8. Review and sole-orchestrator integration

Review queue is serial even while independent workers run.

1. Wait for the worker process to exit. Preserve final report, JSONL, stderr, and worktree.
2. Inspect the diff against the exact allowlist and base SHA. Include untracked source files in review; do not overlook them because `git diff` omits untracked content.
3. Check contract fidelity: byte layouts, enum values, HTTP lengths, event identity/frontiers, single game-state owner, PERSISTING timeout behavior, same-channel reconnection, bounded allocations, and no forbidden NVS/eFuse/flash operations. Check for disabled TLS verification and secret logging.
4. Run the packet's build or syntax check. Firmware packets compile on their frozen scaffold; recovery tools may use `--help`/syntax checks without opening a device. Do not add test suites or mock servers.
5. If incomplete, resume with a bounded defect list. If contracts need revision, stop affected dependent packets, integrate the amended shared contract, and provide a new baseline. Do not let workers edit one another's worktrees.
6. You explicitly stage only reviewed source paths in the worker worktree and commit them. Workers do not commit/merge/push/rebase/reset. Preserve unrelated user changes.
7. Merge the reviewed branch into your integration branch with a reviewable pending merge:

```sh
"$badge_git" -C "$repo_root" merge --no-ff --no-commit "$worker_branch"
```

8. Resolve conflicts yourself, inspect the resulting staged change, wire shared entry points if needed, and build the combined firmware. Only then create the merge commit. A module compiling independently is not proof that the combined firmware links or fits.
9. Record the merge SHA and release-impacting decisions. Launch dependent packets from this new integration SHA, never their stale foundation SHA.

Do not repeatedly rebuild unchanged completed work without a new change/failure/concern. Do not delete failed worktrees or force-reset a worker to hide a defect. Remove worktrees only after verified integration and preserving needed logs, through your own normal cleanup.

Ledger states: `planned`, `ready`, `running`, `needs_review`, `blocked_contract`, `blocked_environment`, `merged`. Keep exact remaining work. No packet may be marked merged without its commit in the integration history.

## 9. Release, recovery gates, and physical rollout

Before the first serial write, all of the following must exist:

- A built application within the stock `0x2A0000` factory limit and a full-partition padded release image with hashes.
- Release manifest binding source SHA, ESP-IDF/tool versions, app hashes, write range, protocol/API/schema versions, and reviewed security/storage config.
- Implemented guarded operator tool with per-device archive/receipt/restore paths and no unguarded arbitrary-offset/force-erase options.
- Private organizer archive and a second durable storage medium; absence of the second copy blocks flashing.
- Backend HTTPS base URL and WSS upgrade path, game/host credentials, and session configuration, supplied privately when networking is to be commissioned.
- First badge owner/device available with the physical Start/USB procedure understood.

Only you run `badge_tool` hardware commands. Its commands must enforce the gates themselves; prose and an old `BACKUP_VERIFIED` label are not sufficient. Every write binds the selected full MAC, current loader connection, fresh dual-read snapshot, both verified copies, target image, supported layout/security state, and exact write range.

Execution sequence:

1. `archive-init`; choose explicit private roots and verify the second medium.
2. `enroll` first badge; acquire two full matching reads and publish both copies/receipt.
3. `rehearse-restore`; write the original full image, read all bytes before reboot, compare exactly, then confirm stock boot. This is a required first-badge operation, not a test-suite task.
4. `release-manifest`; bind built artifacts to source/configuration and the app-only range.
5. `flash-game`; automatically capture fresh state after stock rehearsal boot, verify both copies, write only `0x10000..0x2affff`, compare complete expected 4 MiB readback before boot.
6. Boot to WAIT_INSTALL; `install-init --operation VERIFIED_FLASH_OPERATION_UUID` through the tool (console op install_init), then private `provision`. Verify configuration hashes without exposing values.
7. `commission`; after player and host radio initialization, compare all protected stock regions against the immediate preflash snapshot and readable security state against capture. A change stops rollout.
8. Repeat independent enrollment/flash/commissioning for the second and third badges. Do not reuse the first badge's archive or identity as a shortcut.
9. Observe core gameplay/mesh/mobile-host/offline replay with the backend teammate's service. Record actual results and shortcomings.
10. For each guest, repeat its own complete archive and commissioning, issue the code and verified local owner bundle. Any number of archival participants is supported; only 20 can be in a round.

No writes to bootloader/table/stock NVS/LittleFS during custom deployment. The final 64 KiB must be verified blank on first installation. Its first 4 KiB becomes the raw installation-header sector and the remaining 60 KiB becomes runtime-registered zt_nvs; validate the header before NVS initialization on later boots. No format-on-error path. Restoration writes the selected whole image at zero with header parameters `keep`, followed by complete preboot readback equality and ordinary stock-boot observation. A corrupt current application/table may be READ_VERIFIED and restored from a valid original; do not require current bootability for restoration. Owner bundles have a guarded import-bundle path to reconstruct the archive/index and verified second copy.

If a gate fails, keep device in its known state/loader, retain evidence privately, report the failed condition, and stop that operation. Do not fall back to raw `idf.py flash`, `erase-flash`, another badge's image, or a downloaded stock image. A deliberate guarded restore remains available when its own prerequisites pass.

The ordinary tool supports arbitrary participant count but only the explicitly supported hardware adapter. Unsupported readable devices may be archived; they are not silently flashed using guessed offsets.

## 10. Final delivery and honest completion

Deliver links to the firmware source, `tools/badge_tool.py`, pinned dependency instructions, `docs/backend-contract.md`, `docs/protocol.md`, `docs/operations.md`, and release manifest. Report source commit/build hash, what was actually observed on three badges, whether the restore rehearsal passed, and any remaining backend/hardware limitation.

Do not attach private flash images, eFuse dumps, host tokens, Wi-Fi passwords, or participant archive catalogs to the task response. Provide sanitized per-device statuses/recovery IDs only as needed; owner bundles are handed over locally through the tool.

Completion means required code is implemented and integrated, build/recovery gates passed where executed, and requested physical behavior actually observed. A backend/device input blocker may prevent the final demo, but does not excuse unfinished independent source work. Distinguish that blocker from a successful demo. Do not mark stretch features as missing core work.

## 11. References for the orchestrator

CLI syntax was verified against installed 0.153.4 help during planning. Current official sources fetched:

- [Non-interactive Codex](https://learn.chatgpt.com/docs/non-interactive-mode): stdin prompts, JSONL events, final output, and explicit session resume.
- [Developer CLI command reference](https://learn.chatgpt.com/docs/developer-commands?surface=cli): flags and sandboxing; recheck local help for the installed version.
- [Codex sample configuration](https://learn.chatgpt.com/docs/config-file/config-sample): reasoning configuration.
- [GPT-6 Astra](https://developers.openai.com/api/docs/models/gpt-6-astra): exact requested model name. Local catalog presence is not a guarantee of future account access.

Use relevant skills when implementation actually requires them. OpenAI Docs applies to Codex CLI/model questions. Cloudflare/backend skills do not expand this firmware task into building the teammate's service.
