# Walkthrough — Open WebSocket Protocol for ESP Devices

ESP devices can now maintain an **open, persistent WebSocket connection** to the backend Durable Object. This enables:
1. **0ms Latency Role Delivery**: When the frontend clicks "Start Game", the randomly chosen Patient Zero role is immediately pushed down the open socket.
2. **Real-Time Telemetry over WebSocket**: The ESP sends its infection state over the open socket without needing HTTP POST requests.
3. **Instant Game Over & Rankings**: As soon as all players are infected, each ESP device receives its final rank, survival time, and full leaderboard over its open socket.
4. **Hibernation & Auto Ping/Pong**: The Durable Object hibernates when idle, and `ping` frames are automatically answered with `pong` by the Cloudflare Workers runtime without waking the object or incurring billed CPU duration.

---

## ESP WebSocket Specification

### 1. Connection URL
```
ws(s)://<backend-host>/ws/device?device_id=<your_device_id>
```
*(Or per-game: `ws(s)://<backend-host>/api/v1/games/<game_id>/ws/device?device_id=<your_device_id>`)*

### 2. Connection Handshake (`init`)
Immediately upon connecting, the server auto-registers the device in the game roster and sends:
```json
{
  "type": "init",
  "device_id": "esp-01",
  "role": "not infected",
  "is_infected": false,
  "game_started": false,
  "started_at": null,
  "game_over": false
}
```

### 3. Role Assignment Push (`role_assignment`)
When the frontend clicks **"🚀 Start Game"**, the server randomly selects one player as Patient Zero and pushes their new role to every connected ESP:
```json
// To the chosen Patient Zero ESP:
{
  "type": "role_assignment",
  "device_id": "esp-01",
  "role": "infected",
  "is_infected": true,
  "game_started": true,
  "started_at": 1711000000000,
  "game_over": false
}

// To all healthy ESPs:
{
  "type": "role_assignment",
  "device_id": "esp-02",
  "role": "not infected",
  "is_infected": false,
  "game_started": true,
  "started_at": 1711000000000,
  "game_over": false
}
```

### 4. ESP Reports Getting Tagged / Infected (`event` -> `event_ack`)
When the ESP player is tagged, the ESP sends a message over the open socket:
```json
{
  "type": "event",
  "current_state": "infected",
  "timestamp": 12500
}
```
The server updates the room state and immediately acknowledges:
```json
{
  "type": "event_ack",
  "device_id": "esp-02",
  "role": "infected",
  "is_infected": true,
  "game_started": true,
  "started_at": 1711000000000,
  "game_over": false
}
```

### 5. Game Over & Leaderboard Push (`game_over`)
When all participants have been infected, the server pushes the final ranking to every connected ESP:
```json
{
  "type": "game_over",
  "device_id": "esp-02",
  "rank": 1,
  "survival_time_seconds": 29,
  "rankings": [
    {
      "rank": 1,
      "device_id": "esp-02",
      "state": "infected",
      "infected_at": 1711000029000,
      "survival_time_seconds": 29
    },
    {
      "rank": 2,
      "device_id": "esp-01",
      "state": "infected",
      "infected_at": 1711000000000,
      "survival_time_seconds": 0
    }
  ]
}
```

---

## Verification Results

### Vitest Suite (15/15 Passed)
```bash
npm test
```
```
✓ test/game-room.test.ts (15 tests) 988ms
Test Files  1 passed (1)
     Tests  15 passed (15)
```

### Build & Typecheck
- `npm run typecheck` in `backend` passed with 0 errors.
- `npm run build` in `frontend` passed with 0 errors.
