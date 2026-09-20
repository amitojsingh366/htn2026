# Live badge registration, infections, results and reset

Game: `005a544d454d4f01`. Backend and dashboard origin:
`https://htn2026-backend.amitoj.workers.dev`.

Only `ami`, the designated host with AUX1 on at boot, connects to the Internet.
Other badges use authenticated ESP-NOW through that host. Press A on every badge,
including the host, to register. Start in the dashboard freezes the roster and
waits for each badge to persist PREPARE before scheduling initial roles.

With LIVEG010 and its backend update, the host can also press **B: START GAME**
in the registered lobby. After an authoritative winner appears, **B: RESET GAME**
clears the server and delivers badge cleanup. Players then press A to register
again, and the host presses B to start the next round. Zombie screens identify
**A: INFECT**; a tag still requires the selected human to be in direct proximity.
B keeps its list/radar function during active play.

Host controls use a button-triggered authenticated HTTPS request, serialized with
the gateway's other TLS work. The captured registration and stable request ID
prevent retries from operating on a later lobby or round. Reset is available only
after the server has ended that round; start still requires at least two saved
registrations, including the host, and durable preparation by every player.

The matching backend must be deployed for either host button to work; flashing
firmware alone does not install the `/gateway/control` route or server-side
winner handling. An older server can return HTTP 426 because it routes the
unknown control path to its WebSocket handler. Firmware now treats HTTP
404/405/426/501 from that control request as an unavailable server feature,
shows **SERVER UPDATE NEEDED**, and stops retrying that button request so live
sync can reconnect. The operator must update the matching backend (and verify
the configured game if the server returned 404), then press B again. Network
failures still retry the same request identity. No firmware fallback invokes
the dashboard's separate start/reset routes.

## Private build configuration

The ignored `private/zt_gateway_private.h` supplies `ZT_PRIVATE_GATEWAY_TOKEN`.
The ignored `private/zt_device_private.h` supplies `ZT_PRIVATE_WIFI_SSID`,
`ZT_PRIVATE_WIFI_PASSWORD` and the existing 32-byte `ZT_PRIVATE_MESH_KEY`.
Use `docs/device-private.example.h` as its structure; local credentials are not
committed. LIVEG010 moves the existing values out of the entry point without
changing the fleet's settings.
The matching ignored `private/gateway-secrets.json` supplies `ZT_GATEWAY_TOKEN`
and `ZT_HOST_MAC` to the Worker. Keep these local. The firmware copies the token
into configuration only for the designated host, and sends it only in bearer
authorization headers. No default token is provided.

Changing either private header requires rebuilding and packaging the application.
Lobby self-configuration updates differing baked configuration on boot. LIVEG005
starts a fresh local game on every boot independently of configuration changes. The development
packager records each header's hash without copying its contents into metadata.

## Build and flash

Use the installed ESP-IDF 5.5.3 Python 3.12 environment:

```sh
export IDF_PYTHON_ENV_PATH=/Users/amitojsingh/.espressif/python_env/idf5.5_py3.12_env
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
. /Users/amitojsingh/esp/esp-idf/export.sh
cd /Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/firmware
idf.py build
cd ..
.venv-badge/bin/python tools/badge_dev_release.py --allow-dirty
.venv-badge/bin/python tools/badge_ops.py
```

Select the badge and `2. flash`; the operator menu uses the newest release.
Use the guarded menu for each badge. The full-flash commands printed by ESP-IDF
are not the deployment workflow.

## Current scope

LIVEG010 fixes several registration and same-boot catch-up defects. Intermediate
badges now relay valid JOIN and snapshot requests instead of rejecting those
transit packets. The host keeps per-badge limits without dropping a different
badge behind a shared 250 ms gate. JOIN retries use jitter, and local queue
backpressure gets a shorter retry. A registered badge missing its first frozen
round retries its snapshot request after 1–1.5 seconds; established-round repair
keeps a slower limit. Fresher revision/phase hints and a returning host trigger
snapshot recovery. The host retains frozen PREPARE information for rejoining
clients, including when the current snapshot revision has advanced.

Mesh cache repair retries unresolved inventory differences after a lost exchange,
and peers exchange inventories even when one cache starts empty. Events remain
in the same-boot journal until real server receipts/decisions. This does not
change LIVEG005's intentional reboot policy: reboot clears local history, so
unuploaded tags cannot be recovered after a power cycle.

The gateway caches answered registrations, schedules retry failures per request,
and allows WSS progress while an individual JOIN is waiting. Duplicate JOINs no
longer require another HTTPS request and live socket teardown. A valid bootstrap
can be reused after registration. Acknowledgments are batched, and one outbound
application request awaits a reply or bounded timeout before the next; active
catch-up is paced at 100 ms while idle polling stays at two seconds. The backend
replies to every normal ACK, including an empty command batch when idle, and
prioritizes game-over messages in batches of at most four commands. Duplicate
commands have a one-second retry cooldown; obsolete role updates are skipped.
These are scheduling limits, not measured end-to-end latency guarantees.

The dashboard now shows an explicit GAME OVER winner banner and a frozen final
timer. The public state read during this task showed three accepted zombie roles
but `game_over:false`, confirming the still-deployed older backend lacks the
local winner implementation below. Deploying the prepared backend/dashboard is
required as well as flashing LIVEG010; consult `HANDOFF.md` for approval and
deployment status. No hardware behavior has been measured by the agent.

LIVEG009 source separates the persistent live connection from short HTTPS
requests. Shared link labels now read `LIVE SYNC ONLINE` / `LIVE SYNC WAIT`.
The online flag still requires a connected, welcomed, fresh WSS session; its
meaning and mesh flags are unchanged. HTTPS registration intentionally closes
WSS so only one TLS session exists, so the website can receive badge registrations
while live sync waits. The host detail shows `REGISTERING BADGE` or
`SYNCING WITH SERVER` during HTTPS work, `SERVER READY` when live sync is ready,
and `SERVER RECONNECTING` for up to 25 seconds after a validated successful HTTP
reply when no gateway error is present. Existing hotspot/error details,
`YOU ARE HOST`, winners, and LEDs remain intact.

The reset changes below are also part of LIVEG009 source. Backend and frontend
changes remain local pending approved deployment; firmware packaging status and
the actual deployed Worker are recorded in `HANDOFF.md`. No hardware result is
claimed for these changes.

LIVEG008 processes pending JOIN requests in round-robin order so a repeatedly
retrying badge cannot starve host registration or other badges. A registration
rejected with HTTP `400`/`422` is handled for that badge without halting the
entire gateway. Closed registration, a full roster, and invalid badge
configuration stop that badge's automatic registration retries.

The host lobby distinguishes waiting for hotspot IP from connecting to the
authenticated server link and shows readable network, response, authentication,
and server errors. Host Status page 3/4 exposes HTTP status, gateway error, Wi-Fi
IP readiness, and gateway initialization. Being associated with the phone's
hotspot does not establish an authenticated server connection.

These registration fixes require LIVEG008 firmware but do not depend on the
LIVEG007 backend changes. Deployment of the LIVEG007 winner/backend and frontend
changes is still awaiting explicit user approval; consult `HANDOFF.md` for the
actual deployed release.

LIVEG007 ends play as soon as the server's canonical roster is entirely infected,
with `END_ROUND` reason `all_infected` and a provisional zombie win. If humans
survive until the ten-minute deadline, it sends reason `time_limit` and a
provisional human win. Badges immediately headline `ZOMBIES WIN` or `HUMANS WIN`
with `SYNCING RESULT` while the result is provisional. A local timeout can show a
provisional human win before server reconciliation; later authoritative evidence
can correct that result.

The server remains in `expired_pending_sync` until every roster badge has sent
`round_closed` and every event through each badge's final produced sequence has
an accepted or rejected decision. It then publishes `FINAL_RESULT` and enters
`final`. Missing badges or undecided dependencies keep the result provisional;
they are not silently discarded by a timeout. The scheduled `end_time_ms` stays
equal to `start_time_ms + 600000`, including an early zombie win. Terminal
snapshots separately carry `effective_elapsed_ms`, `result_present`,
`result_final`, `result_complete`, `winner` (`H` or `Z`), and
`missing_slots_bitmap` so reconnecting badges recover the same result.

Passive role LEDs are steady red for zombies and blue for humans on all six
pixels. Approaching opponents add one to four yellow proximity LEDs with faster
pulsing at closer ranges; the bottom pair retains the role color. The electrical
brightness cap is unchanged, and yellow does not increase the passive pattern's
maximum current budget. Brief tag and infection feedback still take priority.

LIVEG006 displays `YOU ARE HOST` on the configured badge selected as host by AUX1
at boot. Client badges keep `HOST ONLINE` / `HOST OFFLINE`, and server connection
status remains separate. This display-only change retains LIVEG005's boot policy
below and does not fix power-related reboots.

LIVEG005 starts every badge in a fresh local lobby after reboot. It clears saved
rounds, pending local tags and reset receipts while retaining badge/Wi-Fi/server
configuration. Press A to register before the badge can admit a game. This is the
operator-requested development policy; reboot no longer resumes a saved round.
The server's saved registrations/round are separate and use dashboard Reset Game.

LIVEG003 adds infection uploads, durable server receipts, causal decisions,
canonical role commands and current-role dashboard counts. Local tags remain in
the durable journal until their actual server receipt/decision; transport send
success is never substituted for either. Missing parent events and victim-sequence
gaps remain pending until their evidence arrives. Duplicate delivery is idempotent.

The dashboard Reset Game button immediately clears the server round and saved
registrations, including an earlier reset stuck waiting for badges. Offline badges
and forgotten sessions do not block it. Already uploaded evidence stays archived;
the Durable Object is retained and command sequences keep increasing.

LIVEG009 changes archived cleanup delivery to send peer resets before the host
reset, so the host can relay them through the mesh before clearing its local
state. Each peer is offered its reset once in batches of up to four, with a bounded
15-second retry window before the host reset proceeds. Earlier completion of peer
cleanup releases the host reset sooner. This window starts after the server is already clear;
offline badges never block the server reset or keep the host waiting indefinitely.

Resetting a registered lobby also creates archived cleanup, using a synthetic
round ID and each badge's registration request key. The host's optional `hello`
`registration_id` identifies its current registration. Lobby `RESET_GAME` commands
carry the flat field pair `target_mac` and `registration_id`; the mesh carries
six MAC bytes plus an eight-byte little-endian registration ID. A badge must
match that registration instance before applying cleanup, so an old reset cannot
clear a new registration in a reused slot. Ordinary round resets retain their
legacy zero-argument form. Both backend deployment and LIVEG009 firmware are
needed for registration-bound lobby cleanup.

An unregistered host can recover peer cleanup using the `host_boot` saved from
its last normal handshake, without becoming a registered player or receiving a
host reset command. Recovery finishes after the bounded peer window and does not
loop on the synthetic round. Bootstrap continues to describe the current server
game; cleanup recovery uses WSS. Replaying an old pre-reset registration key
returns `BAD_CONFIGURATION` instead of silently repopulating the cleared lobby;
pressing A for a fresh registration supplies a new request identity.

Badge cleanup remains best effort. Offline clients may retain their old local
game until they receive reset or reboot with LIVEG005 or later. Pending local
tags are cleared by either action. Archived receipts update only archived
cleanup and cannot change the new active game.

Per-player scoring/ranking, operator force-finish APIs, and late uploads for
archived rounds remain unfinished. The gateway uploads only active-round events;
other retained evidence stays local for the current boot until reset/export.
Normal win determination and round finalization are part of LIVEG007. Backend
redeployment and reconnects do not reset an active round or reselect Patient Zero.

No automated tests or hardware validation are part of this implementation pass.
Build success does not establish an end-to-end hardware result. Radio startup now
waits for game/storage boot completion instead of assuming it finishes in 200 ms.
The reported LIVEG004 host reboot cause remains unconfirmed; clearing saved game
state does not establish a hardware reset diagnosis. The host heartbeat
reports gateway state, HTTP status, clock readiness, free heap and largest block
to support the operator's manual result without printing credentials.

## LIVEG002 recovery fixes (2026-09-20)

This section records the earlier release; current LIVEG003 behavior is above.

The gateway now forwards WebSocket control frames, so pong replies reach the
client's heartbeat handler. The strict upgrade wrapper forwards the underlying
socket and TLS error state, and avoids the large `sscanf` stack frame. A full RX
mailbox drops the additional complete message and recovers through requests,
rather than restarting TLS. Empty lobby acknowledgments discover a missed round
snapshot; receipt replay remains paced and fair across badges.

A host with hotspot credentials advertises its lobby only after acquiring IP.
Players rediscover after ten seconds without a fresh lobby contact; frozen rounds
retain their locked channel. A registered player missing the first roster retries
using a round-zero snapshot request, including when the host is already locked.
PREPARE requires confirmation of the actual channel lock before saving READY.

Before Start, `ROUND ROSTER PENDING` is expected. After Start, the dashboard's
frozen roster, prepared and Start-applied counts expose delivery progress.
Registration is saved membership, not live radio presence. `SERVER RX AGE S`
resets on inbound frames, including pongs; it is not connection uptime.

The user reported zero roster both before and after Start and currently has the
host on USB. No crash/reset output was available. The reboot cause remains
unconfirmed. Sanitized heartbeat diagnostics now include firmware build, uptime,
reset reason, minimum free heap and both gateway task stack watermarks. No badge
was accessed or flashed during this fix; the user performs manual verification.

Firmware release: `.orchestration/releases/dev-20260920T065942.146989Z/release-manifest.json`.
Worker version: `6d0fc0fc-504a-4987-bd43-cf203a80b1bb`.
Flash both badges through the guarded menu. Keep ami on USB with AUX1 on at boot,
wait for its Wi-Fi connection, and press A on each badge if it asks to register.
The existing backend round was preserved by that deployment. Use the explicit
Reset Game control only when intending to clear badge state and registrations.
