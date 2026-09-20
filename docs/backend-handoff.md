# What the badges need from the backend

**To:** backend teammate · **From:** firmware · **Date:** 2026-09-19 · **Status:** proposed, amend before you code

This is the short version. The byte-level spec is [`docs/backend-contract.md`](backend-contract.md) (426 lines) — read that before implementing, but read this first to know what you're signing up for.

**Nothing here is built yet on your side, and that's fine — but the gap is bigger than it looks.** The current Worker implements a population counter (`/add-player`, `/add-infected`, `/num-infected`, a WebSocket push of `{num_players, num_infected, num_humans, survived_pct}`). None of that is on the path below. The firmware doesn't need a counter; it needs an authority that adjudicates causal infection events. If you'd rather change the contract than build this, say so now — it's cheaper to amend before either of us writes code.

---

## The shape of it

One game. Up to 20 badges. **Exactly one badge — the host — ever talks to you.** The other 19 talk only to each other over ESP-NOW radio. So you are serving a single client, not twenty.

```
19 player badges  ──ESP-NOW radio──►  host badge  ──HTTPS + one WSS──►  you
```

Two transports, and the split matters:

- **HTTPS** — bootstrap and player registration only. Short request/response, runs before the socket opens.
- **One WSS connection** — everything live: event uploads, receipts, decisions, commands, state. Opened once, resumed across drops, never polled.

There is **no HTTP polling fallback**. That was the original design and it was deliberately removed. Please don't add one back as a convenience.

---

## Endpoints

| Method | Path | Transport |
|---|---|---|
| `GET` | `/api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}` | HTTPS |
| `POST` | `/api/v1/games/{game_id}/registrations` | HTTPS |
| `GET` | `/api/v1/games/{game_id}/gateway/socket` → `101` | WSS upgrade |
| `POST` | `/api/v1/games/{game_id}/rounds` | HTTPS — **your dashboard, not firmware** |
| `POST` | `/api/v1/games/{game_id}/rounds/{round_id}/finish` | HTTPS — **your dashboard, not firmware** |

The firmware never calls the last two. It runs no HTTP server and needs no PUT/PATCH/DELETE.

**Auth:** `Authorization: Bearer <host-token>` as an HTTP header, on the HTTPS calls *and* on the WebSocket upgrade. Never in a URL or query string — the badge will not send it that way. Echo `Sec-WebSocket-Protocol: zt.v1` on the upgrade.

---

## Messages

One JSON object per WebSocket text message. **4,096-byte cap on the aggregate logical message**, not per frame. Every message carries `{"v":1,"t":"<type>","id":<u32>,"ts":<ms>}`.

**Badge → you**

| `t` | What it is | Cap |
|---|---|---|
| `hello` | Resume handshake. First message on every connection, always | once |
| `events` | Infection events from the mesh | ≤8, one batch in flight |
| `ack` | `applied`, `decision_applied`, `ready`, `round_closed`, `presence` | ≤8 per array |
| `need` | Asks for a snapshot page or specific events | ≤8 IDs |
| `time_sync` | Clock anchor request with a nonce | one outstanding |

**You → badge**

| `t` | What it is | Cap |
|---|---|---|
| `welcome` | Handshake reply: `resume:"ok"` or `"reset"`, `server_time_ms` | once |
| `receipts` | "I durably stored these event IDs" | ≤8 |
| `decisions` | "I adjudicated these": `accepted` / `rejected` / `pending_dependency` | ≤8 |
| `commands` | Durable outbox: PREPARE_ROUND, START_ROUND, ROLE_SET, ANNOUNCE, END_ROUND, FINAL_RESULT, CANCEL_PREPARE | ≤4 |
| `snapshot` | One roster page, bound to `snapshot_id` + `roster_hash` | 8 players/page |
| `need_events` | "Replay these to me" | ≤8 |
| `time_sync_reply` | Echo the nonce + `server_time_ms` | 1:1 |
| `error` | `{code, detail, fatal}` | — |

---

## The six things that will break the game if you get them wrong

These aren't style preferences. Each one maps to a way the demo visibly fails.

**1. Receipt, decision, and application are three different things.**
`receipts` means you durably stored it. `decisions` means you adjudicated it. Badge application means the target badge committed the resulting state. None implies another, and a successful socket send implies none of them. The badge keeps an event in durable storage until a `receipts` message names its ID — if you skip receipts and only send decisions, badges re-flood events forever.

**2. Events form a causal graph rooted at patient zero.**
Event ID is `<round16>/<slot02>/<seq04>`. Every infection names the actor *and the actor's own infection cause*. Arrival order is **not** gameplay order — two badges can tag out of radio range and sync hours apart. If a child event arrives before its parent, hold it as `pending_dependency` and ask for the parent via `need_events`. Don't reject it, and don't invent a parent.

**3. Dedupe by event ID; same ID with a different body is a conflict, never an overwrite.**
Reconnects replay. Duplicates are expected and harmless *if* you dedupe. Return `DUPLICATE_CONFLICT` if the body differs — that means a bug, not a retry.

**4. Role updates need a revision *and* a covered event sequence.**
A `ROLE_SET` carries `role_rev` and `covered_seq`. Without `covered_seq`, a stale "you're human" update can erase an infection the badge committed offline and hasn't uploaded yet. The badge will hold your update until it covers that sequence. This is the single most likely source of "the game said I was human again" bugs.

**5. Patient zero is chosen exactly once, by you, at round preparation.**
Random, from the frozen roster. Never re-run it. `START_ROUND` is idempotent — badges ignore a repeat with the same command sequence rather than restarting the timer.

**6. Ping/pong does not establish server time.**
Pongs carry no clock. The badge anchors time only from `server_time_ms` in bootstrap, `welcome`, or `time_sync_reply`, plus half the measured RTT. It rejects an anchor if RTT > 2,000 ms. Round expiry is computed from this, so a wrong clock ends the round early or late.

---

## Timings to match

Fixed on the firmware side; set your own timeouts to agree.

| | |
|---|---|
| WS ping interval | 10 s |
| Pong timeout | 20 s |
| Stale link (no inbound frame) | 25 s |
| Clock exchange | every 30 s, and once after `welcome` |
| Network timeout | 5 s |
| Reconnect backoff | 1, 2, 4, 8, 16, 30 s cap, ±20% jitter |
| Round length | 600,000 ms |

**Reconnect semantics.** On every reconnect the badge sends `hello` with `last_server_id` — the last outbox message it durably *applied*. Reply `resume:"ok"` and replay from there, or `resume:"reset"` if that cursor has expired. `"reset"` means the badge re-fetches a snapshot; it does **not** clear its journal, restart the round, or change any role. A socket drop must never cost a pending tag.

**Close codes the badge understands:** `1000`/`1001` reconnect · `1008` auth failure, stop · `1009` message too big · `1011` server error, reconnect · `4001` unknown game · `4003` stale round · `4010` cursor expired.

---

## Yours, not mine

- Game creation, operator login, the dashboard.
- Round preparation and forced finalization (the two dashboard endpoints).
- Durable command outbox with never-reused sequences, and repeating unacknowledged commands.
- The AI announcements. **`ANNOUNCE` only** — it has no gameplay power. Filter to ≤96 printable ASCII, no markup, at most one per 15 s. The badge holds no AI credentials and makes no model calls.
- Deciding when a round is `final` vs `provisional`: provisional until every roster member has reported `round_closed` and all their events are decided. A missing badge is explicit missing evidence, never a silent timeout.

---

## Checklist

- [ ] `GET /gateway/bootstrap` returns game metadata + `server_time_ms` + first snapshot page
- [ ] `POST /registrations`, idempotent by `Idempotency-Key` and by game/round + badge MAC
- [ ] `GET /gateway/socket` upgrades to WSS, validates bearer header, echoes `zt.v1`
- [ ] `hello` → `welcome` with working `resume:"ok"` / `"reset"`
- [ ] Durable event store, deduped by event ID, `receipts` sent only after the write commits
- [ ] `pending_dependency` + `need_events` for out-of-order causal chains
- [ ] Durable command outbox, u32 sequences never reused, repeat until acked
- [ ] Snapshot paging bound to `snapshot_id` + `roster_hash`, 8 players/page, 3 pages for 20
- [ ] `time_sync_reply` echoing the nonce with a fresh `server_time_ms`
- [ ] Patient zero selected randomly exactly once per frozen round
- [ ] Round finalization: provisional until every badge closes; forced-final sets `complete:false`

---

## Things I'd like your call on

1. **Are you good with WSS on a Cloudflare Worker + Durable Object?** A DO per game gives you the single-writer semantics this design wants — one game, one socket, one authority. It's a natural fit, but it's your call.
2. **Field spellings.** Everything in §5 of the contract is fixed by the radio protocol. The snapshot metadata and the remaining command bodies are *proposed* — flagged in `backend-contract.md`. Push back on any of them now rather than after we both build.
3. **Cursor retention.** How long do you want to keep the outbox replayable before returning `resume:"reset"`? I need to know so I can size the badge's expectations.

If any of this is more than you want to build before the deadline, tell me which parts and I'll tell you what degrades. Some of it is genuinely load-bearing; some of it only matters if badges go offline mid-round.
