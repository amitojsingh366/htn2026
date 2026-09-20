import { DurableObject } from "cloudflare:workers";
import { HostGateway } from "./gateway";
import type {
  DeviceEventInput,
  DeviceStateResponse,
  LeaderboardResponse,
  PlayerRanking,
  PopulationState,
  StartGameResponse,
} from "./types";

interface PopulationRow extends Record<string, SqlStorageValue> {
  num_players: number;
  num_infected: number;
}

interface PlayerRow extends Record<string, SqlStorageValue> {
  device_id: string;
  state: string;
  joined_at: number;
  infected_at: number | null;
}

interface GameMetaRow extends Record<string, SqlStorageValue> {
  started_at: number | null;
  ended_at: number | null;
  game_over: number;
  patient_zero_id: string | null;
}

/**
 * One instance per game id. Owns that game's population counters, player list,
 * leaderboard, and every dashboard WebSocket watching it.
 */
export class GameRoom extends DurableObject<Env> {
  private gateway: HostGateway;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.gateway = new HostGateway(ctx, env, () => { this.broadcastState(); });

    ctx.blockConcurrencyWhile(async () => {
      this.gateway.initialize();
      this.ctx.storage.sql.exec(`
        CREATE TABLE IF NOT EXISTS population (
          id INTEGER PRIMARY KEY CHECK (id = 1),
          num_players INTEGER NOT NULL DEFAULT 0,
          num_infected INTEGER NOT NULL DEFAULT 0
        )
      `);
      this.ctx.storage.sql.exec(
        "INSERT OR IGNORE INTO population (id, num_players, num_infected) VALUES (1, 0, 0)",
      );

      this.ctx.storage.sql.exec(`
        CREATE TABLE IF NOT EXISTS game_meta (
          id INTEGER PRIMARY KEY CHECK (id = 1),
          started_at INTEGER,
          ended_at INTEGER,
          game_over INTEGER NOT NULL DEFAULT 0,
          patient_zero_id TEXT
        )
      `);
      this.ctx.storage.sql.exec(
        "INSERT OR IGNORE INTO game_meta (id, started_at, ended_at, game_over, patient_zero_id) VALUES (1, NULL, NULL, 0, NULL)",
      );

      // In case the table was created earlier without patient_zero_id
      try {
        this.ctx.storage.sql.exec("ALTER TABLE game_meta ADD COLUMN patient_zero_id TEXT");
      } catch {
        // Column already exists
      }

      this.ctx.storage.sql.exec(`
        CREATE TABLE IF NOT EXISTS players (
          device_id TEXT PRIMARY KEY,
          state TEXT NOT NULL,
          joined_at INTEGER NOT NULL,
          infected_at INTEGER
        )
      `);
      await this.gateway.recoverAlarm();
    });

    // Answered by the runtime without waking the object from hibernation.
    this.ctx.setWebSocketAutoResponse(
      new WebSocketRequestResponsePair("ping", "pong"),
    );
  }

  override async alarm(): Promise<void> {
    await this.gateway.alarm();
  }

  calculateRankings(): PlayerRanking[] {
    const meta = this.ctx.storage.sql
      .exec<GameMetaRow>("SELECT started_at, ended_at, game_over FROM game_meta WHERE id = 1")
      .one();

    const players = Array.from(
      this.ctx.storage.sql.exec<PlayerRow>(
        "SELECT device_id, state, joined_at, infected_at FROM players",
      ),
    );

    // Rank from longest survived to earliest infected:
    // 1. Uninfected players survived the longest.
    // 2. Among infected players, higher infected_at (later infected) survived longer.
    // 3. Earliest infected (lowest infected_at / patient zero) has the lowest rank.
    players.sort((a, b) => {
      const aInfected = a.state === "infected";
      const bInfected = b.state === "infected";

      if (!aInfected && bInfected) return -1;
      if (aInfected && !bInfected) return 1;

      if (aInfected && bInfected) {
        const aTime = a.infected_at ?? 0;
        const bTime = b.infected_at ?? 0;
        if (bTime !== aTime) {
          return bTime - aTime; // descending: latest infected first
        }
      }

      return a.joined_at - b.joined_at;
    });

    const gameStart = meta.started_at ?? (players.length > 0 ? Math.min(...players.map((p) => p.joined_at)) : 0);

    return players.map((p, idx) => {
      let survivalTimeSeconds = 0;
      if (p.state === "infected" && p.infected_at !== null) {
        const start = Math.min(gameStart, p.joined_at);
        survivalTimeSeconds = Math.max(0, Math.round((p.infected_at - start) / 1000));
      } else {
        const start = Math.min(gameStart, p.joined_at);
        const end = meta.ended_at ?? Date.now();
        survivalTimeSeconds = Math.max(0, Math.round((end - start) / 1000));
      }

      return {
        rank: idx + 1,
        device_id: p.device_id,
        state: p.state as "infected" | "not infected",
        infected_at: p.infected_at,
        survival_time_seconds: survivalTimeSeconds,
      };
    });
  }

  getRankings(gameId = "default"): LeaderboardResponse {
    const meta = this.ctx.storage.sql
      .exec<GameMetaRow>("SELECT started_at, ended_at, game_over FROM game_meta WHERE id = 1")
      .one();

    const counts = this.ctx.storage.sql
      .exec<{ total: number; infected: number }>(`
        SELECT 
          COUNT(*) as total, 
          COALESCE(SUM(CASE WHEN state = 'infected' THEN 1 ELSE 0 END), 0) as infected 
        FROM players
      `)
      .one();

    return {
      game_id: gameId,
      game_over: Boolean(meta.game_over),
      total_players: counts.total,
      num_infected: counts.infected,
      rankings: this.calculateRankings(),
    };
  }

  getState(): PopulationState {
    const row = this.ctx.storage.sql
      .exec<PopulationRow>("SELECT num_players, num_infected FROM population WHERE id = 1")
      .one();

    const meta = this.ctx.storage.sql
      .exec<GameMetaRow>("SELECT started_at, ended_at, game_over, patient_zero_id FROM game_meta WHERE id = 1")
      .one();

    const humans = Math.max(0, row.num_players - row.num_infected);
    const survivedRatio = row.num_players > 0 ? humans / row.num_players : 0;
    // The gateway closes gameplay immediately; evidence completeness remains a
    // separate result flag while offline badges finish their close receipts.
    const isGameOver = Boolean(meta.game_over) || (!this.gateway.enabled() && row.num_players > 0 && row.num_infected >= row.num_players);

    const state: PopulationState = {
      ...this.gateway.dashboard(),
      num_players: row.num_players,
      num_infected: row.num_infected,
      num_humans: humans,
      survived_pct: Math.round(survivedRatio * 1000) / 10,
      game_over: isGameOver,
      started_at: meta.started_at,
      ended_at: meta.ended_at,
      patient_zero_id: meta.patient_zero_id,
    };

    if (isGameOver) {
      state.rankings = this.calculateRankings();
    }

    return state;
  }

  async startGame(gameId = "default"): Promise<StartGameResponse | Record<string, unknown>> {
    if (this.gateway.enabled()) return this.gateway.prepare(gameId);
    const now = Date.now();

    // Fetch registered players
    const players = Array.from(
      this.ctx.storage.sql.exec<PlayerRow>(
        "SELECT device_id, state, joined_at, infected_at FROM players",
      ),
    );

    let patientZeroId: string | null = null;

    if (players.length > 0) {
      // Pick one random participant to be infected
      const randomIndex = Math.floor(Math.random() * players.length);
      const chosen = players[randomIndex];
      patientZeroId = chosen.device_id;

      // Reset all players to not infected first
      this.ctx.storage.sql.exec(
        "UPDATE players SET state = 'not infected', infected_at = NULL",
      );

      // Infect the chosen patient zero
      this.ctx.storage.sql.exec(
        "UPDATE players SET state = 'infected', infected_at = ? WHERE device_id = ?",
        now,
        patientZeroId,
      );

      // Sync population counters
      this.ctx.storage.sql.exec(
        "UPDATE population SET num_players = ?, num_infected = 1 WHERE id = 1",
        players.length,
      );
    } else {
      const pop = this.ctx.storage.sql
        .exec<PopulationRow>("SELECT num_players, num_infected FROM population WHERE id = 1")
        .one();
      if (pop.num_players > 0) {
        this.ctx.storage.sql.exec(
          "UPDATE population SET num_infected = 1 WHERE id = 1",
        );
      }
    }

    // Update game_meta
    this.ctx.storage.sql.exec(
      "UPDATE game_meta SET started_at = ?, ended_at = NULL, game_over = 0, patient_zero_id = ? WHERE id = 1",
      now,
      patientZeroId,
    );

    const state = this.broadcastState();
    this.broadcastRolesToDevices();

    return {
      message: patientZeroId
        ? `Game started! ${patientZeroId} assigned as Patient Zero.`
        : "Game started! Awaiting players.",
      game_id: gameId,
      started_at: now,
      patient_zero_id: patientZeroId,
      num_players: state.num_players,
      num_infected: state.num_infected,
      state,
    };
  }

  getDeviceState(deviceId: string): DeviceStateResponse {
    const trimmedId = (deviceId || "").trim();
    const meta = this.ctx.storage.sql
      .exec<GameMetaRow>("SELECT started_at, ended_at, game_over, patient_zero_id FROM game_meta WHERE id = 1")
      .one();

    const existing = Array.from(
      this.ctx.storage.sql.exec<PlayerRow>(
        "SELECT device_id, state, joined_at, infected_at FROM players WHERE device_id = ?",
        trimmedId,
      ),
    );

    const isGameOver = Boolean(meta.game_over);
    const gameStarted = meta.started_at !== null;

    if (existing.length === 0) {
      return {
        device_id: trimmedId,
        role: "not infected",
        is_infected: false,
        game_started: gameStarted,
        started_at: meta.started_at,
        game_over: isGameOver,
      };
    }

    const player = existing[0];
    const isInfected = player.state === "infected";

    return {
      device_id: trimmedId,
      role: isInfected ? "infected" : "not infected",
      is_infected: isInfected,
      game_started: gameStarted,
      started_at: meta.started_at,
      game_over: isGameOver,
    };
  }

  recordDeviceEvent(payload: DeviceEventInput): PopulationState & { assigned_role: string; is_infected: boolean } {
    this.gateway.assertLegacyMutation();
    const deviceId = (payload.device_id ?? payload.deviceId ?? "").trim();
    if (!deviceId) {
      throw new Error("device_id is required");
    }

    const rawTs = payload.timestamp ?? payload.time_stamp ?? payload.time;
    let ts: number;
    if (typeof rawTs === "number") {
      if (rawTs >= 1e9 && rawTs < 1e11) {
        ts = rawTs * 1000;
      } else {
        ts = rawTs;
      }
    } else if (typeof rawTs === "string") {
      const parsed = Number(rawTs);
      if (!Number.isNaN(parsed)) {
        if (parsed >= 1e9 && parsed < 1e11) {
          ts = parsed * 1000;
        } else {
          ts = parsed;
        }
      } else {
        ts = Date.parse(rawTs) || Date.now();
      }
    } else {
      ts = Date.now();
    }

    const rawState = payload.current_state ?? payload.state;
    let isInfected = false;
    if (typeof rawState === "boolean") {
      isInfected = rawState;
    } else if (typeof rawState === "string") {
      const normalized = rawState.trim().toLowerCase();
      isInfected = normalized === "infected" || normalized === "true" || normalized === "1";
    }
    const stateStr = isInfected ? "infected" : "not infected";

    // Set game start timestamp if not yet established
    this.ctx.storage.sql.exec(
      "UPDATE game_meta SET started_at = ? WHERE id = 1 AND started_at IS NULL",
      ts,
    );

    const existing = Array.from(
      this.ctx.storage.sql.exec<PlayerRow>(
        "SELECT device_id, state, joined_at, infected_at FROM players WHERE device_id = ?",
        deviceId,
      ),
    );

    if (existing.length === 0) {
      this.ctx.storage.sql.exec(
        "INSERT INTO players (device_id, state, joined_at, infected_at) VALUES (?, ?, ?, ?)",
        deviceId,
        stateStr,
        ts,
        isInfected ? ts : null,
      );
    } else {
      const curr = existing[0];
      if (isInfected && curr.state !== "infected") {
        this.ctx.storage.sql.exec(
          "UPDATE players SET state = 'infected', infected_at = ? WHERE device_id = ?",
          ts,
          deviceId,
        );
      }
    }

    // Sync population table with current counts
    const counts = this.ctx.storage.sql
      .exec<{ total: number; infected: number }>(`
        SELECT 
          COUNT(*) as total, 
          COALESCE(SUM(CASE WHEN state = 'infected' THEN 1 ELSE 0 END), 0) as infected 
        FROM players
      `)
      .one();

    this.ctx.storage.sql.exec(
      "UPDATE population SET num_players = ?, num_infected = ? WHERE id = 1",
      counts.total,
      counts.infected,
    );

    // Game ends when all participants have been infected (minimum 1 player)
    if (counts.total > 0 && counts.infected >= counts.total) {
      const maxInfected = this.ctx.storage.sql
        .exec<{ max_inf: number | null }>("SELECT MAX(infected_at) as max_inf FROM players")
        .one();

      this.ctx.storage.sql.exec(
        "UPDATE game_meta SET game_over = 1, ended_at = ? WHERE id = 1",
        maxInfected.max_inf ?? ts,
      );
    }

    const state = this.broadcastState();
    const dev = this.getDeviceState(deviceId);

    return {
      ...state,
      assigned_role: dev.role,
      is_infected: dev.is_infected,
    };
  }

  addPlayer(): PopulationState {
    this.gateway.assertLegacyMutation();
    this.ctx.storage.sql.exec(
      "UPDATE population SET num_players = num_players + 1 WHERE id = 1",
    );
    return this.broadcastState();
  }

  /** Infecting is capped at the roster size, matching the previous backend. */
  addInfected(): PopulationState {
    this.gateway.assertLegacyMutation();
    this.ctx.storage.sql.exec(
      "UPDATE population SET num_infected = MIN(num_infected + 1, num_players) WHERE id = 1",
    );
    return this.broadcastState();
  }

  reset(): PopulationState {
    this.gateway.resetLobby();
    return this.broadcastState();
  }

  broadcastRolesToDevices(): void {
    for (const ws of this.ctx.getWebSockets("device")) {
      try {
        const att = ws.deserializeAttachment() as { device_id?: string } | null;
        const id = att?.device_id;
        if (id) {
          const dev = this.getDeviceState(id);
          ws.send(
            JSON.stringify({
              type: "role_assignment",
              ...dev,
            }),
          );
        }
      } catch {}
    }
  }

  broadcastGameOverToDevices(): void {
    const rankings = this.calculateRankings();
    for (const ws of this.ctx.getWebSockets("device")) {
      try {
        const att = ws.deserializeAttachment() as { device_id?: string } | null;
        const devId = att?.device_id;
        const myRank = devId ? rankings.find((r) => r.device_id === devId) : null;
        ws.send(
          JSON.stringify({
            type: "game_over",
            device_id: devId,
            rank: myRank?.rank,
            survival_time_seconds: myRank?.survival_time_seconds,
            rankings,
          }),
        );
      } catch {}
    }
  }

  /** WebSocket upgrade. Supports both dashboard clients and ESP devices. */
  override async fetch(request: Request): Promise<Response> {
    const gatewayMatch = new URL(request.url).pathname.match(/^\/api\/v1\/games\/([^/]+)(\/gateway\/(?:bootstrap|socket|control)|\/registrations)$/);
    if (gatewayMatch) return this.gateway.fetch(request, gatewayMatch[1], gatewayMatch[2]);
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const url = new URL(request.url);
    const deviceIdParam = (url.searchParams.get("device_id") || url.searchParams.get("deviceId") || "").trim();
    const isDevice = url.pathname.includes("/ws/device") || url.pathname.includes("/ws/esp") || Boolean(deviceIdParam);

    const [client, server] = Object.values(new WebSocketPair());

    if (isDevice) {
      if (this.gateway.enabled()) return Response.json({ error: "Live badges use the designated host gateway." }, { status: 409 });
      const tags = deviceIdParam ? ["device", `device:${deviceIdParam}`] : ["device"];
      this.ctx.acceptWebSocket(server, tags);
      server.serializeAttachment({ device_id: deviceIdParam, is_device: true });

      if (deviceIdParam) {
        // Auto-register device in players table if not yet present
        const existing = Array.from(
          this.ctx.storage.sql.exec<PlayerRow>(
            "SELECT device_id, state, joined_at, infected_at FROM players WHERE device_id = ?",
            deviceIdParam,
          ),
        );

        if (existing.length === 0) {
          this.ctx.storage.sql.exec(
            "INSERT INTO players (device_id, state, joined_at, infected_at) VALUES (?, 'not infected', ?, NULL)",
            deviceIdParam,
            Date.now(),
          );

          // Sync population table
          const counts = this.ctx.storage.sql
            .exec<{ total: number; infected: number }>(`
              SELECT COUNT(*) as total, COALESCE(SUM(CASE WHEN state = 'infected' THEN 1 ELSE 0 END), 0) as infected FROM players
            `)
            .one();

          this.ctx.storage.sql.exec(
            "UPDATE population SET num_players = ?, num_infected = ? WHERE id = 1",
            counts.total,
            counts.infected,
          );

          this.broadcastState();
        }

        const devState = this.getDeviceState(deviceIdParam);
        server.send(
          JSON.stringify({
            type: "init",
            ...devState,
          }),
        );
      } else {
        server.send(
          JSON.stringify({
            type: "connected",
            message: "ESP WebSocket connected. Send { \"device_id\": \"...\" } to register.",
          }),
        );
      }
    } else {
      this.ctx.acceptWebSocket(server, ["dashboard"]);
      server.serializeAttachment({ is_device: false });
      server.send(JSON.stringify(this.getState()));
    }

    return new Response(null, { status: 101, webSocket: client });
  }

  /**
   * Handles incoming frames from both dashboard clients and ESP devices.
   * If a device sends telemetry over WebSocket, records event and replies with event_ack.
   */
  override async webSocketMessage(ws: WebSocket, message: ArrayBuffer | string): Promise<void> {
    if ((ws.deserializeAttachment() as { is_gateway?: boolean } | null)?.is_gateway) {
      await this.gateway.message(ws, message); return;
    }
    try {
      if (typeof message === "string") {
        const parsed = JSON.parse(message);
        let att = (ws.deserializeAttachment() as { device_id?: string; is_device?: boolean } | null) || {};
        const deviceId = (parsed.device_id || parsed.deviceId || att.device_id || "").trim();

        if (parsed.device_id && !att.device_id) {
          att = { ...att, device_id: parsed.device_id, is_device: true };
          ws.serializeAttachment(att);
        }

        // If ESP reports state / event over WebSocket
        if (parsed.current_state !== undefined || parsed.state !== undefined || parsed.type === "event") {
          if (!deviceId) {
            ws.send(JSON.stringify({ type: "error", error: "device_id is required" }));
            return;
          }

          const res = this.recordDeviceEvent({
            device_id: deviceId,
            timestamp: parsed.timestamp ?? parsed.time_stamp ?? Date.now(),
            current_state: parsed.current_state ?? parsed.state,
          });

          const devState = this.getDeviceState(deviceId);
          ws.send(
            JSON.stringify({
              type: "event_ack",
              ...devState,
            }),
          );

          if (res.game_over) {
            this.broadcastGameOverToDevices();
          }
          return;
        }

        // If ESP requests current role / state
        if (deviceId) {
          const devState = this.getDeviceState(deviceId);
          ws.send(
            JSON.stringify({
              type: "device_state",
              ...devState,
            }),
          );
          return;
        }
      }
    } catch {}

    ws.send(JSON.stringify(this.getState()));
  }

  override async webSocketClose(
    ws: WebSocket,
    code: number,
    reason: string,
  ): Promise<void> {
    // 1006 is reserved and cannot be sent back to the peer.
    const attachment = ws.deserializeAttachment() as { is_gateway?: boolean; welcomed?: boolean } | null;
    if (attachment?.is_gateway) {
      ws.serializeAttachment({ ...attachment, welcomed: false });
      this.broadcastState();
    }
    ws.close(code === 1006 ? 1000 : code, reason);
  }

  /** Persist first, then push: storage is written before this is called. */
  private broadcastState(): PopulationState {
    const state = this.getState();
    const payload = JSON.stringify(state);

    for (const ws of this.ctx.getWebSockets("dashboard")) {
      try {
        const att = ws.deserializeAttachment() as { is_device?: boolean } | null;
        if (!att?.is_device) {
          ws.send(payload);
        }
      } catch {
        // Socket died between getWebSockets() and send(); the close handler cleans up.
      }
    }

    if (state.game_over) {
      this.broadcastGameOverToDevices();
    }

    return state;
  }
}
