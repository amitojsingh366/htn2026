# Backend — Cloudflare Worker + Durable Objects

Replaces the previous FastAPI app (`main.py`, in git history before this branch).

The LIVEG007 winner logic and LIVEG009–LIVEG010 reset/replay improvements described
here are local source changes. Production deployment remains pending explicit
approval; a successful local build does not update the live Worker. See
[HANDOFF.md](../HANDOFF.md) for the exact packaged and deployed versions.

## Layout

| File | Role |
|---|---|
| `src/index.ts` | Worker: routing, CORS, WebSocket pass-through. Stateless. |
| `src/game-room.ts` | `GameRoom` Durable Object: one instance per game, owns state + sockets. |
| `src/gateway.ts` | Host authentication, registration, durable infection evidence/decisions, canonical roles, command replay and coordinated reset. |
| `src/types.ts` | Shared wire types. `Env` is generated into `worker-configuration.d.ts`. |
| `wrangler.jsonc` | Bindings and the SQLite DO migration. |

The Worker holds no state. Every read and write goes to a `GameRoom` instance
resolved by `env.GAME_ROOM.getByName(gameId)`, so the same game id always lands
on the same object. State lives in that object's SQLite storage, which survives
hibernation, eviction and redeploys.

## Routing

Unprefixed paths address the game `default`, so the original endpoints are unchanged:

```
GET  /population-state     GET  /num-players      POST /add-player
GET  /health               GET  /num-infected     POST /add-infected
GET  /rankings             GET  /ws/population    POST /device-event
                                                  POST /reset-population
```

The same legacy routes are available per game:

```
POST /api/v1/games/{game_id}/device-event
GET  /api/v1/games/{game_id}/rankings
POST /api/v1/games/{game_id}/add-infected
GET  /api/v1/games/{game_id}/population-state
WS   /api/v1/games/{game_id}/ws/population
WS   /api/v1/games/{game_id}/ws/device?device_id={device_id}
```

The live badge implementation additionally uses:

```
GET  /api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}
POST /api/v1/games/{game_id}/registrations
POST /api/v1/games/{game_id}/gateway/control
WSS  /api/v1/games/{game_id}/gateway/socket  (subprotocol zt.v1)
```

Only the designated host uses these authenticated device endpoints. The dashboard
continues to use `/start-game` and `/reset-game`; firmware does not call those
public operator routes. There is no HTTP event polling endpoint. Start freezes
registration, waits for per-badge PREPARE
acknowledgments, and schedules Patient Zero once. Repeating Start is idempotent.

An explicit host-button action sends `/gateway/control` a version-1 object with
`action` (`start` or `reset`), nonzero hexadecimal `request_id` and
`registration_id`, and `round_id` (null for lobby Start, current completed round
for Reset). The host's current registration must match. Reset is allowed once
the authoritative winner is present, before final evidence synchronization.
Start preserves the minimum roster and host checks, accepting an authenticated
host session seen within 25 seconds so closing WSS for the HTTPS request does not
make the host appear unavailable. Expired readiness returns retryable HTTP 503.

Accepted replies contain `v:1`, `accepted:true`, the matching `action` and
`request_id`, resulting `round_id`, and `state_rev`. Replies commit atomically
with the mutation. The newest 512 accepted/semantic-error responses survive
reset for exact retries; registration and round checks prevent delayed old
requests from affecting a later lobby. See the
[control contract](../docs/backend-contract.md#explicit-host-controls) for details.

## WebSockets

1. **Dashboard WebSocket (`/ws/population`)**:
   Pushes the full population state object on connect and after every mutation. It uses the hibernation API (`ctx.acceptWebSocket`), so clients stay connected while the object sleeps.

2. **Legacy ESP Device WebSocket (`/ws/device?device_id={id}`)**:
   Maintains an open bidirectional socket for ESP devices:
   - On connect: Sends `{ "type": "init", "device_id": "...", "role": "not infected", "is_infected": false }`.
   - On game start: Pushes `{ "type": "role_assignment", "role": "infected" | "not infected" }` immediately.
   - Sending events: ESP sends `{ "type": "event", "current_state": "infected" }` over the socket; server replies with `{ "type": "event_ack" }`.
   - On game over: Pushes `{ "type": "game_over", "rank": N, "survival_time_seconds": S, "rankings": [...] }`.
   - Keepalive: `ping` is auto-answered with `pong` by the runtime without waking the object.

3. **Live host gateway (`/gateway/socket`)**:
   Uses the event/receipt/decision contract in `docs/backend-contract.md`. Events
   are durably stored before receipts. Conflicting duplicate bodies are retained
   separately and rejected without overwriting original evidence. Missing causal
   parents or earlier victim sequences remain pending and are requested again.
   Accepted/rejected decisions and targeted ROLE_SET commands persist until badge
   application is acknowledged; only the latest canonical role revision per target
   needs replay. The dashboard shows canonical current
   roles and received/pending/rejected event counts; registrations are not presence.

Legacy device mutation is blocked once a game has live gateway registrations.
Gateway responses are paced to one logical frame per client turn, with rotating
command, decision and missing-event replay so an offline badge cannot starve others.
Every valid ordinary ACK receives a response, including ACKs carrying application
receipts. When no work is eligible, `commands:[]` explicitly releases the gateway's
outstanding request and returns it to idle polling. Event uploads receive only
their durable receipt frame; the next ACK fetches resulting state changes.

LIVEG010 batches up to four commands per frame and prioritizes END_ROUND and
FINAL_RESULT. Superseded ROLE_SET revisions are omitted from replay while retained
in durable history. Normal command retries have a one-second minimum interval;
decision and missing-event cursor cycles also pause before repeating unchanged
work. Each host connection replays the frozen PREPARE until that session receives
the host's readiness/application receipt, restoring the relay cache used when
out-of-range clients return. Matching PREPARE replay preserves a running or
terminal round instead of restarting it.

## Winner and timer behavior

Once every registered player is canonically infected, the backend immediately
sets `game_over:true`, records `ended_at`, and emits a provisional `Z` winner and
END_ROUND. At the time limit with survivors, the provisional winner is `H`. Badges
and the dashboard can stop their clocks and display the winner before all badges
finish synchronizing. A durable alarm also closes the round at its scheduled end.
FINAL_RESULT follows once every badge has reported its final produced-event
frontier and all those events are decided. Terminal snapshots include the result
so returning badges can recover it. Pending offline evidence can change a
provisional time-limit winner before finalization.

The live report of 3/3 canonical zombies with `game_over:false` was produced by
the older deployed Worker, which did not yet contain this local winner behavior.

## Explicit reset and remaining scope

The dashboard Reset Game button asks for confirmation, then immediately archives
the old round and clears active registrations/population. Missing badges and
forgotten sessions never block completion. Command, server and snapshot counters
keep increasing, and uploaded evidence remains archived in the same Durable Object.
RESET_GAME commands remain available for best-effort archived cleanup. LIVEG009
binds them to the target MAC and registration identity when available; a synthetic
nonzero cleanup round also supports registered lobby badges. The host offers each
peer a reset and retries within a bounded 15-second window before resetting itself.
A stale host can resume cleanup without restoring the old game on the dashboard.
Offline clients can still retain their local game until they receive a reset;
server completion does not claim that their flash was cleared.
Deploying never invokes reset.

Current-round infection adjudication validates frozen membership, causal roles,
known role revisions, occurrence time/uncertainty and both RSSI observations.
Cooldown remains enforced by firmware: victim reception timestamps cannot prove
the actor's initial-send cooldown when a tag succeeds on a later radio retry.
Final scoring and archived-round late event ingestion remain unimplemented.
Retained old-round evidence must not be silently
submitted as current-round traffic or discarded outside explicit operator reset.

Payload (unchanged from the FastAPI version):

```json
{ "num_players": 4, "num_infected": 1, "num_humans": 3, "survived_pct": 75 }
```

## Commands

```bash
npm install
npm run dev         # http://localhost:8787
npm run deploy
npm run cf-typegen  # regenerate Env after editing wrangler.jsonc
npm run typecheck
```

Point the frontend at a deployed Worker with `VITE_API_BASE` in `frontend/.env`;
it defaults to the deployed game at
`https://htn2026-backend.amitoj.workers.dev/api/v1/games/005a544d454d4f01`.
The Worker serves the built frontend at its root. Build `frontend` before deploying.
The live host gateway requires `ZT_GATEWAY_TOKEN` and `ZT_HOST_MAC` secrets;
see [live backend operation](../docs/live-backend.md) for the build, flash and scope notes.

## Notes

- Current validation is TypeScript checking and builds only; the user has
  requested no tests, reviews, harnesses, simulations or hardware access.
- First deploy applies migration `v1` (`new_sqlite_classes: ["GameRoom"]`).
  Do not edit that entry afterwards — add a new tag instead.
