# LIVEG011 branch — nonblocking sync and shared demo countdown

Current work is on `codex/firmware-sync-recovery`, based on main `62c4dab`, in
`/Users/amitojsingh/Desktop/misc/hackerbadge/firmware-sync-recovery`.
See `docs/firmware-sync-recovery.md` for behavior and rollout details.

The user requested firmware fixes on a separate branch with frequent commits.
They retained the server's all-registered-badges-ready requirement, then added
a shared five-second demo countdown on frontend and badges, showing roles while
gameplay is inactive. Backend changes are limited to the countdown schedule and
server timestamp for dashboard clock alignment; readiness policy is unchanged.

- Gateway command overflow no longer pins the live socket mailbox. Missing
  targets receive three attempts over six seconds, then remain in the bounded
  repair cache. Server replay preserves real host receipts without rearming
  absent targets. Returning badges request state; lost repair requests retry.
- START yields to queued registrations' first HTTP attempts. Rejected controls
  display specific registration/roster/round reasons. Unsupported endpoints
  stop automatic retries instead of repeatedly interrupting the live socket.
- Host reset preserves queued peer cleanup commands and allows later remote
  reset retries through the retired-round guard. Previously resetting the host
  first could leave the other badges in their old game.
- A single scheduled start time drives the five-second countdown. Role displays
  remain visible; timers and tag controls wait for zero, including queued
  button presses and radio tag requests captured before the start.
- Firmware build ID is `LIVEG011`. Change `START_COUNTDOWN_MS` in
  `backend/src/gateway.ts` from `5_000` to `60_000` for the full-game countdown.

Live read-only evidence from this task supersedes the older deployment inference
below: GET `/gateway/control` returns `401 {"v":1,"code":"UNAUTHORIZED"}`, and
public population state is an empty lobby with no connected host. The route is
recognized by the running backend. The exact earlier B: RETRY rejection was not
captured; no live start/reset request was issued to reproduce it. Evidence files
are ignored under `.orchestration/live-control-get.json` and `live-state.json`.

No deployment, flashing, serial access, hardware tests, simulations or automated
test suites were performed. Firmware compilation, backend typechecking and
frontend production compilation validate source only. The prior unused
`self_install` warning and four prior frontend lint errors remain. Keep private
build headers and binary artifacts out of Git. Do not push integration history.

---

# Historical LIVEG010 packaged — winner display, faster catch-up, and host buttons

## Current checkpoint — 2026-09-20

User reported 3/3 zombies with timers continuing, slow LIVE SYNC, simultaneous
registration failures, and asked about recovery after moving out of range. They
then explicitly requested committing all work and merging into local `main`
**after** adding host start/reset buttons and clear zombie infection instructions.
Implementation and all prior pending source changes were committed locally as
`38c93bb` (Complete live badge gateway, game results, and host controls), then
fast-forwarded from `firmware/integration` into local `main`. The user subsequently
requested updating this handoff and pushing `main`.

Push preparation found embedded Wi-Fi/mesh credentials in unpublished historical
commits, despite no private values remaining in the current tracked source. The
original history is retained **locally only** on `firmware/integration`. Main's
publication therefore uses one clean commit of the final source and this handoff
on top of the fetched `origin/main`, preserving remote history without a force
push. Do not push the old integration history or bulk-push all branches/tags.
`git log main` and `origin/main` identify the final publication commit.

No additional firmware/frontend/backend implementation or build was needed for
this handoff-and-push step. LIVEG010 remains the packaged badge release below.
The repository's Backend CI/CD workflow uses `workflow_dispatch` only, so this
Git push does not itself trigger that workflow's tests or Cloudflare deployment.

### Live evidence and deployment boundary

A read-only GET of the public game state is saved at
`.orchestration/liveg010-server-state.json`. It showed round `674874ee6bcd489c`,
3 registered players, 3 canonical zombies, 2 accepted infection events,
0 pending/rejected events, phase `running`, and `game_over:false` with no
winner/ended_at fields. This matches the older deployed implementation.

The local LIVEG007 winner implementation already stops play on canonical
all-infected or on the time limit, freezes ended_at, sends END_ROUND and later
FINAL_RESULT after complete closure/frontiers. The new dashboard prominently
shows GAME OVER + ZOMBIES WIN/HUMANS WIN and a frozen final timer. Deploying the
prepared Worker can reconcile the existing all-zombie round via recoverAlarm;
we have not deployed, pressed Reset, or pressed Start.

Production deployment still requires explicit approval under the prior automatic
approval review rejection for `https://htn2026-backend.amitoj.workers.dev`.
The user's bug reports and Git commit/merge/push requests have not been treated as
explicit live-service deployment approval. Do not retry/bypass that gate. Existing deployed
Worker remains `a8bc8ecf-522b-4bec-a932-be34e58d816b` per the prior deployment record.
The prepared backend/dashboard and LIVEG010 firmware are both needed for the
complete features below. Deployment uses installed Wrangler `deploy --keep-vars`
from backend with WRANGLER_LOG_PATH in `.orchestration`, after approval.

### LIVEG010 changes

- Intermediate badges now relay valid JOIN and snapshot requests. Previously,
  non-host game ingress rejected them, stopping multi-hop forwarding.
- Removed host-wide 250 ms JOIN/snapshot drops while retaining per-badge limits.
  JOIN retries are jittered; queue backpressure retries sooner. Missing first
  snapshots retry after 1–1.5 seconds, with matching mesh/host limits; established
  round repair remains slower. Fresher revision/phase hints and returning host
  contact trigger catch-up.
- Preserved frozen PREPARE in the host command cache, made matching running/end
  PREPARE idempotently re-arm relay, and accept newer roster snapshot revisions
  only when their frozen hash/count/self identity still match. Backend seeds
  PREPARE once per reconnected host session, with retries until host acknowledgment.
- Inventory repair now retries unresolved digest differences and exchanges local
  inventory even when one side starts empty. Same-boot event journals and actual
  receipts/decisions remain authoritative. Reboot intentionally discards local
  game/evidence under LIVEG005; unuploaded events do not survive a power cycle.
- Gateway caches JOIN responses, uses per-request retries, services live WSS while
  an individual JOIN backs off, and processes at most four registration HTTPS
  operations before reconnecting live sync. It reuses validated bootstrap paths.
  JOIN/ACK capacities cover the roster; receipts are batched, event uploads remain
  fair, and END/FINAL commands get queue priority.
- One application request waits for its reply or a one-second fallback timeout.
  Active catch-up is paced at 100 ms; idle polling stays at two seconds. Backend
  replies to every normal ACK (empty commands when idle), batches at most four
  commands, prioritizes terminal results, omits superseded ROLE_SET revisions,
  and applies a one-second command retry cooldown. These are scheduling limits,
  not measured hardware latency claims.
- **Host lobby:** A: REGISTER, then B: START GAME. **Host authoritative winner:**
  B: RESET GAME (does not wait forever for FINAL evidence). Players press A again
  after reset; the host presses B to start the next round. During active play B
  still toggles list/radar. START/Home retain their status-screen behavior.
- **Zombies:** A: INFECT is visible on radar/list with the in-range human
  requirement. Host controls show starting/resetting and retryable failures.
- `/gateway/control` is authenticated, button-triggered HTTPS, serialized with
  other TLS operations. Requests include stable request_id, captured
  registration_id, action and expected round. Durable responses commit atomically
  with accepted start/reset (bounded 512-result history). Old requests cannot act
  on a later registration. Start accepts current WSS or authenticated host
  activity within 25 seconds to accommodate closing WSS for single-TLS HTTPS;
  normal dashboard readiness remains unchanged. Reset requires the named ended
  round. Code was compiled; no control request was sent by the agent.
- Existing Wi-Fi and mesh literals moved unchanged from app_main.c to ignored
  `private/zt_device_private.h`; the gateway token stays in its existing ignored
  header. No private values were printed. CMake requires both headers and the
  packager records hashes/freshness only. Template: `docs/device-private.example.h`.
- All LIVEG009 immediate reset/archived cleanup, strict live-sync status labels,
  LIVEG007 winner/LED behavior and the user-selected fresh-boot policy remain.

### Production build and release

Firmware ID **LIVEG010**, ESP-IDF 5.5.3 production build passed; only the existing
unused self_install warning remains. The first sandboxed CMake regeneration was
blocked from reading the process tree; the authorized build then succeeded with
that sandbox restriction lifted. No hardware access occurred.

Backend TypeScript compilation, frontend production build, Worker offline dry-run
bundle and guarded development packaging all passed. Logs:
`.orchestration/build-liveg010.log`, `build-liveg010-typescript.log`,
`build-liveg010-frontend.log`, and `build-liveg010-worker.log`.
Prepared Worker bundle: `.orchestration/worker-liveg010/`.
Frontend assets: `index-DjXGzp0J.js` and `index-DaxSbUFn.css`.

Latest badge_ops release: `.orchestration/releases/dev-20260920T093003.978295Z/release-manifest.json`.
App size: 1,317,472 bytes. SHA-256:
`0cf946a7b46504dffa73e867db3013b33a7a371fb6f9fb710dd7d438cdca7b53`.

User flashes **all badges** through `.venv-badge/bin/python tools/badge_ops.py`
then badge → `2. flash`. Host AUX1 on at boot; press A on each badge to register.
Never substitute ESP-IDF full-flash commands for the guarded workflow.

### Persistent restrictions

No tests, reviews, harnesses, simulations, serial access, flashing or hardware
operations were performed. Those restrictions remain active. Source/build success
is not hardware verification. Offline cleanup remains best effort; an unregistered
host without a recorded prior handshake cannot be identified retrospectively for
lobby cleanup. Keep credentials/private files and generated builds out of Git.

---

# Historical LIVEG009 checkpoint — superseded by LIVEG010

## Current checkpoint — 2026-09-20

User asked this new agent to continue from HANDOFF.md. The interrupted LIVEG009
work is now completed and packaged. Preserve the existing dirty tree; no commits
or pushes were made. Repository remains `htn2026`, branch `firmware/integration`.

### Completed changes

- Gateway activity enum/status fields now match the partial game diagnostics.
  HTTP activity and success timestamps are guarded; success requires decoded
  registration or validated bootstrap identity/path. Strict fresh, welcomed WSS
  connection semantics and mesh server-status flags are unchanged.
- UI uses LIVE SYNC ONLINE / LIVE SYNC WAIT. Host details distinguish REGISTERING
  BADGE, SYNCING WITH SERVER, SERVER READY, recent-HTTP SERVER RECONNECTING and
  existing specific hotspot/backend failures. YOU ARE HOST, winners and role LEDs
  remain intact.
- Definite reset defect: archived cleanup previously sent ONLY the host RESET
  and closed after its receipt. It never delivered peers' reset commands.
  Backend now sends up to four peer RESETs per frame, offers each peer once,
  retries within a persisted 15-second window, then resets the host. Server
  population/registrations still clear immediately without waiting for badges.
- Pre-round lobby reset was also missing because it had no round identity.
  Backend creates a synthetic cleanup round and binds reset commands to the
  badge MAC and successful registration idempotency key. Hello optionally sends
  `registration_id`; cleanup can recover before the first round snapshot.
  Unregistered hosts can recover using a previously recorded boot identity.
  Finished cleanup is marked so it cannot loop on reconnect. Retired registration
  requests cannot silently repopulate the server after reset.
- Gateway encodes optional reset identity as 14 wire bytes: MAC[6] plus LE64
  registration ID. Legacy zero-argument named-round resets remain accepted.
  Badges apply identity-bound RESET only to their current registration. The
  host forwards remote cleanup without requiring a local frozen roster or its
  own registration, leaving retries to the backend instead of filling the
  eight gameplay command slots. Registration HTTPS is deferred during cleanup.
- Mesh retains only bounded cleanup MAC/slot/command-sequence identities after
  clearing gameplay state. Matching reset receipts and RESET floods continue
  through cleared intermediaries, and queued RESET frames survive round purge.
  Host authentication, HMAC and receipt identity checks remain required.
- Updated `docs/backend-contract.md` and `docs/live-backend.md`.

### Build and package

- Firmware build ID: **LIVEG009**.
- ESP-IDF production build passed. Only the pre-existing unused `self_install`
  warning remains. Log: `.orchestration/build-liveg009.log`.
- Backend TypeScript compilation passed:
  `.orchestration/build-liveg009-typescript.log`.
- Worker offline dry-run bundle passed, using existing built frontend assets:
  `.orchestration/build-liveg009-worker.log`; bundle in
  `.orchestration/worker-liveg009/`. No deployment occurred.
- Guarded offline development packaging passed. Latest badge_ops release:
  `.orchestration/releases/dev-20260920T090510.098526Z/release-manifest.json`.
- Application size: 1,309,824 bytes. SHA-256:
  `ac77166a8e72d0201dd41435eb7af60e9e3399dead3105a7754e840b48899954`.
- User manually flashes all badges with `.venv-badge/bin/python tools/badge_ops.py`
  then selects badge and `2. flash`. Do not use ESP-IDF full-flash instructions.
  Host AUX1 remains on at boot. Full reset propagation needs both LIVEG009 badges
  and the pending backend deployment.

### Remaining boundaries and next action

The user's **no tests or reviews** restriction remains active. No tests, reviews,
harnesses, simulations, serial access, flashing, hardware operations or server
mutations were performed. Source/build success does not establish actual radio
or hardware behavior. Offline badges can still miss best-effort cleanup; reboot
clears their game state under the existing user-selected policy. An unregistered
host that has never completed a WSS hello has no recorded boot identity and
cannot be identified retrospectively for a lobby cleanup.

Backend/frontend updates remain local. Earlier automatic approval review rejected
production deployment pending explicit user approval for the exact live service:
`https://htn2026-backend.amitoj.workers.dev`. That gate is still pending; no retry
or bypass occurred. Existing deployed Worker is still
`a8bc8ecf-522b-4bec-a932-be34e58d816b`. Once explicit approval arrives, deploy the
prepared Worker/assets with installed Wrangler `deploy --keep-vars` from backend,
setting WRANGLER_LOG_PATH inside `.orchestration`. Do not run Reset or Start for
the user. No credentials/private files were changed or printed.

---

## Last completed release — LIVEG008

Repository: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026`.
Branch: `firmware/integration`, HEAD `8b92379aae48fc986847655dc72a882e277f842b`.
Keep the existing dirty tree intact. Do not print the full `firmware/main/app_main.c`
or private headers/configuration: they contain credentials. Do not change or expose
Wi-Fi credentials, mesh key, host token or ignored private files.

## Latest release — LIVEG008 registration fixes

User flashed LIVEG007 and reports host stays REGISTERING/SERVER OFFLINE although
phone hotspot lists the host as connected. Website shows one client, not host.
Read-only GET of the public population-state endpoint confirmed lobby, null round,
one client B1640, no saved host registration and no active host socket. Saved at
`.orchestration/registration-liveg007-state.json`. No server mutation occurred.
Wi-Fi hotspot association is separate from an authenticated backend connection.
The old deployed registration response contract matches LIVEG007; this registration
problem does not depend on the still-unapproved winner/frontend deployment.

Completed source:
- JOIN queue service rotates rather than repeatedly selecting low-index client
  requests ahead of the host. Already answered registration responses can still
  reach game state while network retry backoff defers new HTTP.
- Malformed registration HTTP 400/422 and invalid local request encoding produce
  per-badge BAD_CONFIGURATION instead of permanently stopping the shared gateway.
  Bootstrap/route/protocol failures and bearer auth rejection remain protected.
- Terminal BAD_CONFIGURATION, REGISTRATION_CLOSED and ROOM_FULL stop that badge's
  repeated JOIN nonce. Successful registration semantics are unchanged.
- Host lobby now shows hotspot/backend progress or failure details. Host Status
  page 3/4 exposes HTTP status, gateway error, IP readiness and gateway initialized.
  No credentials or network identifiers appear in these diagnostics.
- Build ID LIVEG008. Preserve LIVEG007 winner/LED functionality and LIVEG006 host
  identity label. These definite defects could cause the report, but no serial/
  hardware observation establishes that they are its sole cause.

Production ESP-IDF build and guarded offline packaging passed. Log:
`.orchestration/build-liveg008.log`; only pre-existing unused self_install warning.
Newest badge_ops release is LIVEG008:
`.orchestration/releases/dev-20260920T085226.420477Z/release-manifest.json`.
App: 1,306,528 bytes; SHA-256:
`1280b334bd2758a18fb8c787b21dc8739cf20d35be696827c948370b401a11eb`.
Flash host through the usual menu for gateway fixes and diagnostics; clients also
receive terminal JOIN retry handling with this build. If still stuck, the new host
lobby detail and Status page 3/4 HTTP/error fields provide the next evidence.
No tests, reviews, harnesses, simulations, serial access or flashing. The earlier
backend/frontend deployment approval is still pending; do not retry that rejected
deployment without explicit user approval.

## LIVEG007 completed implementation — winner screens and visible roles

User requests: stop the round and show ZOMBIES WIN on every badge once all
registered players are canonically infected; on time limit with survivors show
HUMANS WIN on all badges. Passive zombies red, humans blue, proximity danger
yellow. Also frontend heading must be large N SURVIVORS with a readable smaller
of X. User's mesh/tagging questions were answered from the current implementation:
ESP-NOW broadcast transport, application-addressed authenticated request/reply
without per-badge pairing, direct-only proximity, relayed game evidence; random
backend patient zero chosen once per new round (not hardcoded host).

Implemented source (firmware packaged; backend/frontend deployment blocked):
- Backend durable end alarm and canonical all-infected early end; immediate
  provisional winner + stopped timer. Per-badge round_closed frontiers keep valid
  offline pre-cutoff tags admissible. FINAL is emitted only after every badge has
  closed and all produced events are decided. Terminal snapshots carry result
  fields for reconnect recovery, while scheduled end_time_ms remains unchanged.
- Gateway END_ROUND/FINAL_RESULT decode and radio-command bridge are now wired.
- Badge terminal result handling freezes remaining time and carries end/result
  through host relay and snapshots. FINAL waits for durable local closure; stale
  END cannot downgrade FINAL, and relayed HOST_STATE preserves the frozen clock.
- LED role pattern fills six LEDs red/blue under the existing brightness cap;
  1–4 yellow proximity pixels pulse faster as distance closes, keeping the bottom
  pair in the role color. Brief infection/tag feedback stays.
- End screen winner is the headline immediately, with SYNCING RESULT until final.
- Dashboard timer freezes at actual ended_at and displays winner. Roster headline
  is N SURVIVORS (or N REGISTERED before start), with of X at 24–34px below.

ESP-IDF production build, guarded offline development packaging, backend
TypeScript compilation, frontend production build and Worker dry-run bundling
passed. Only the pre-existing unused self_install warning remains. No tests,
reviews, harnesses, simulations, serial access or hardware actions.
Previous badge_ops release is LIVEG007:
`.orchestration/releases/dev-20260920T083715.664323Z/release-manifest.json`.
App: 1,305,520 bytes; SHA-256:
`5ca12279827075f174a84307fc8c9a40f3dadd547eb2493d1bc9df90c732746d`.
Logs: `.orchestration/build-liveg007.log`, `build-liveg007-frontend.log`, and
`build-liveg007-worker.log` in the same directory. All badges need LIVEG007 for
the new result/LED behavior. Use the usual guarded badge_ops → badge → 2. flash.

Deployment was rejected by automatic approval review before execution, stating
that the user must explicitly approve the exact external Cloudflare destination
and live-service update. An asynchronous approval request is pending for
`https://htn2026-backend.amitoj.workers.dev`. No deployment, reset or Start occurred.
On approval, run installed Wrangler deploy --keep-vars from backend with
WRANGLER_LOG_PATH pointing into .orchestration. Do not retry until approval arrives.
Frontend assets ready: `index-tLS0_hHS.js` / `index-i5Ghz9KL.css`.
Prior deployed Worker is still `a8bc8ecf-522b-4bec-a932-be34e58d816b`.


## Latest change — LIVEG006 host label

User requested replacing HOST OFFLINE on the host itself with YOU ARE HOST.
The shared link renderer now shows **YOU ARE HOST** in green when both the
boot AUX selection and designated-host configuration are true. Other badges
still show HOST ONLINE/OFFLINE; SERVER ONLINE/OFFLINE remains independent.
Applies to lobby, prepared and radar pages. No protocol or gameplay changes.
LIVEG005 fresh-boot and registration behavior remains unchanged.

Production build and offline development packaging passed. Newest badge_ops
release: `.orchestration/releases/dev-20260920T082346.877903Z/release-manifest.json`.
App: 1,302,592 bytes; SHA-256:
`b9120e0176c8f94dfd15a7432b463ae15b97777ed52e0d3ae9e5cbb488bc15e5`.
Build log: `.orchestration/build-liveg006.log`; only existing unused self_install
warning. Host can be flashed through the usual badge_ops → badge → 2. flash menu.
No tests, reviews, serial access, flashing, backend changes or server mutations.
Power follow-up below is still pending for other badges; this UI label change
makes no claim to fix battery-related resets.

## LIVEG005 power follow-up

User reported host and other badges repeatedly rebooting independently on LIVEG005.
They confirmed **all badges were on AA batteries**. They then moved the **host to
USB and confirmed it stays online**. They are trying brand-new AA batteries in the
other badges; that result is still pending. This strongly points to power sag for
the host, but no reset-reason capture exists and the other badges remain unconfirmed.

No firmware change was needed for this power observation; the subsequent
LIVEG006 label-only release is recorded above. Focused implementation diagnosis found no demonstrated game/gateway/
radio fault on the registration/beacon paths. Compiled panic policy is HALT;
the SDK brownout path directly reboots independently of that policy. Actual main
allocations are 8192-byte game/radio stacks and 12288-byte gateway stack.
No runtime flags, RF power, credentials, backend or server state were changed.
A temporary reset-reason field assignment was removed, returning game.c exactly
to packaged LIVEG005 contents; no incomplete diagnostic changes remain. No tests,
serial access, flashing or hardware operations were performed by the agent.

## Latest report and implementation

User now confirms **LIVEG004** on the designated host with AUX on: it connects,
shows WAITING FOR SERVER SYNC, then WAITING FOR HOST SYNC, then reboots; frontend
still empty. No reset/panic output exists. The prior source-only LIVEG005 work has
now been completed, but clearing state does not prove the MCU reset cause. No
concrete later overwrite of the boot-latched host flag was found in the necessary
implementation reads; the changing sync label remains unexplained.

- Store open erases only named round/decision blobs, event keys and reset receipt,
  then loads retained configuration. Installation metadata and credentials remain.
  This intentionally discards unsent local tags after every reboot.
- Game startup enters a fresh lobby and no longer restores old mesh/checkpoint
  state. Snapshot/command admission, snapshot requests and queued clock samples
  require an A-triggered registration accepted in this boot.
- Gateway remains connected before A, forwarding registration requests and
  transport time/status traffic. Game payloads, commands, round-clock application,
  event uploads and snapshot recovery wait for accepted host registration.
- Registration remains set during normal in-session network reconnects. Remote
  badges can still submit JOIN through a host that has not yet pressed A itself.
- Main now waits for game/storage boot completion before radio initialization,
  replacing the fixed 200 ms delay that could race the new boot cleanup.
- Build ID is LIVEG005. No backend/frontend changes or server mutations were made
  during this continuation. A connection alone never registers a badge: press A
  on each badge, including host, before expecting registrations on the dashboard.

## Previous LIVEG005 build and release

Production ESP-IDF 5.5.3 build and guarded offline development packaging passed.
Build log: `.orchestration/build-liveg005.log`. Only the existing unused
`self_install` warning remains. Previous release:
`.orchestration/releases/dev-20260920T081404.897310Z/release-manifest.json`.
App size: 1,302,544 bytes; SHA-256:
`586f089bf31f7171fd11b19c0923e5d27b301f329e2d0a50203894a90c04ede1`.
Both badges should use LIVEG005. Host AUX1 must be on at boot; press A on each
badge, including the host. Dashboard Start follows successful registration.

User's **no reviews or tests** instruction remains active. No harnesses,
simulations, serial access, flashing or hardware validation. Only necessary
implementation reads/edits, production build and offline packaging occurred.
Packaging used `.venv-badge/bin/python tools/badge_dev_release.py --allow-dirty`.
User flashes with `.venv-badge/bin/python tools/badge_ops.py` → badge → `2. flash`.
Do not use full-flash commands printed by ESP-IDF.

Backend/dashboard remain deployed as Worker
`a8bc8ecf-522b-4bec-a932-be34e58d816b` with immediate server reset. The previous
stuck reset was completed in the prior session; no server state was read or
changed during this continuation. Server saved state is separate from badge boot
state and is cleared with dashboard Reset Game.

---

# Historical interrupted work — fresh game state on reboot, LIVEG005

User reports LIVEG003 host repeatedly showing REJOINING and rebooting, and now
explicitly wants game state forgotten on every reboot. This supersedes prior
cross-reboot checkpoint/journal/reset-receipt recovery requirements. No panic log
was captured; the underlying MCU reset cause is not established by this change.

The unbuilt LIVEG005 source clears only named game-state NVS keys at store open: both round and
decision blobs, the bounded event journal, and the reset receipt. Provisioning,
Wi-Fi/gateway configuration and installation metadata are retained. No whole-NVS
erase, reinstall, serial access or flash operation is performed by this work.
Game startup enters a fresh lobby; server/mesh admission waits for A registration
in the current boot, preventing an old server snapshot from silently rejoining it.
Unsent local tags are intentionally discarded by the next reboot, per the user's
new game-state policy. Backend records still require the dashboard Reset Game.

At that earlier handoff, implementation/build/package were stopped. The current
continuation and release status are recorded at the top of this file.

---

# Latest follow-up — immediate reset, frontend merge, setup wording, 2026-09-20

User explicitly changed reset semantics: **server reset must complete even when
badges are offline or forgot the session**. This supersedes the wait-for-all-badges
behavior described below. `/reset-game` now atomically archives the round and
clears active registrations/population immediately, preserving monotonic counters.
Archived old-round host hello uses a separate cleanup session (`resetting:true`,
zero snapshot metadata), prioritized host RESET, and archived-only receipts. It
does not restore the old game or block the new lobby. Remote offline badges may
still retain local state; server reset does not assert their flash was cleared.

Before deployment the current stuck reset was read from the public state endpoint:
round `27104e40237346ef`, phase `resetting`, 2 registrations, 0 reset acknowledgments.
User authorized finishing this stuck reset. Do not clear a different new round.

User also requested merging latest `backend-comms` and its new frontend here.
Fetched `origin/backend-comms` tip `11e06df2acc135a831ea4417087d58886a2a6c14` and merged
into `firmware/integration`, merge commit `8b92379aae48fc986847655dc72a882e277f842b`.
Only three overlapping frontend files were stashed; firmware/backend/private work
stayed in place. Backup stash `c0a56bd951b7ce648ad74b4e1c7f253585679247` is retained.
Local frontend integrations preserve the incoming design and real live-game state,
immediate Reset, and preparation/start gates. Incoming demo controls/placeholder
roster names must not replace actual badge state. Nothing was pushed.

The “CONFIGURE OVER USB” report was traced to unconditional setup-screen text:
REJOINING/RECOVERING/BOOT shared the setup renderer with actual NEEDS_CONFIG.
An existing PREPARED/RUNNING checkpoint legitimately enters REJOINING with valid
config. LIVEG004 changes only those labels plus build ID: USB configuration is
shown only for NEEDS_CONFIG; rejoining names host/server sync. Successful durable
reset already enters LOBBY. No new hardware diagnosis or recovery logic change.

LIVEG004 compiled and packaged as
`.orchestration/releases/dev-20260920T080138.020470Z/release-manifest.json`.
Use the guarded badge_ops menu; this is now the newest firmware release.

Latest user instruction: **no reviews or tests; leave those to the user and finish
ASAP**. Stop source reviews as well as tests/harnesses/simulations. Only necessary
implementation, production builds, packaging, merge/deploy, and the requested reset
remain authorized here. Do not access serial, flash or validate hardware.
Merged frontend production build passed; conflicts were resolved and Git conflict
bookkeeping cleared. Local live-game changes remain unstaged; the incoming branch
merge is committed, with no push. Dashboard/backend deployment completed:
Worker `a8bc8ecf-522b-4bec-a932-be34e58d816b`, frontend assets
`index-BZ3zpVg6.js` / `index-DiL3DPgE.css`, same workers.dev origin.

The user's existing stuck reset was completed through POST `/reset-game`, guarded
by matching the pending round above. Response: `gateway_phase=lobby`,
`round_id=null`, registered/total/infected counts all 0. Result saved at
`.orchestration/immediate-reset-result.json`. No new game was started.
The initial Python HTTP GET returned 403 without sending any reset; curl performed
the guarded operation successfully. No tests or further reviews were performed
after the user's stop instruction; only required builds/package/deploy/operator
reset and merge bookkeeping. Nothing was flashed and no serial port was opened.

---

# Latest continuation — LIVEG003 tag synchronization and operator reset, 2026-09-20

This section supersedes the LIVEG002 status below. User reports local tags without
dashboard updates, repeated TAG CONFIRMED animation, and client SERVER OFFLINE.
They requested cumulative filled proximity rings, a dashboard Reset Game button,
and small firmware-version text on home/game pages. Fresh AA batteries had already
resolved the client's reboot loop; host GW reconnects was only 1 and periodic RX
age returning to zero was normal heartbeat traffic.

## Implemented

- Gateway uploads active-round immutable journal events in bounded retry batches;
  explicit durable server receipts advance custody. Backend persists/adjudicates
  causal evidence and returns decisions, missing-parent requests, and targeted
  canonical ROLE_SET commands. Host relays durable decisions to victims, whose own
  persisted final frontiers produce decision acknowledgments. Duplicate decisions
  avoid repeated flash writes. Dashboard shows current roles/counts and event status.
- Tag confirmation is latched per outbound attempt. Radar now fills concentric
  proximity bands cumulatively instead of showing an invented directional dot.
  Clients receive the designated host's fresh backend-link status through BEACON
  and HOST_STATE, with expiry and replay checks. Both badges need the new firmware.
  Every screen now has a small LIVEG003 build label at the bottom right, including
  home/lobby, radar, peer list, status and end pages; footer text reserves its space.
- Dashboard Reset Game confirms clearing pending local tags and registrations.
  Uploaded evidence stays archived. In an active round, targeted RESET_GAME commands
  clear all remote badges first, then the host; progress remains visible while any
  badge is absent. Lobby-only reset clears saved registrations immediately.
  The Durable Object is retained; command and snapshot counters remain monotonic.
- Badge reset commits a separate CRC-protected NVS tombstone before named-round
  journal/decision/checkpoint cleanup. Recovery completes interrupted cleanup and
  replays the reset receipt. A durable admission epoch prevents older retained
  checkpoints from becoming active after reset. Config/unrelated retained evidence
  are preserved. Queued old-round work cannot resurrect cleared state.
- Reset has command-queue priority, permits a registered badge without an admitted
  round checkpoint, and keeps snapshot recovery available until the host clears.
  Matching old START retries preserve roles from newer infection snapshots.

## Deployment and validation

Backend/frontend deployed successfully to the existing origin. Current Worker:
`b1285af6-b514-43e6-b8a6-aa701bc46969`; assets
`index-sJNQS4dI.js` / `index-CpTM57hP.css`. No live reset/Start was invoked.
Backend TypeScript and frontend production build passed, as did Worker dry-run
bundling. Final ESP-IDF firmware build and guarded development packaging passed.
Release: `.orchestration/releases/dev-20260920T074245.071942Z/release-manifest.json`.
App: 1,303,648 bytes; SHA-256
`22b3600ff82d0dec61ef5c36e680d64251aa99bc71fc8189be76b834ebb2eeae`.
Build logs: `.orchestration/build-liveg003-final.log`; the earlier full build only
warned about the pre-existing unused `self_install` function.
Run `.venv-badge/bin/python tools/badge_ops.py`, select a badge, then `2. flash`.
The menu selects this newest release automatically; repeat for both badges.
No tests, harnesses, simulations, serial access, flashing or hardware validation
were performed. The user's restriction against those activities remains active.

Remaining scope: normal final scoring/round finalization and archived-round late
ingestion are unfinished. Gateway uploads only the active round; other retained
evidence stays local. Reset waits for every frozen badge; LIVEG002 cannot acknowledge
it. Flash both badges through the guarded operator menu before using active reset.
Do not downgrade firmware after using the new reset tombstone without reviewing
storage compatibility. Core recovery tooling and private credentials were unchanged.

---

# Runtime follow-up — client reset report, 2026-09-20

After the LIVEG002 delivery the user reported dashboard `2 awaiting`, server RX
age resetting about every 10–15 seconds, and the client rebooting while showing
host online. User confirms **host USB / client AA**. They have not separately
confirmed the displayed client build ID or supplied crash/reset output.

A read-only public population-state GET at about 07:09:16 UTC returned two saved
registrations, lobby phase, `round_id=null`, zero frozen/prepared/Start-applied,
no scheduled Start/Patient Zero, and a host message about four seconds earlier.
These counts establish stored registration, not live client presence. No reset,
Start, or other backend mutation was performed during diagnosis. The user's
earlier statement that Start was pressed does not establish a successful round
preparation; the current backend read is authoritative for current stored state.

RX age measures time since inbound traffic and should reset on the configured
ten-second WebSocket ping/pong cycle. Use gateway reconnect count and system
uptime to distinguish connection retries and MCU resets from ordinary RX age.

Bounded source/disassembly review found no definite new crash in channel/mesh,
game snapshot/PREPARE or storage paths. Current game/radio stacks are 8192 bytes;
the prior 6144-byte stack overflow is historical, not a diagnosis of this report.
The user subsequently reported that **fresh AA batteries made the client
stable**. This strongly supports weak batteries as the cause of that client loop;
no reset-reason output was captured. They continue to observe host RX age returning
to zero, which is expected heartbeat behavior. No actual host uptime reset or
rising reconnect counter has been reported in this follow-up. Do not access serial
or run tests/hardware validation without changed authorization.

No firmware changes or new firmware release were made for this report. Dashboard
wording now says saved registrations, roles unassigned and online badges unknown;
pre-start green human counts/bars are removed. Frontend build passed. Automatic
approval review initially required fresh approval; the user explicitly approved
this dashboard update and deployment succeeded. Latest Worker version:
`e3fa866a-a23e-4d01-a1ee-7c710e8049a8` at the same deployed origin. Firmware remains
LIVEG002. No game records were reset or changed by this follow-up.

---

# Latest continuation — LIVEG002 recovery fixes, 2026-09-20

This section supersedes the status below. Source fixes are built, packaged and
deployed, but **host reboot cause and end-to-end hardware behavior remain
unconfirmed**. No reset/panic output was provided and no hardware was accessed.

The user clarified that Start Game **was pressed** and both badges showed zero
roster before and after. The host is currently powered by USB; power at the time
of earlier resets is still unknown.

## Completed source fixes

- WebSocket external transport now explicitly propagates control frames. Missing
  propagation consumed PONG below the client, causing healthy ping exchanges to
  time out. The strict upgrade wrapper also forwards socket/error hooks; its
  missing socket previously broke graceful close polling. Neither issue alone
  proves the reset cause. Teardown review did not find a double-free.
- Replaced upgrade-header `sscanf` (960-byte parser frame in the earlier ELF)
  with bounded digit parsing. Preserved verified TLS, strict `zt.v1`, redirect
  rejection, watchdogs and durable state. No speculative stack/heap downsizing.
- RX backpressure drops an entire extra message without tearing down TLS.
  Periodic empty ACKs with null round recover missed first snapshots; only empty
  arrays are allowed by the codec/backend. Receipt replay is round-robin and
  cannot starve round/command recovery requests. Corrected close-status timing
  and guarded stale-link age against callback timestamps newer than the loop.
- Configured host lobby transmissions wait for hotspot IP. Player lobby channel
  adoption has a ten-second freshness lease and rediscovers on expiry. Frozen
  rounds keep their lock. Channel state is published coherently by the radio
  owner; stale queued contacts cannot survive a channel/generation change.
- Registered players missing the first roster request it with round zero and
  invalid slot; locked hosts permit that authenticated admission request and
  still verify membership. Waiting players retry. PREPARE waits for the actual
  lock before persistence/READY; lock requests are idempotent. Command attempts
  advance only when transport accepts submission.
- Host reachability is based on authenticated contact independently of clock
  readiness. Badge labels distinguish pending/frozen roster and server RX age.
  Dashboard distinguishes registered, frozen, prepared and Start-applied counts,
  host freshness and dashboard connectivity. It reports failed/stale HTTP/WS
  data and explicitly does not claim live per-badge radio presence.
- Sanitized heartbeat now includes build, uptime, reset reason, minimum free
  heap and gateway/WebSocket stack watermarks. Actual TLS headroom is unmeasured.

## Delivered artifacts and checks

- Firmware build ID `LIVEG002`; `firmware/build/zombie_tag.bin`, 1,290,864 bytes.
  SHA-256 `3c4f2fecb6dd1988687f25aca4d098ecd95fd9ec98bf150c3f807d890bb709cf`.
- Development package:
  `.orchestration/releases/dev-20260920T065942.146989Z/release-manifest.json`.
  Guarded `tools/badge_ops.py` → badge → `2. flash` selects this newest release.
- ESP-IDF build passed; log `.orchestration/build-live-gateway-fix.log`.
  Only the pre-existing unused `self_install` warning remains.
- Backend TypeScript check, frontend production build, Worker bundle and source
  whitespace check passed. No tests, harnesses, simulations or hardware checks.
- Backend/dashboard deployed to `https://htn2026-backend.amitoj.workers.dev/`,
  Worker version `6d0fc0fc-504a-4987-bd43-cf203a80b1bb`.
  Automatic approval initially rejected deployment for destination authorization;
  the user explicitly approved this exact service, then deployment succeeded.
- No badge flashing/serial access, backend reset, secret changes, commits or
  pushes. Prior dirty work is preserved. Current round/Patient Zero are retained.

Next manual step belongs to the user: flash **both** badges with the guarded menu,
boot ami on USB with AUX1 on, allow Wi-Fi connection, and press A if requested.
The saved round should reconcile through the repaired delivery paths; do not
erase/reset it as a workaround. If host resets recur, obtain user-captured reset
output or explicit serial authorization. Do not claim the reboot has been fixed
until there is runtime evidence.

Infection adjudication, final scoring, active-round finish/reset and a durable
whole-outbox cursor ledger remain outside this registration/initial-role build.
See `docs/live-backend.md` for operator details. Historical record follows.

---

# DEBUGGING HANDOFF — unstable live build, latest user report 2026-09-20

**Read this section first. The live implementation below compiled and was deployed,
but the user now reports significant runtime problems. It is NOT a confirmed
working end-to-end build.** The user requested this handoff for another agent to
debug and fix the problems, rather than continuing implementation in this turn.

Repository: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026`.
Preserve the dirty working tree and private configuration. No commits or pushes
were made by the implementation session.

## Latest observations — reported by the user, not independently diagnosed

1. **Host eventually begins restarting / boot cycling after running for a while.**
   This is the highest-priority regression. There is no captured reset reason,
   panic/backtrace, uptime history, heap trace or stack watermark for this failure.
2. **Server age resets roughly every 20 seconds.** The user suspects instability;
   this observation alone does not establish that the WebSocket reconnects.
3. **Frontend shows two humans connected, but both badges show `0/20`.** The exact
   badge screen/label and admission state at that moment were not recorded.
4. **Host reports channel 6; client reports channel 11.** It is unknown whether
   these readings were simultaneous, during discovery, or after channel lock.
5. **Direct peers shows 1 on both badges**, despite the reported channel mismatch.
   The user suspects the mesh is not working. Treat that as a hypothesis, not a
   proven diagnosis; a cached/transient contact does not prove a stable link.

Not yet established: whether Start Game was pressed, whether either badge reached
REGISTERED / WAITING FOR ROUND / PREPARED / RUNNING, whether the backend already has
a frozen active round, which release each badge is actually running, whether the
host was powered by USB or AA cells, and the first failure preceding the boot loop.
Do not invent these facts or silently reset the backend/badges to establish them.

## Objective for the next agent

Diagnose and fix host stability, player/host channel convergence, actual mesh
registration/snapshot/command delivery, and consistency of badge/dashboard status.
Resolve the root causes, compile the firmware, build/deploy backend/frontend if
changed, and package a fresh development release for the user's guarded CLI.
Explain any remaining limits honestly. Live backend operation remains the goal;
do not substitute the local demo or one Internet connection per ordinary badge.

**Existing user constraints still apply:** do not write/run tests, harnesses,
simulations, or hardware validation. Do not flash badges or open serial ports
unless the user explicitly changes that instruction. Build and source review are
allowed; the user performs flashing/manual verification. The latest request does
not itself authorize serial access. If reset diagnostics are needed, request
existing/user-captured output or obtain explicit permission for hardware access.
Never print/copy the hotspot password, SSID, mesh key, or private gateway token.

## Investigation pointers — hypotheses and code paths, not findings

- **Start with the host boot loop.** Review gateway/TLS allocation and teardown,
  callback/task lifetimes, shared buffers, task stacks and watchdog behavior.
  Relevant files: `firmware/main/app_main.c` and
  `firmware/components/zt_gateway/{bridge,http,ws}.c`, `gateway_internal.h`.
  The new gateway context is approximately 13 KiB; its task stack is 12,288 bytes
  plus the WebSocket client's 4,096-byte stack. Earlier free heap was only about
  48 KiB before the gateway. Display/boot cleanup reclaimed about 20 KiB, but no
  hardware measurement established sufficient TLS peak or steady-state headroom.
  Registration deliberately destroys WSS before HTTPS, then reconnects; review
  this lifecycle and the custom TLS parent used for strict upgrade validation.
  Repeated JOIN retries could contribute to socket churn; correlate them with
  registration results and reconnect counters before drawing that conclusion.
  Power/brownout was a historical issue on AA cells, but current power is unknown:
  do not assume either brownout or memory exhaustion without evidence. Do not
  disable watchdogs/TLS verification or remove persistence to conceal the problem.
- **Interpret server age correctly.** `zt_gateway_status.last_inbound_us` updates
  on inbound WebSocket frames; age going back to zero may simply mean a heartbeat
  or time reply arrived. Configured cadence: ping 10 s, pong timeout 20 s, stale
  inbound threshold 25 s, application time sync 30 s. Compare connection/welcome
  state, reconnect count, uptime and errors before calling it a reconnect loop.
  Review frame reassembly, pong handling, notification/backpressure, and teardown.
- **Trace actual radio state, not only the displayed number.** Review
  `zt_radio/channel.c`, mesh host discovery/channel adoption, and `zt_game/game.c`
  host-status/beacon handling. Preserve the prior host-switch debounce and DHCP
  rearming fixes. Distinguish current hardware channel, configured/remembered
  channel, discovery state, frozen round channel, and UI snapshot channel. Check
  contact freshness and driver-generation invalidation before trusting peers=1.
- **Separate registration, frozen roster and online presence.** Backend player
  counts persist; two humans in the dashboard is not proof both radios are live.
  Lobby bootstrap intentionally has no frozen round roster. The game obtains the
  frozen roster when Start prepares a round, so `0/20` may expose either missing
  delivery or unclear UI semantics depending on the phase. Do not blindly equate
  the badge roster count with the backend registration count, or merely change
  the display to hide a protocol failure.
- **Trace the complete admission path.** A -> JOIN -> host feed -> HTTPS
  registration -> retained result -> JOIN_RESULT -> assigned slot; then Start ->
  frozen snapshot -> durable host admission -> PREPARE -> mesh snapshot/command
  delivery -> actual per-badge PREPARED_READY -> START with fresh clock. Relevant
  files: `zt_game/game.c`, gateway bridge/codec, `zt_ui/{ui,render}.c`,
  `backend/src/gateway.ts`, `frontend/src/{App.tsx,components/PopulationState.tsx}`.
  Inspect how failures/backpressure, host self-registration, retries, snapshot
  revisions and channel locking interact. The transport uses a single immutable
  RX buffer with deliberately paced backend responses; dropped frames must not
  permanently stall admission. Do not fabricate readiness or reset patient zero.
- Existing sanitized heartbeat lines report `admission`, `ch`, `chstate`, `assoc`,
  `ip`, peers, switch/configured/host mode, RX/auth/invalid/dedupe counters, plus
  gateway `connected`, `welcomed`, `error`, `http`, `clock`, `free`, `largest`,
  `drops`, and `reconnects`. These are available for user-provided diagnostics;
  no new logs were collected in this handoff-only turn.

## Exact delivered artifacts and external state

- Firmware: `LIVEG001`, `firmware/build/zombie_tag.bin`, 1,306,704 bytes.
  Successful build log: `.orchestration/build-live-gateway.log`.
- Last packaged release:
  `.orchestration/releases/dev-20260920T063831.664350Z/release-manifest.json`.
  This includes the live gateway code, unlike the older Wi-Fi-only release below.
  The user has not supplied independent confirmation of the version on each badge.
- Backend/dashboard: `https://htn2026-backend.amitoj.workers.dev/`, game
  `005a544d454d4f01`, Worker version
  `304a3fcf-649a-4c35-b1b6-7dda4c77487f`.
- Matching private secrets are configured on the Worker and in the ignored local
  files listed below. Do not regenerate/rotate them as a speculative fix.
- Only this handoff was edited for the latest request. No new build, deployment,
  release, hardware access, reset, or diagnostic run was performed in this turn.

## Remaining scope limitations

Infection adjudication, final scoring, finish/reset of an active live round and
a durable whole-outbox cursor ledger remain unfinished. Reconnect cursor is kept
at zero conservatively; backend commands persist and badge application dedupes.
Preserve local event evidence and existing round state while fixing these runtime
issues. The next agent should not represent the initial-role adapter as completed
full-game synchronization.

---

# Previous continuation — implementation/build record, 2026-09-20

This update supersedes the unfinished implementation status below; the earlier
handoff is retained as history. See `docs/live-backend.md` for operator steps.

- Backend and dashboard deployed to `https://htn2026-backend.amitoj.workers.dev/`.
  Worker version: `304a3fcf-649a-4c35-b1b6-7dda4c77487f`.
- Private gateway credential and host identity are configured on the Worker.
  Matching local inputs are in ignored `private/gateway-secrets.json` and
  `private/zt_gateway_private.h`. Do not print or commit their contents.
- Firmware gateway HTTPS/WSS, bounded JSON, registration, snapshot admission,
  PREPARE/START, real per-badge ready/application acknowledgments, and time sync
  are implemented. Only the host connects to the Internet. Main starts SNTP and
  the gateway task after radio startup. Prior Wi-Fi fixes remain intact.
- Dashboard and firmware both use game `005a544d454d4f01`. The host must register
  too. START waits for all registered badges to persist PREPARE, chooses Patient
  Zero once, and schedules start 15 seconds ahead. Repeated Start is idempotent.
- Backend typecheck, frontend build, Worker bundle and ESP-IDF firmware build
  passed. Firmware build ID is `LIVEG001`; binary size is 1,306,704 bytes with
  53% application partition free. Package using the existing dirty-source
  development release tool; the operator menu selects the newest release.
- Display buffers and temporary boot allocations reclaim about 20 KiB. Dynamic
  TLS buffers are enabled, peer certificates are released after verification,
  TLS RX stays 16 KiB, and registration closes WSS to keep one TLS session at a
  time. Actual TLS heap headroom remains unmeasured on hardware.
- Conservative reconnect cursor stays zero; durable backend commands replay and
  badges deduplicate applied command sequences. A complete durable outbox cursor
  ledger is still not implemented.
- Infection adjudication, final scoring, round finish/reset are outside this
  implementation. Evidence stays in the durable journal; no false receipt or
  decision is generated. Active live rounds cannot be reset through the dashboard.
- No tests, harnesses, simulations, hardware validation, flashing, serial access,
  commits or pushes were performed. User performs the next manual hardware step:
  flash through `tools/badge_ops.py`, boot ami with AUX1 on, press A on each badge,
  and use Start Game in the deployed dashboard.

---

# Zombie Tag — handoff after Wi-Fi fix and backend merge

Updated 2026-09-20. Repository: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026`.
Branch: `firmware/integration`. HEAD: `bc640f50577460cd83125983fdd0ea4ebc80fade`.
This replaces the earlier handoff. Read `plan.md` and `orchestrator.md` for architecture,
but use the current user decisions and actual state below when older notes disagree.

## 1. User's objective and instructions

**Wi-Fi now works. The user manually confirmed: “it connects to wifi and then locks to
channel.” Next they want LIVE BACKEND registration and roles, not the local demo.**

- They explicitly selected live backend registration/roles over the local demo.
- They requested merging `backend-comms`; that merge is DONE.
- They provided the deployed URL: `https://htn2026-backend.amitoj.workers.dev/`.
- They do not think a backend token has been configured. There is no default token in
  the merged backend. A private shared token was proposed, but NONE has been generated,
  stored, uploaded, or compiled yet.
- **Do not write or run tests, harnesses, simulations, or hardware validation.** They
  repeatedly said they have no time for tests and will flash/manual-verify themselves.
  Compile/build the deliverable, package it for their CLI, and give concise instructions.
- Do not flash their badge or open serial ports for this next step unless asked.
- Keep existing Wi-Fi fixes and local work. Do not revert to standalone mode.
- They requested this handoff to give to a new agent. All active implementation workers
  stopped; no background edits/builds/deployments remain.

## 2. What is complete

### Wi-Fi firmware fixes (uncommitted, but built and manually confirmed by user)

`firmware/main/app_main.c`:

- Radio startup now waits up to one second for `zt_buttons_boot_host_switch()` to finish
  debouncing. Previously input polling had only just started, the call returned BUSY,
  its return was ignored, and the host radio started permanently in player mode.
  The game later latched the host switch independently. This explained channel hopping.
- The existing iPhone hotspot SSID/password were restored in `self_config()`; do not
  copy their values into notes, output or new files. iPhone Maximize Compatibility was
  already enabled and was not the root issue.
- Heartbeat now uses `zt_console_log()` rather than suppressed `ESP_LOG` output, and
  includes `switch`, `configured`, `radio_host`, `assoc`, `ip`, channel and counters.

`firmware/components/zt_radio/channel.c`:

- Calls `esp_netif_dhcpc_start()` before each association, accepting ALREADY_STARTED.
  Cancellation explicitly stops DHCP. Without rearming it, ESP-NETIF treats the next
  association as static-IP mode and retries can associate without obtaining an IP.

The earlier claim that empty SSID safely enables standalone self-configuration was
wrong: storage rejects `host_credentials_present=1` with empty SSID. A prior valid
configuration survives the rejected write; a fresh host cannot configure. Do not
reinstate that workaround.

### Backend branch merge (committed)

`git merge --no-edit backend-comms` completed cleanly, preserving local firmware edits.
Merge commit: `bc640f5`; merged backend tip: `619a440` (`modify to websockets`).
Nothing was pushed. No backend deployment was performed this session.

The merged branch adds actual device registration/state, role assignment, game start,
rankings and dashboard controls. It DOES NOT yet implement the planned host gateway
protocol; see section 5.

### Development release packaging (user explicitly authorized dirty-source bypass)

New, currently untracked: `tools/badge_dev_release.py`.

```sh
.venv-badge/bin/python tools/badge_dev_release.py --allow-dirty
.venv-badge/bin/python tools/badge_ops.py
```

The first command packages the existing built firmware, marks development/dirty source
truthfully, records source hashes without copying credential-containing diffs, and
creates a new release under `.orchestration/releases/`. Then the existing operator menu
selects the newest release on `2. flash`; restarting the menu is unnecessary.

This bypasses only the clean-source packaging requirement. Hardware identity, backups,
flash range and readback gates remain in the existing recovery tool. The regular menu's
“Generate a release” still uses the strict clean-tree builder; on a dirty checkout run
`badge_dev_release.py --allow-dirty` after building BEFORE selecting flash.

Last generated development release:
`.orchestration/releases/dev-20260920T060419.933514Z/release-manifest.json`.
That is the Wi-Fi-fixed build, BEFORE the backend merge and registration edits below.

## 3. Current working tree and build state

Expected dirty/untracked files at handoff:

```
 M HANDOFF.md
 M firmware/components/zt_contract/include/zt_game.h
 M firmware/components/zt_game/game.c
 M firmware/components/zt_radio/channel.c
 M firmware/main/app_main.c
?? tools/badge_dev_release.py
```

Only the backend merge was committed. Do not overwrite these changes.

`firmware/build/zombie_tag.bin` was successfully built at approximately 01:59 Toronto
on Sep 20, size 1,030,000 bytes (63% factory partition free). **It does not contain the
new registration changes.** Those edits happened after the last build and are uncompiled.
Do not represent the existing binary/release as the live-backend implementation.

## 4. Partial registration implementation left for you

These are the ONLY new live-backend implementation edits so far:

`firmware/components/zt_contract/include/zt_game.h` adds:

```c
ZT_GAME_FEED_JOIN
// Added body member in zt_game_feed_item_t:
struct { zt_wire_join_t request; zt_boot_nonce_t boot_nonce; } join;

zt_err_t zt_game_post_registration(
    zt_round_id_t round_id, const zt_wire_join_result_t *result);
```

`firmware/components/zt_game/game.c` has one completed, UNCOMPILED patch:

- Adds `IN_REGISTRATION` and queue-owned `zt_game_post_registration()`.
- Authenticated unknown player JOIN feeds `ZT_GAME_FEED_JOIN` instead of immediately
  refusing registration; sends WAITING_FOR_SERVER while awaiting backend response.
- Host's own A-button JOIN feeds the gateway directly; ESP-NOW does not loop its own
  broadcast back and the previous self-JOIN path could never register the host.
- Applies verified self registration results or relays remote JOIN_RESULT matching the
  pending MAC/request nonce; retains queued result if radio submission is busy.
- Preserves frozen-roster offline rejoin and existing three-second same-nonce retry.
- Handles registered/rejoined, closed, full and bad-configuration outcomes.

Read the diff before continuing. There has been no compile or review pass after this
patch. No tests should be added. The patch cannot work alone: `on_gateway_feed()` in
`app_main.c` still always returns INVALID_STATE and the entire gateway is stubbed.

## 5. Backend mismatch that must be resolved

Current merged backend:

- Worker router: `backend/src/index.ts`.
- SQLite Durable Object/game state/sockets: `backend/src/game-room.ts`.
- `/ws/device?device_id=...` auto-registers ONE device per socket and sends `type`-based
  init/role/game-over messages. `/ws/population` is the dashboard feed.
- `/start-game` currently immediately picks patient zero, and rerolls on every call.
- No `/gateway/bootstrap`, `/registrations`, `/gateway/socket`, or planned `/rounds` API.
- Unprefixed dashboard calls use game `default`. Firmware game ID is
  `005a544d454d4f01` (from `ZT_SELF_GAME_ID`). They must use the SAME game instance.

Plan/firmware architecture: ONLY the designated host uses Internet/TLS. Other badges
register through authenticated ESP-NOW JOINs. One host WSS connection carries the whole
roster. Do not quietly replace this with one Internet socket per ordinary badge.

Agreed implementation direction, NOT YET WRITTEN:

1. Preserve legacy dashboard/device endpoints and add the planned host gateway API to
   the existing `GameRoom` (no new Durable Object binding required):
   - GET `/api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}`
   - POST `/api/v1/games/{game_id}/registrations` with stable `Idempotency-Key`
   - WSS `/api/v1/games/{game_id}/gateway/socket`, subprotocol `zt.v1`
2. Add private bearer-token configuration (`ZT_GATEWAY_TOKEN` was proposed) and host
   identity (`ZT_HOST_MAC` proposed). Do not invent a pre-existing default token.
3. Persist registrations/slots, frozen roster, snapshots, command sequences/outbox and
   per-badge readiness. Dashboard start should prepare the gateway round when gateway
   registrations exist; preserve old behavior for legacy-only games.
4. Require 2–20 players, wait for actual `prepared_ready` from each roster slot, choose
   patient zero exactly once and schedule START at least ten seconds ahead.
5. Implement hello/welcome, snapshot pages, PREPARE/START commands, time_sync, ack and
   need/reconnect replay. Do not reroll patient zero or reset roles on reconnect.
6. Exclude gateway sockets from existing population broadcasts: currently
   `broadcastState()` sends legacy JSON to every socket without `is_device`.
7. Point dashboard API_BASE to `/api/v1/games/005a544d454d4f01` on the supplied origin
   (or deliberately align both ends another way). Current frontend defaults localhost
   and its requests otherwise control `default`, not the firmware game.

Scope requested for this step is live registration and initial roles. Do not fabricate
infection receipts/decisions if adjudication remains unfinished; retain evidence and
report unsupported paths honestly.

## 6. Firmware gateway work still required

All four `firmware/components/zt_gateway/{http,ws,codec,bridge}.c` remain scaffold stubs.
No `gateway_internal.h` exists. No gateway task or sinks have been wired in main.
No changes were made to gateway CMake or sdkconfig in this backend phase.

Read `zt_gateway.h`, `zt_game.h`, `docs/backend-contract.md`, `plan.md` sections 4–5,
and `packets/N01-gateway-wss.md`. Some old packet exclusions contradict its own WSS
scope; current explicit live-backend request and current plan transport take precedence.

Suggested bounded ownership split for parallel work:

- Game registration: finish/read section 4's patch.
- Backend adapter: section 5.
- Transport/bridge: HTTPS + WSS lifecycle/queues and server-to-game conversion.
- Codec/main integration: bounded JSON + task creation/configuration.

Codec helpers proposed but NOT implemented:

```c
zt_err_t zt_gateway_decode_bootstrap(const char *json, size_t len,
    zt_json_workspace_t *workspace, zt_bootstrap_t *out);
zt_err_t zt_gateway_encode_registration(const zt_registration_request_t *request,
    char *out, size_t capacity, size_t *written);
zt_err_t zt_gateway_decode_registration(const char *json, size_t len,
    zt_json_workspace_t *workspace, zt_registration_response_t *out);
```

Key integration details:

- Gateway waits for radio `has_ip`; it never owns Wi-Fi scanning/channel selection.
- Bootstrap/registration use verified HTTPS; one `zt.v1` WSS carries live traffic.
  Token only in Authorization headers, never URLs or logs. No redirects with token.
- Synchronize wall time before certificate verification; SNTP was proposed, not added.
  `provisioned_time_ms` is currently zero. Do not disable TLS verification to connect.
- Gateway task at priority 4, provisionally 12,288-byte stack; do not block game/radio.
- Callbacks copy bounded chunks/flags only. Gateway task owns JSON and WS lifecycle.
  Pinned esp_websocket_client is 1.8.0; frame offsets/FIN are per-frame, not message.
- Queue JOINs, use stable badge/boot/nonce-derived registration idempotency keys, and
  retain result until `zt_game_post_registration()` accepts it.
- Lobby bootstrap with zero round/empty roster is metadata, NOT a game snapshot:
  game snapshot assembly requires a nonzero round and valid roster/rules.
- Publish complete frozen roster with nonzero round and phase `lobby` BEFORE PREPARE,
  then wait for durable admission. Existing game PREPARE/START already handles roles.
- `zt_clock_apply()` queues safely when called outside the game task. Apply fresh time
  only after the round is admitted; START UTC becomes signed elapsed with uncertainty.
- Snapshot <=8 entries/page, commands <=4/message, total JSON <=4096 bytes.
- Canonical roster hash: entries sorted by slot, bytes `slot:u8 + MAC[6] + name_len:u8
  + name[12]` zero-padded; SHA-256 first 8 bytes interpreted little-endian, encoded hex16.
- Gateway hello needs actual channel for backend PREPARE. Optional `channel` was
  proposed; it has NOT been added to `zt_gateway_hello_t` or JSON yet.
- Extending gateway status with last_error/http_status was proposed, NOT implemented.
- `main` still contains placeholder backend URL/token and obsolete no-gateway comments.
  Wire real config privately, game sinks, feed submission and service task.

### Memory is a real constraint

A prior live status showed ~48 KiB free heap (minimum ~41 KiB) before adding gateway.
Plan's desired 64 KiB before initial TLS is not currently met. Do not allocate duplicate
4 KiB buffers, 768-token workspace, snapshots and queues casually. Consider shared
scratch, bounded streaming codec, dynamic TLS buffers, dropping retained peer cert,
and reducing LCD stripe height/duplicate application pools. None was implemented yet.
Current TLS receive record length is 16384 and transmit is 4096; preserve server record
compatibility. Do not blindly shrink RX to 4 KiB. Do not remove persistence/TLS checking
or disable watchdogs. User will perform manual hardware verification.

## 7. Build and release commands (no tests)

ESP-IDF v5.5.3 at `/Users/amitojsingh/esp/esp-idf`. System Python is now 3.14, so plain
`source export.sh` looks for a nonexistent 3.14 environment. Use the installed 3.12 env:

```sh
export IDF_PYTHON_ENV_PATH=/Users/amitojsingh/.espressif/python_env/idf5.5_py3.12_env
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
. /Users/amitojsingh/esp/esp-idf/export.sh
cd /Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/firmware
idf.py build
cd ..
.venv-badge/bin/python tools/badge_dev_release.py --allow-dirty
```

Build-generated full-flash instructions are NOT the deployment workflow. User flashes
with `.venv-badge/bin/python tools/badge_ops.py` → badge → `2. flash`.
Build/package after completing new code; do not simply repackage the old Wi-Fi binary.

Backend uses installed Wrangler 4.x (`backend/node_modules`) and `wrangler.jsonc`.
Cloudflare/DO/Wrangler skills were read, but no auth check, secret upload, deployment,
backend build, or frontend build was run this session. Deployment remains outstanding.

## 8. Existing operational facts and traps

- `ami` is designated host, MAC `288485d1af74`, AUX1 ON at boot; `ant` is the other
  enrolled badge. Both have verified individual backups and self-installed custom NVS.
- Primary archive: `~/Library/Application Support/ZombieTag/BadgeArchive`.
  Mirror: `/Volumes/badge-test/ZombieTagMirror`. Use existing registry/guarded tool;
  do not copy archives/credentials into worker context. Prior session reports USB stable
  and brownouts on AA cells when radio starts.
- Recovery revision hashes `tools/badge_tool.py`, `tools/requirements.txt`, and every
  `tools/zt_badge/*.py`. Changing those invalidates existing rehearsal acceptance.
  `badge_ops.py` and new `badge_dev_release.py` are outside that hash. Core recovery tool
  was NOT changed. Do not solve packaging by weakening recovery gates.
- Self-install enabled via `ZT_STORE_SELF_INSTALL=1` in firmware root CMake; preserved
  stock partitions, named custom NVS/tail only. Self-installed fleet cannot commission.
- `ZT_SELF_CONFIG=1` rewrites differing baked settings on lobby boot without a round.
  Account for this when changing URL/token; don't expect console values to win forever.
- Actual console configure handler is `zt_ops/ops.c:configure()` and deliberately
  returns NOT_IMPLEMENTED. The legacy `apply_configure()` in main is unreachable and
  is NOT a working provisioning path. Do not rely on CLI provision for backend setup.
- `info`/CLI diagnose currently refuses configured lobby with NOT_IMPLEMENTED. A small
  UNMERGED fix remains in `.worktrees/P01/firmware/components/zt_ops/ops.c`: validate
  phase, derive pending from nonfinal active round, remove lobby refusal. Read/apply
  only that patch if useful; it has not been merged this session.
- Earlier packet status: F00/R01/H01/M01/G01–G05/C01/C02/D01/O01/R02/R03/M02/P01 base
  merged; N01 gateway never implemented. Demo code exists but user chose live backend.
- No round/roles/mesh gameplay/backend registration has been confirmed on hardware in
  this conversation. Only Wi-Fi connection/channel stability is user-confirmed now.

## 9. Immediate next actions

1. Preserve current dirty files; read the new game registration diff.
2. Implement backend gateway API + firmware codec/HTTPS/WSS/bridge and main wiring in
   parallel, using the existing game role machinery and exact shared schema.
3. Set the supplied backend origin and create/configure a private host token; align
   dashboard and firmware game IDs. Auth/deployment setup is still required.
4. Integrate, compile (no tests), deploy backend when ready, build and create a new dev
   release. Give user the CLI flash path and manual sequence: AUX1 on for ami, press A
   on each badge, then Start Game in the matching game dashboard.
5. State any unfinished behavior honestly; do not claim end-to-end success before
   the user's manual result.
