# Backend — Cloudflare Worker + Durable Objects

Replaces the previous FastAPI app (`main.py`, in git history before this branch).

## Layout

| File | Role |
|---|---|
| `src/index.ts` | Worker: routing, CORS, WebSocket pass-through. Stateless. |
| `src/game-room.ts` | `GameRoom` Durable Object: one instance per game, owns state + sockets. |
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

The same routes are available per game, which is where `plan.md` §5.2 is headed:

```
POST /api/v1/games/{game_id}/device-event
GET  /api/v1/games/{game_id}/rankings
POST /api/v1/games/{game_id}/add-infected
GET  /api/v1/games/{game_id}/population-state
WS   /api/v1/games/{game_id}/ws/population
WS   /api/v1/games/{game_id}/ws/device?device_id={device_id}
```

Adding the real `plan.md` endpoints (`/gateway/sync`, `/registrations`, `/rounds`)
means new RPC methods on `GameRoom` and new cases in the Worker switch — the
per-game routing is already in place.

## WebSockets

1. **Dashboard WebSocket (`/ws/population`)**:
   Pushes the full population state object on connect and after every mutation. It uses the hibernation API (`ctx.acceptWebSocket`), so clients stay connected while the object sleeps.

2. **ESP Device WebSocket (`/ws/device?device_id={id}`)**:
   Maintains an open bidirectional socket for ESP devices:
   - On connect: Sends `{ "type": "init", "device_id": "...", "role": "not infected", "is_infected": false }`.
   - On game start: Pushes `{ "type": "role_assignment", "role": "infected" | "not infected" }` immediately.
   - Sending events: ESP sends `{ "type": "event", "current_state": "infected" }` over the socket; server replies with `{ "type": "event_ack" }`.
   - On game over: Pushes `{ "type": "game_over", "rank": N, "survival_time_seconds": S, "rankings": [...] }`.
   - Keepalive: `ping` is auto-answered with `pong` by the runtime without waking the object.

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
npm test            # vitest-pool-workers
```

Point the frontend at a deployed Worker with `VITE_API_BASE` in `frontend/.env`;
it defaults to `http://localhost:8787`.

## Notes

- `npm test` warns that the bundled test runtime only supports compatibility
  date `2025-10-11` and falls back to it. Deploys use the date in
  `wrangler.jsonc`; only the local test runtime is older.
- First deploy applies migration `v1` (`new_sqlite_classes: ["GameRoom"]`).
  Do not edit that entry afterwards — add a new tag instead.
