# Packet O01 — interactive operator CLI for many badges, addressed by memorable name

## Goal

Implement `tools/badge_ops.py`: a single-file, interactive convenience front end over the
existing guarded recovery tool, so an operator can enrol, flash, restore and inspect **any
number of badges** by a memorable nickname (`alex`, `badge-1`, `red`) instead of copying a
`ZT-XXXX-XXXX-XXXX-XXXX-XX` recovery ID by hand.

You are writing an **address book and a menu**, not a flashing tool. Every operation that
touches a badge is performed by running the existing `tools/badge_tool.py` as a
subprocess. You implement no gate, no capture, no write, no esptool call, and no retry of
a refused operation.

## Base commit and dependencies

- Base commit: `1adb1ece7c955e7ca651ffe0bb098907a234bba3` on `firmware/integration`.
- R01 is merged: `tools/badge_tool.py` and `tools/zt_badge/` exist and are final for you.
- The badge-1 flash/restore loop was demonstrated on real hardware at this base.

## Branch and worktree

- Branch: `firmware/O01-operator-cli`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/O01`

## Owned file allowlist

```
tools/badge_ops.py
docs/operator-cli.md
```

Nothing else. In particular:

**Do not create or modify any file under `tools/zt_badge/`, `tools/badge_tool.py`, or
`tools/requirements.txt`.** This is not style. `zt_badge.revision()` hashes exactly
`badge_tool.py`, `requirements.txt` and `zt_badge/*.py`, and `gates.rehearsal()` only
accepts a restore rehearsal recorded under the *current* revision. Adding one file to
`zt_badge/` invalidates a rehearsal that was performed on real hardware and re-locks
`flash-game` until the operator repeats it on a physical badge. `tools/badge_ops.py` is
deliberately outside that hashed set. Keep it a single file with no new package.

If you think you need a second module, that is a `BLOCKED_CONTRACT`, not permission.

## Read before writing code

In your worktree: `tools/badge_tool.py`, `tools/zt_badge/cli_recovery.py` (especially
`confirm()` and the command registration at the bottom), `tools/zt_badge/index.py`,
`tools/zt_badge/config.py` (`local_dir()`, `load()`), `tools/zt_badge/errors.py`, and
`docs/recovery.md`. `plan.md` §11 is the ownership boundary.

## Required behavior

### Invocation

- `python3 tools/badge_ops.py` with no arguments enters the **interactive menu**.
- `python3 tools/badge_ops.py <command> [...]` runs one command directly.
- Python 3.12, standard library only. No new dependency, no network, no `pip`.
- It may `import zt_badge.config` and `import zt_badge.index` **for read-only lookup**
  (importing performs no I/O and does not affect `revision()`). It must not import or call
  `device`, `archive`, `gates`, `release`, or `bundle`.

### The name registry

- Stored at `zt_badge.config.local_dir() / "badge-names.json"`, created with mode `0600`,
  written atomically (temp file in the same directory, `fsync`, `os.replace`).
- Content: `{"schema": 1, "badges": {"<nickname>": {"recovery_id": "ZT-...",
  "added_utc": "..."}}}`. Nothing else — no MAC, no port, no image path, no token.
- It lives beside `recovery-config.json` in the organizer's private directory, never in
  the repository, a worktree, or a synced folder. **Never modify `recovery-config.json`.**
- Nickname rules: 1–24 characters matching `[a-z0-9][a-z0-9-]*`, case-folded on input,
  unique, and not a reserved word (`list`, `help`, `quit`, `all`). Reject anything else
  with a clear message. One nickname maps to exactly one recovery ID and vice versa.
- A registry entry is a **label, never an authorization**. Before any command that touches
  a badge, re-resolve the nickname and require that its recovery ID appears in
  `zt_badge.index.scan(config.load()["archive"])`, which only returns archives with a
  signed `BACKUP_VERIFIED` receipt. A nickname naming an unknown or unverified archive is
  refused, and the message says to run `enrol` — it is never silently created.

### Commands

| Command | Behavior |
|---|---|
| `list` | Every registered nickname, its **masked** recovery ID (`ZT-DPA0-…-07`), whether the archive is currently visible, and the last recorded outcome from `badge_tool.py status`. Also lists verified archives that have **no** nickname yet, so nothing is stranded. |
| `enrol <nick>` | Capture `index.scan()` first, run `badge_tool.py enroll --port P --owner-label L`, re-scan, and bind the single newly appearing recovery ID to the nickname. If zero or more than one appeared, bind nothing and say so. (`enroll` deliberately does not print the ID.) |
| `flash <nick> --release PATH` | `badge_tool.py flash-game --recovery-id … --port P --release PATH` |
| `restore <nick> [--snapshot original]` | `badge_tool.py restore --recovery-id … --snapshot S --port P` |
| `rehearse <nick>` | `badge_tool.py rehearse-restore …` |
| `verify <nick> [--snapshot original]` | `badge_tool.py verify …` — the correct way to finish an `AWAITING_SECOND_COPY` capture, with no hardware |
| `status <nick>` | `badge_tool.py status --recovery-id …` |
| `export <nick> --to DIR` | `badge_tool.py export …` |
| `commission <nick> --release PATH` | `badge_tool.py commission …` |
| `ports` | `badge_tool.py ports`, presented as observations |
| `rename <old> <new>`, `forget <nick>` | Registry only. `forget` removes the label and states plainly that the archive, its images and its receipts are untouched and the badge is still recoverable by recovery ID. It never deletes archive data. |

Accept `enroll` as an alias of `enrol`.

### Running the guarded tool

- Build an **argv list**; never a shell string, never `shell=True`. The interpreter is
  `sys.executable`; the script path is resolved from `__file__` (`…/tools/badge_tool.py`).
- **Inherit stdin, stdout and stderr.** Do not capture them, do not pipe them, do not pass
  `input=`. `cli_recovery.confirm()` reads `/dev/tty`, or falls back to an interactive
  stdin, and fails closed with `LOCAL_OPERATOR_OBSERVATION_REQUIRED` otherwise. The
  operator must see every prompt and type every confirmation themselves.
- **Never type, echo, default, pre-fill, or offer to send `AGREED`, `REVIEWED`,
  `STOCK_BOOT_CONFIRMED`, `REHEARSAL_ACCEPTED`, `INDEPENDENT`, `INITIALIZED`, `player` or
  `host`.** Answering an observation prompt on the operator's behalf defeats the only
  mechanism that binds a recorded outcome to something a human actually saw. There is no
  `--yes`, `--force`, `--batch`, `--non-interactive`, or `--assume` flag anywhere in this
  program.
- Surface the child's exit status. On a non-zero exit, print the tool's own
  `STOPPED: <CONDITION>` line as it appeared and point at the private invocation record;
  never re-run it automatically, never "fix" it by trying a different command, and never
  interpret a refusal as a transient error.
- One operation at a time. No concurrency, no threads, no background work.

### Port selection

Run `badge_tool.py ports` and present the candidates as **observations**, with the
reminder that a port name never identifies a badge — identity is the archived full MAC,
which the guarded tool re-checks on every connection. Accept an explicit `--port`. If
exactly one candidate exists, you may pre-select it, but still display it and let the
operator override. Never remember a port in the registry.

### The interactive menu

A plain numbered loop on stdin, no curses, no colour codes, no clearing the screen, no
third-party library. It shows the registered badges, offers the commands above, asks for
what it needs (nickname, port, release path), prints the exact `badge_tool.py` argv it is
about to run, and runs it with inherited stdio so the tool's own prompts appear. `q` or
EOF quits. An invalid choice re-prompts. Ctrl-C during a child process must not leave the
registry half-written.

### Before a write

Print a short, honest summary — nickname, masked recovery ID, the operation, and for
`flash` the release path — then require the operator to type the **nickname** to proceed.
This is a typo guard for choosing the wrong badge; it is **not** a substitute for, and
never replaces, the guarded tool's own confirmations, which still follow.

### Refusals

Recovery IDs are private lookup references. Print them masked by default; a `--show-id`
flag on `list` and `status` may print one in full. Never print or log a MAC, a token, an
SSID, a password, flash contents, or an eFuse value. Never open a serial port, never
import `esptool` or `serial`, never read `flash.bin`, never write anywhere inside the
archive or mirror, and never create, move or delete an archive directory.

## Documentation

`docs/operator-cli.md`: what the program is, what it is not, the registry file and its
location, every command, the nickname rules, an example enrol-then-flash-then-restore
session, and an explicit statement that it adds no capability and removes no gate.

## Excluded

No GUI, TUI, curses, colour, progress bar, or animation. No config wizard. No editing of
`recovery-config.json`. No parallel flashing. No inventory of participants' names or
contact details. No backup scheduling. No network calls of any kind. No tests, no test
harness, no mock `badge_tool.py`, no fake device, no CI. No changes under
`tools/zt_badge/` or to `badge_tool.py`.

## Allowed checks

`python3 -m py_compile tools/badge_ops.py` and `python3 tools/badge_ops.py --help`.
`python3 tools/badge_ops.py list` is acceptable **only** if it exits cleanly when no
organizer configuration exists; it must not create one.

**No hardware. No serial ports. No credentials. No archive access. No real recovery IDs
in code, comments, documentation or examples — use `ZT-AAAA-BBBB-CCCC-DDDD-99`. No Git
commit, push, rebase or reset: leave your work uncommitted in your worktree.**

## Review criteria

1. Only the two allowlisted files exist or changed; nothing under `tools/zt_badge/`.
2. No `shell=True`, no shell string, no captured stdio for the child, no `input=`.
3. No path that can answer a confirmation prompt, and no force/yes/batch flag anywhere.
4. The registry is outside the repository, `0600`, atomically written, and holds no MAC,
   port, credential or image path.
5. A nickname is re-resolved and re-verified against `index.scan()` before every badge
   operation; an unverified archive is refused.
6. Recovery IDs masked by default; no secret is ever printed.
7. A non-zero child exit is reported faithfully and never retried automatically.
8. `py_compile` passes and `--help` works with no archive present.

## Final report

Use the standard format: Status, Changed files, Behavior implemented, Build/syntax command
and result, Memory/size implications (write `n/a, host tool`), Known gaps, Contract
amendments requested, Integration notes, Hardware operations: NONE.
