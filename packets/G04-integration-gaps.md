# Packet G04 — close the real post-merge gaps in the game module

## Read this section before anything else

You wrote `zt_game` and `zt_store` (packet G01), then host origination (G02), then
snapshot admission (G03). All three are merged, the firmware builds and links, and a
badge has been flashed and is running this code on real hardware: it renders, it reaches
WAIT_INSTALL, and it accepts USB provisioning. This packet is not new features. It is the
set of gaps that only became visible once everything was merged and other packets' work
landed around yours.

Each item below names the evidence. None of them is speculative.

## Base commit and dependencies

- Base commit: `34e92a9334412ffad29c6d98963f6eab29e64415` on `firmware/integration`.
- Branch: `firmware/G04-integration-gaps`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/G04`
- Merged and available to you: H01 (`zt_hal`, `zt_ui`), M01 + its follow-up (`zt_radio`
  with the boot-nonce getter, the diagnostics getter and mesh round restoration), your own
  G01/G02/G03, C01 (`zt_console`), and contract amendments 1 through 5.

## Owned file allowlist

```
firmware/components/zt_game/game.c
firmware/components/zt_game/clock.c
firmware/components/zt_game/peers.c
```

Nothing else. Not `zt_store`, not `zt_radio`, not `zt_ui`, not a header, not
`app_main.c`, not `zt_demo`, not a document, not `tools/`. Packet D01 is concurrently
writing `components/zt_demo/` and packet H01 is writing `components/zt_ui/`; do not
touch, depend on the internals of, or coordinate with either.

## Gap 1 — nothing ever calls `zt_radio_mesh_restore()`

Contract amendment 3(b) exists because you asked for it, M01 implemented it, and its
header comment states plainly that **the game task calls it**, once per retained round,
after that round's checkpoint is durably restored, **previous round first and the active
round last**, because the most recent successful call selects the round the ordinary
beacon carries.

No call site exists. The consequence on hardware: after any reboot the mesh RAM caches
are empty, so the badge advertises an empty cache digest, cannot answer WANT_EVENTS for
events it still durably holds, and cannot offer previous-round inventories at all. Events
that survived the reboot in NVS become invisible to every neighbour. Offline recovery —
one of the four things this firmware exists to demonstrate — silently does not work.

Call it from the game task at the right point in your restoration path, in the documented
order, and handle its failure returns without aborting boot. Re-read the amendment's
comment in `zt_radio.h` first.

## Gap 2 — the radio half of `zt_diagnostics_t` is never populated

`zt_ui_snapshot_t.diagnostics` reserves `rx_drops`, `tx_drops`, `invalid_frames`,
`auth_failures`, `dedupe_hits`, `rx_high_water`, `tx_high_water`, `tx_watchdogs`,
`radio_restarts` and `replay_backlog`. You currently set only the few you own directly.
Contract amendment 3(a) added `zt_radio_get_diagnostics()` specifically so these could be
filled, and M01 implemented it, counting HMAC failures separately from malformed frames.

Nothing calls it. The diagnostics screen and the console `status` operation therefore
report zeros for every radio counter, which is worse than reporting nothing: during the
three-badge demo the operator will read "0 auth failures, 0 drops" whether the radio is
healthy or completely broken.

Copy the getter's fields into the published snapshot on your ordinary publish cadence —
it is read-only, non-blocking and safe from any task. Also set `diagnostic_mode` in the
snapshot from the getter's `diagnostic_mode`, so the DIAGNOSTIC banner that plan §13
requires whenever the link allowlist is engaged can actually be raised by the UI.

While you are there, fill `free_heap`, `minimum_heap` and `largest_free_block` from the
ordinary IDF heap APIs. plan §7 asks for exactly those to be recorded on the first host
badge, and there is no other place they can come from.

## Gap 3 — the half-RTT clock adjustment

M01 reported this honestly as needing integration confirmation: the matched TIME_REPLY
validation exists in the mesh, but the **game owns the clock**, and `zt_clock_apply()` is
yours. plan §5.6 fixes the rule and §4.2 governs the rest: an anchor's uncertainty is
`ceil(RTT/2) + 50 ms`, an anchor is accepted only for RTT ≤ 2000 ms with a sane
server/round match, and uncertainty then grows by 200 ppm of monotonic elapsed time.

Make the peer time exchange apply half the measured round trip when it adopts a sample,
and carry the resulting uncertainty. The invariants in §4.2 are absolute and must not be
weakened to make a sample usable: a new sample may **shorten** an established local
deadline but must **never extend** it; a badge that cannot establish that the round is
still running refuses new tags; and a tag requires
`estimated_elapsed_ms + uncertainty_ms < 600000`.

## Gap 4 — an honest refusal is being counted as a drop

`app_main`'s gateway feed sink returns `ZT_ERR_INVALID_STATE` on every event, because no
gateway exists yet and claiming a delivery that did not happen would be a lie. Your
publish path increments `diagnostics.gateway_drops` each time. On a player badge that
counter will therefore climb forever during the demo and mean nothing.

Distinguish "there is no gateway on this badge" from "the gateway dropped something".
A badge with no gateway should not accumulate gateway drops. Do not change the sink
contract and do not start returning ZT_OK for undelivered events.

## What must not change

Everything that already works on hardware. Specifically: the single-writer rule, the
PERSISTING lock and its 500 ms PENDING behaviour, event immutability and the durable
journal, tag range and cooldown, causality and parent-chain validation, the admission
refusals G03 added, the host-origination retry and receipt rules G02 added, and every
existing snapshot and command validation. This packet adds no gameplay rule and relaxes
none.

## Excluded

No backend, HTTPS, WebSocket or gateway implementation. No new API, header change, packet
type, command type or NVS key. No demo logic. No tests, harness, mock or CI.

## Allowed checks

Per-file compilation with the installed cross compiler, as in your previous packets.
**Do not run `idf.py build`** — the sandbox blocks it with the psutil `sysctl()`
PermissionError; that is expected, is not your problem, and is not a
`BLOCKED_ENVIRONMENT` for this packet. The orchestrator runs the authoritative build.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the three allowlisted files changed.
2. `zt_radio_mesh_restore()` is called from the game task, per retained round, previous
   first and active last, with failures handled and boot never aborted.
3. Every radio-owned diagnostics field and the heap fields are populated; `diagnostic_mode`
   reflects the allowlist.
4. Half-RTT is applied with §4.2's uncertainty rules intact; no deadline can be extended.
5. A gateway-less badge no longer accumulates gateway drops, and no undelivered event is
   ever reported as delivered.
6. No existing validation, refusal or durability guarantee weakened.

## Final report

Standard format. For each of the four gaps, state exactly what you changed and where. Be
explicit about anything you could not close, and why.
