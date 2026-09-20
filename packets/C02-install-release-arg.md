# Packet C02 — stop making the operator paste a release path

## The problem, in the operator's words

"bro why do i have to put this path everytime"

They are right. `install-init` prompts interactively:

```
Enter the absolute local release-manifest path used for the recorded flash:
```

and offers no argument for it. The operator CLI already resolves that badge's most recent
`FLASH_VERIFIED` operation UUID from the archive without asking, and the release path is
derivable from the **same record**, since the flash operation stores the release hash.
But the wrapper cannot supply a value to a prompt — that rule is what keeps every operator
confirmation genuinely the operator's — so the path has to be pasted by hand each time.

The fix belongs here: accept it as an argument.

## Base commit and dependencies

- Base commit: `5263eea4cfebddf1204e4c0d6ed8641bdbe49ef0` on `firmware/integration`.
- Branch: `firmware/C02-install-release-arg`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/C02`
- You wrote `cli_console.py` in packet C01. Packet R03 is merging around the same time and
  touches `device.py`, `gates.py`, `cli_recovery.py` and `docs/recovery.md`. **Do not
  touch any of those four files**; the orchestrator will merge both.

## Owned file allowlist

```
tools/zt_badge/cli_console.py
docs/console.md
```

Nothing else. No firmware. Not `badge_ops.py`.

## Required behavior

1. Add an optional `--release` argument to `install-init` taking the absolute path to a
   release manifest. When given, use it and **do not prompt**.
2. When it is absent, prompt exactly as today. Do not change the prompt's wording and do
   not add a default, so an operator running the tool directly sees no change.
3. **Validate identically in both paths.** The supplied path must go through exactly the
   same checks the prompted value does today — the recorded flash operation's release hash
   must match, and every existing release verification must run unchanged. An argument is a
   convenience for typing, never a shortcut past a check. If the two disagree, refuse with
   the existing condition, do not fall back to prompting, and do not retry.
4. Keep it an absolute path, rejecting a relative one as now.

That is the whole change. Do not add arguments to any other command, do not add a flag
that suppresses any confirmation, and do not touch the `y/N` confirmations at all.

Update `docs/console.md` where it documents `install-init`: the new argument, that it is
optional, and that it changes nothing about validation.

## Expected consequence, state it in your report

`zt_badge.revision()` hashes `zt_badge/*.py`, so this changes the tool revision and
invalidates the restore rehearsal recorded against the previous one. That is known and is
being paid once, together with packet R03's change, rather than twice. Do not try to
preserve the hash and do not modify any recorded result.

## Allowed checks

`python3 -m py_compile tools/zt_badge/cli_console.py`, `badge_tool.py --help`, and
`badge_tool.py install-init --help`. Nothing that opens a serial port, touches an archive,
or prompts. No tests, no mock device.

**No hardware. No credentials. No archive access. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the two allowlisted files changed.
2. With no `--release`, behaviour is identical to today, prompt included.
3. With `--release`, no prompt, and every validation still runs.
4. A mismatch between the argument and the recorded flash refuses; it never prompts as a
   fallback and never retries.
5. No other command gained an argument, and no confirmation can be supplied by argument.

## Final report

Standard format. Confirm the validation is identical on both paths and that no
confirmation became suppressible.
