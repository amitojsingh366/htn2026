# Packet R03 — fast custom-to-custom reflash

## Goal

Let an operator iterate on firmware without paying for a fresh dual full-flash capture on
every write, without weakening a single guarantee that matters.

## The operator's actual problem, in their words

"if i want to flash from custom rom to custom rom i dont want to delete the real badge rom
with saved data to be deleted but also dont want to backup our custom rom cuz we will keep
iterating on it (and i dont want to overwrite the actual htn badge rom that needs to be
backed up)"

Two of those three worries are already satisfied and must stay that way: `original/` is
immutable and is never replaced, and `flash-game` only ever writes
`0x10000..0x2affff`, so the stock bootloader, partition table, stock NVS, PHY data and
LittleFS are never written. The real cost is that `flash-game` takes a fresh dual
capture before **every** write. On the first flash that is essential, because it
preserves whatever the owner did since enrolment. On the tenth custom-to-custom
iteration it is capturing a copy of our own firmware, which preserves nothing and is the
slowest part of the loop.

The operator chose: **skip the capture, keep the full readback.**

## Base commit and dependencies

- Base commit: `c8d15616328003e71a805ca374266648083af878` on `firmware/integration`.
- Branch: `firmware/R03-fast-reflash`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/R03`
- You wrote this tool (packet R01). C01 added `cli_console.py` and R02 changed the
  confirmations to y/n; both are merged.

## Owned file allowlist

```
tools/zt_badge/device.py
tools/zt_badge/gates.py
tools/zt_badge/cli_recovery.py
docs/recovery.md
```

No other file. No firmware. Do not touch `badge_ops.py` — packet O01 owns it.

## Required behavior

Add a fast path **inside `flash-game`**, chosen automatically, never a new command and
never a flag the operator has to remember.

### When the fast path may be taken

All of these must hold. Any failure falls back to the ordinary full-capture path
silently and correctly — never refuses the flash, never asks, never retries.

1. The connected badge's full factory MAC, chip package/revision, JEDEC ID, capacity and
   readable security state match the archive, exactly as today. **This check is not
   relaxed in any way.**
2. A verified `original/` snapshot exists and passes the existing backup gate on both
   media. The fast path depends on `original/` for the bytes it does not rewrite, so an
   unverified original means no fast path.
3. The restore rehearsal gate passes for the current tool revision, exactly as today.
4. The badge is **already running our firmware**: read the 4 KiB installation-header
   sector at `ZT_INSTALL_OFFSET` and require a structurally valid header — magic,
   schema, length, CRC over its covered bytes, `nvs_offset`/`nvs_length` equal to the
   contract values, the initialised flag set, and a `mac` equal to the selected badge's
   full factory MAC. A blank, corrupt, foreign or absent header means this badge may be
   carrying stock state that has never been captured, so take the ordinary path.

### What the fast path does

- Skips the fresh dual capture only.
- Writes only `0x10000..0x2affff`, unchanged.
- **Keeps the complete 4 MiB preboot readback**, comparing by bytes and by SHA-256 before
  any boot, unchanged. Build the expected image as: `original/`'s bytes everywhere
  outside the application range, and the release's padded application inside it.
  Note what this buys, and say it in `docs/recovery.md`: because the expectation outside
  the app range comes from `original/`, this comparison now **detects any drift in the
  stock regions**. If a stock region has changed since enrolment, the readback fails and
  the operator learns something important. Treat that as a hard failure and stop.
- Records the decision durably in the operation: that the pre-write capture was skipped,
  every condition that justified it, the installation header bytes observed, and which
  snapshot supplied the non-application expectation. An operation record must never imply
  a capture that did not happen.
- The operation's recorded status vocabulary is unchanged: this still ends at
  `FLASH_VERIFIED` and nothing else.

### What must not change

- No new force, fast, yes or skip flag. No arbitrary offset. No chip erase. No change to
  identity, security, release verification, or rehearsal gating.
- `original/` stays immutable. The fast path reads it and never writes it.
- `restore`, `rehearse-restore`, `enroll`, `snapshot`, `verify` and `commission` are
  untouched. A first flash onto a stock badge still takes the full capture, always.
- Never treat a failed fast-path precondition as a reason to refuse: fall back.

## Expected consequence, state it in your report

`zt_badge.revision()` hashes `zt_badge/*.py`, so this invalidates the restore rehearsal
recorded against the current revision and the operator must rehearse once more before the
next flash. That is known and accepted. Do not try to preserve the hash and do not touch a
recorded result.

## Allowed checks

`python3 -m py_compile` on each changed file and `badge_tool.py --help`. **Nothing that
opens a serial port, touches an archive, or prompts.** No tests, no mock device.

**No hardware. No credentials. No archive access. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the four allowlisted files changed.
2. Every precondition above is enforced, and any failure falls back rather than refusing.
3. Identity, security, release and rehearsal gates are untouched.
4. The full 4 MiB preboot comparison still happens, built from `original/` outside the
   app range, and stock-region drift fails the flash.
5. The operation record states plainly that no capture was taken and why that was allowed.
6. No new flag, no new command, no relaxation anywhere else.

## Final report

Standard format. State exactly which conditions gate the fast path, what the operation
record now contains, and confirm the readback is still a complete 4 MiB comparison.
