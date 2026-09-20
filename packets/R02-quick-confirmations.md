# Packet R02 — one-keystroke operator confirmations

## Goal

Replace the typed-sentinel confirmations in the guarded recovery tool with `y`/`n`
answers, without weakening a single property that makes those confirmations meaningful.

This is direct operator instruction. Their words: "dont have me typing all this REVIEWED
and FLASHSMTGSMTG, it should be one word answers mainly y/n type so its quick."

## Base commit and dependencies

- Base commit: `6ab4ed319e3e7d697b2b73134870725327a2a8a8` on `firmware/integration`.
- Branch: `firmware/R02-quick-confirmations`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/R02`
- R01 and C01 are both merged. C01 added `cli_console.py`, which also calls `confirm()`.

## Owned file allowlist

```
tools/zt_badge/cli_recovery.py
tools/zt_badge/cli_console.py
tools/zt_badge/release.py
docs/recovery.md
```

No other file. Not `device.py`, `gates.py`, `archive.py`, `manifest.py`, `index.py`,
`config.py`, `bundle.py`, `badge_tool.py` or `badge_ops.py`. Touch no firmware.

## What must not change

Read `confirm()` in `cli_recovery.py` and its docstring before editing. Every one of
these properties is deliberate and must survive:

1. **Fail closed.** No answer, an unreadable terminal, a closed stdin, EOF, or any answer
   other than an explicit yes means refuse, raising the existing
   `LOCAL_OPERATOR_OBSERVATION_REQUIRED` / `OPERATOR_OBSERVATION_NOT_CONFIRMED`
   conditions. Never treat a bare Enter, an empty line, a default, or a timeout as yes.
2. **A real human, really present.** `/dev/tty` first, then the preserved operator fd with
   an interactive stdin. A piped, redirected or heredoc answer must still be refused —
   that is the reason a pipeline cannot drive this tool today, and it must stay that way.
   Do not add a flag, an environment variable, or an argument that supplies an answer.
3. **One prompt per recorded outcome.** Each distinct attestation keeps its own separate
   prompt and its own separate answer. Do not merge two confirmations into one, do not
   ask once and reuse the answer, and do not skip a prompt because an earlier one in the
   same command was answered.
4. **The prompt still says exactly what is being attested**, in full, before asking. The
   operator is confirming a physical observation; shortening the ANSWER must not shorten
   the STATEMENT. If anything, state it more plainly now that the reply is one key.
5. **The recorded evidence is unchanged.** The strings written into `result.json` — the
   status vocabulary `BACKUP_VERIFIED`, `RESTORE_VERIFIED`, `STOCK_BOOT_CONFIRMED`,
   `FLASH_VERIFIED`, `COMMISSIONED`, `rehearsal_accepted`, and the rest — must not change
   at all. `gates.rehearsal()` and `gates.application()` read those records; renaming one
   would silently invalidate accepted evidence. This packet changes how an operator
   answers, never what is recorded.

## What to change

- `confirm(prompt, expected)` becomes a yes/no question. Accept `y` and `yes`, case
  insensitive, and nothing else as agreement. Everything else, including `Y E S` with
  spaces, refuses.
- Rewrite each call site's prompt so it reads as a question ending in ` [y/N]: `, keeps
  the entire existing explanatory text, and no longer instructs the operator to type a
  sentinel. Call sites: participant agreement, the stock-boot observation, the rehearsal
  acceptance, the archive-root independence attestation, the commissioning
  initialisation, and the build review in `release.py`, plus whatever `cli_console.py`
  added.
- The commissioning role question in `cli_recovery.py` reads `player` or `host` from the
  terminal. That is a choice between two values, not a confirmation. Keep it, but accept
  `p` and `h` as well as the full words. It must still refuse anything else.
- Keep the `N` capitalised in ` [y/N]: ` so the safe answer is visibly the default, while
  still requiring an explicit `y` and never actually defaulting on empty input.

Update `docs/recovery.md` wherever it quotes a prompt or tells the operator to type a
sentinel, including the section that documents the confirmations verbatim. State plainly
that answers are now `y`/`n`, that an empty answer refuses, and that a piped answer is
still refused.

## Expected consequence, state it in your report

`zt_badge.revision()` hashes `zt_badge/*.py`, so this changes the tool revision and
invalidates the restore rehearsal recorded against the previous revision. That is known,
accepted, and already being paid for by C01's merge in the same round. Do not attempt to
preserve the old hash, do not copy code into an unhashed file to avoid it, and do not
touch any recorded result to make an old rehearsal match.

## Allowed checks

`python3 -m py_compile` on each changed file, and `--help` on the tool. **Do not run any
command that opens a serial port, touches an archive, or would prompt.** No tests, no
mock terminal, no harness.

**No hardware. No credentials. No archive access. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the four allowlisted files changed.
2. Empty input, EOF, a piped answer and any non-`y` string all refuse; only an explicit
   `y`/`yes` proceeds.
3. Every prompt still states the full attestation; no two confirmations were merged.
4. No recorded status string, evidence field or gate input changed.
5. No flag, argument or environment variable can supply an answer.

## Final report

Standard format. List every prompt you changed with its new wording, and state explicitly
that no recorded outcome string changed.
