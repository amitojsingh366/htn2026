# Packet G05 — boot straight into the game

## The operator's ask, verbatim

"can we not have a firmware build where it literally loads in lobby and shit like why we
rebooting 2x and stuff why not just make the firmware completely around this game. i dont
wanna have more apps, just this game right"

They are right. A badge running this firmware should power on and be in the game. Today it
boots to `WAIT_INSTALL`, needs an operator step, and the integration layer's workaround
restarts twice. That is a workflow leaking into the product.

## Base commit and dependencies

- Base commit: `00741088bcd9395807800f053a286b480d243144` on `firmware/integration`.
- Branch: `firmware/G05-boot-straight`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/G05`
- You wrote `zt_store` and `zt_game` (G01) and extended them in G02, G03 and G04.

## Owned file allowlist

```
firmware/components/zt_store/store.c
firmware/components/zt_game/game.c
```

No headers, no other component, no `app_main.c`, no `tools/`, no documents.

## What must stay true

Read `plan.md` §3.1 and §3.2 before changing anything. The installation header is not
bureaucracy: it records that the final 64 KiB was verified **entirely blank** before this
firmware started using it, so the firmware can never quietly consume flash that belonged
to something else. Two rules are absolute and this packet does not touch them:

- A **non-blank tail that is not a valid header for this badge** still refuses to
  initialise, still reports a storage error, and still never formats or erases anything.
  There is no format-on-error path, and none may be added.
- `zt_store_install_init()` and the operator's guarded `install-init` keep working exactly
  as they do now, because that is the path that writes a header with a **tool-verified**
  baseline hash and a **tool-issued** UUID. A badge installed that way is provably bound
  to a verified backup, and nothing here may weaken or bypass that.

## Task 1 — one boot, no WAIT_INSTALL detour, when the tail is provably blank

Add a build-time option in `store.c`, defaulting **off**, for example
`ZT_STORE_SELF_INSTALL`. When it is enabled **and** the inspection finds the entire tail
blank — the same check that exists today, unchanged — initialise the named NVS partition
and report `ZT_INSTALL_VALID` during `zt_store_inspect()`/`zt_store_open()`, instead of
returning `ZT_INSTALL_WAIT`.

Write a local header so later boots recognise the installation, marking it clearly as
self-installed rather than tool-verified: a distinct flag bit or a reserved-value baseline
that is obviously not an image hash. **Do not** fabricate a value that could be mistaken
for a tool-issued UUID or a real baseline hash, and keep the ordering plan §3.2 requires —
NVS initialised first, raw header written last.

Say plainly in your report that a self-installed badge is not commissionable and that the
guarded tool will refuse to flash over such a header, because the tool validates the
baseline against the archive. That is correct behaviour and the operator knows it: this
option exists for their own development badges, not for participants.

With the option off, behaviour is byte-for-byte what it is today.

## Task 2 — no reboot after configuration

`zt_game_init()` loads configuration once and, finding none, settles in
`ZT_ADMISSION_NEEDS_CONFIG`. Today the only way out is a restart, which is why the
integration layer reboots.

Make the game leave `NEEDS_CONFIG` on its own when configuration becomes durably present:
re-read it during `zt_game_service()` while in that state, at a bounded interval rather
than every iteration, and when a valid configuration for this badge appears, complete
exactly the initialisation that `zt_game_init()` would have done with it — configuration
adopted, name published, host-configured decided, round checkpoint loaded if one exists,
peers configured, admission moved to `LOBBY` — and publish a snapshot.

Constraints:

- The configuration's `expected_mac` must equal this badge's factory MAC, or it is a
  storage error, exactly as at init.
- Only from `NEEDS_CONFIG`. A configured badge never re-reads configuration and never
  re-initialises mid-round.
- The game task remains the only writer. No new API, no header change, and the existing
  single-owner check stays.
- If the stored configuration is malformed, report a storage error rather than looping.

## Task 3 — radio start without a reboot

The radio is started by the integration layer after it observes a configured badge, which
is the other reason a restart was needed. The snapshot already carries everything that
decision needs, so make sure that once Task 2 moves the badge to `LOBBY`, the published
snapshot reflects a configured badge with its channel and host status, and that nothing in
the game assumes the radio was already running when it reached `LOBBY`. The orchestrator
wires the actual start; do not touch `app_main.c`.

## Excluded

No format, erase, or recovery-by-deletion path. No change to the durable journal, event
immutability, tag rules, causality, admission refusals, host origination, or the clock. No
new API, packet or command. No backend. No tests or harness.

## Allowed checks

Per-file compilation with the installed cross compiler, as before. **Do not run
`idf.py build`**; the sandbox blocks it with the psutil `sysctl()` PermissionError and
that is expected. The orchestrator runs the authoritative build.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the two allowlisted files changed.
2. With the option off, today's behaviour is unchanged, including `WAIT_INSTALL` and the
   guarded install path.
3. A non-blank tail without a valid header still refuses and still never erases.
4. A self-installed header is distinguishable from a tool-issued one and is not passed off
   as verified.
5. `NEEDS_CONFIG` to `LOBBY` happens without a restart, only from that state, with the
   MAC check intact and no re-initialisation of a configured badge.
6. No durability, validation or refusal weakened anywhere.

## Final report

Standard format. State what a fresh badge now does from power-on to `LOBBY`, how many
reboots that involves, and exactly what the self-installed header looks like versus a
tool-issued one.
