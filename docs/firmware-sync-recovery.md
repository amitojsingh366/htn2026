# LIVEG011: sync recovery and shared countdown

Branch `codex/firmware-sync-recovery` starts from main commit `62c4dab`.
Firmware fixes remain independent of the backend's all-ready requirement. The
later countdown request also changes the dashboard and server start delay.

## Missing badges

An unreachable badge no longer holds the gateway's incoming command mailbox.
Overflow command copies remain on the server for replay; no synthetic applied
or ready receipt is generated. Pending clocks, snapshots or relay capacity do
not prevent unrelated queued commands from being serviced.

The host makes three radio delivery attempts over a maximum six-second active
window, then keeps the command in its bounded catch-up cache. Server duplicates
replay the host's genuine receipt without restarting absent-peer retries. A
returning badge requests a snapshot and command replay. Lost or partial repairs
retry at the existing five-second interval with jitter. Complete periodic
snapshots postpone a thirty-second fallback, so healthy badges do not poll
continuously. Both direct and relayed host contact trigger repair on return.

This recovery applies during the same boot. The existing fresh-boot policy,
which clears local game history and requires A registration again, is unchanged.
Starting still waits for every registered badge's real PREPARE receipt.

## Host buttons

START lets already queued registrations make their first HTTP attempt before
freezing the roster. It does not wait for their radio replies or failed HTTP
retries. RESET retains priority. Button errors distinguish missing host
registration, too few registrations, an active round, a changed round and an
unavailable server feature. Unsupported control endpoints stop automatic HTTP
retries so they cannot keep interrupting live sync.

The exact earlier B: RETRY rejection was not captured. A read-only endpoint
check returned authenticated-route HTTP 401, and public state showed an empty
lobby; the historical handoff's claim that the control endpoint was still
undeployed is no longer current evidence. No live Start or Reset request was
sent during this work.

## Countdown

After all PREPARE receipts arrive, the server assigns roles and one shared start
timestamp five seconds in the future. Change `START_COUNTDOWN_MS` in
`backend/src/gateway.ts` to `60_000` for the longer game countdown.

Badges show their role and countdown; the dashboard shows the same deadline
using `server_time_ms` to account for browser clock differences. Round timers
stay at their initial values until zero. Gameplay, tag requests and queued
button presses captured before zero are inactive. Dashboard Start/Reset controls
are disabled during the countdown. Status viewing remains available on badges.

The five-second schedule requires deploying the backend change, and the visible
dashboard countdown requires the frontend change. Firmware alone uses whichever
start timestamp the deployed server supplies. No deployment or flashing is part
of this branch's validation.

## Validation

ESP-IDF 5.5.3 firmware compilation, backend TypeScript checking and frontend
production compilation are used for validation. No hardware tests, simulations,
serial access or automated test suites are run. The pre-existing unused
`self_install` firmware warning and four pre-existing frontend lint findings
remain outside this change.

Builds and offline release artifacts are in the ignored `.orchestration/` and
`firmware/build/` directories of this worktree. Flash through the existing
guarded `badge_ops.py` workflow, using the release generated from this branch.
