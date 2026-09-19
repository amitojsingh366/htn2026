# Walkthrough — Start Game Flow, Patient Zero Assignment & ESP Role Delivery

Added an interactive **"Start Game"** capability:
- The frontend clicks **"🚀 Start Game (Pick Patient Zero)"**.
- The backend initiates game timing, randomly selects one registered participant to become **Patient Zero** (`state = 'infected'`), and resets the other participants to healthy (`state = 'not infected'`).
- The role assignment and timing are delivered to ESP devices via HTTP polling (`GET /device-state?device_id=...`), telemetry ingestion responses (`POST /device-event`), and real-time WebSocket push (`/ws/population`).
- The frontend displays a live digital survival clock, patient zero alert, and survival leaderboard.

---

## Changes Made

### 1. Backend (`backend/`)

- **[types.ts](file:///c:/Users/anton/code_and_projects/htn2026/backend/src/types.ts)**:
  - Added `StartGameResponse` and `DeviceStateResponse`.
  - Added `started_at?: number | null` and `patient_zero_id?: string | null` to `PopulationState`.

- **[game-room.ts](file:///c:/Users/anton/code_and_projects/htn2026/backend/src/game-room.ts)**:
  - Added `patient_zero_id TEXT` column to `game_meta` table in SQLite.
  - Implemented `startGame(gameId)`:
    - Sets `started_at = Date.now()`, `ended_at = NULL`, `game_over = 0`.
    - Randomly picks one player from registered participants in the `players` table as Patient Zero.
    - Sets that player's state to `'infected'` and all other players to `'not infected'`.
    - Broadcasts the update to all connected WebSocket clients.
  - Implemented `getDeviceState(deviceId)`:
    - Returns `{ device_id, role, is_infected, game_started, started_at, game_over }`.
  - Updated `recordDeviceEvent()`:
    - Returns `assigned_role` and `is_infected` in the response payload.
  - Updated `reset()`:
    - Resets `patient_zero_id = NULL` and `started_at = NULL`.
  - Updated `webSocketMessage()`:
    - Supports device-specific queries `{ "device_id": "esp-01" }`.

- **[index.ts](file:///c:/Users/anton/code_and_projects/htn2026/backend/src/index.ts)**:
  - Added `POST /start-game` (and `/api/v1/games/:gameId/start-game`, alias `/game/start`).
  - Added `GET /device-state` (and `/api/v1/games/:gameId/device-state`, supporting `?device_id=...`).

- **[game-room.test.ts](file:///c:/Users/anton/code_and_projects/htn2026/backend/test/game-room.test.ts)**:
  - Added tests for `startGame`, random patient zero selection, and `GET /device-state`.

---

### 2. Frontend (`frontend/`)

- **[PopulationState.tsx](file:///c:/Users/anton/code_and_projects/htn2026/frontend/src/components/PopulationState.tsx)**:
  - Extended `PopulationStateData` with `started_at`, `patient_zero_id`, `game_over`, and `rankings`.
  - Connected `onUpdate` callback to dispatch these to the parent `App` component on both HTTP fetch and WebSocket stream frames.

- **[App.tsx](file:///c:/Users/anton/code_and_projects/htn2026/frontend/src/App.tsx)**:
  - Added **"🚀 Start Game (Pick Patient Zero)"** button.
  - Added live ticking survival timer (`MM:SS`) synced with `started_at`.
  - Added Patient Zero badge (`☣ esp-XX`).
  - Added **Final Survival Leaderboard** rendered upon Game Over (`game_over: true`).

- **[App.css](file:///c:/Users/anton/code_and_projects/htn2026/frontend/src/App.css)**:
  - Styled `.btn-start-game` with amber/crimson glowing gradient.
  - Styled `.round-status-banner`, `.round-timer-value`, and `.leaderboard-table`.

---

## Verification Results

### Backend Automated Tests
```bash
npm test
```
```
✓ test/game-room.test.ts (14 tests) 647ms
Test Files  1 passed (1)
     Tests  14 passed (14)
```

### TypeScript Validation
```bash
npm run typecheck # in backend
npm run build     # in frontend
```
Both succeeded with 0 errors.

---

## How the ESP Device Receives Its Role

1. **Option A: HTTP Polling**
   ```http
   GET /device-state?device_id=esp-01
   ```
   **Response**:
   ```json
   {
     "device_id": "esp-01",
     "role": "infected",
     "is_infected": true,
     "game_started": true,
     "started_at": 1711000000000,
     "game_over": false
   }
   ```

2. **Option B: Event Telemetry Response**
   Whenever the device posts telemetry to `POST /device-event`, the response contains:
   ```json
   {
     "message": "Device event recorded",
     "assigned_role": "infected",
     "is_infected": true,
     "num_players": 4,
     "num_infected": 1
   }
   ```

3. **Option C: WebSocket Stream**
   ESP devices connected to `ws://localhost:8787/ws/population` receive live broadcast updates with `patient_zero_id` as soon as the game begins.
