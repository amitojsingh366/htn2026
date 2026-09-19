import { DurableObject } from "cloudflare:workers";
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
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);

    ctx.blockConcurrencyWhile(async () => {
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
    });

    // Answered by the runtime without waking the object from hibernation.
    this.ctx.setWebSocketAutoResponse(
      new WebSocketRequestResponsePair("ping", "pong"),
    );
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
    const isGameOver = Boolean(meta.game_over) || (row.num_players > 0 && row.num_infected >= row.num_players);

    const state: PopulationState = {
      num_players: row.num_players,
      num_infected: row.num_infected,
      num_humans: humans,
      survived_pct: Math.round(survivedRatio * 1000) / 10,
      game_over: isGameOver,
      started_at: meta.started_at,
      patient_zero_id: meta.patient_zero_id,
    };

    if (isGameOver) {
      state.rankings = this.calculateRankings();
    }

    return state;
  }

  startGame(gameId = "default"): StartGameResponse {
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
    this.ctx.storage.sql.exec(
      "UPDATE population SET num_players = num_players + 1 WHERE id = 1",
    );
    return this.broadcastState();
  }

  /** Infecting is capped at the roster size, matching the previous backend. */
  addInfected(): PopulationState {
    this.ctx.storage.sql.exec(
      "UPDATE population SET num_infected = MIN(num_infected + 1, num_players) WHERE id = 1",
    );
    return this.broadcastState();
  }

  reset(): PopulationState {
    this.ctx.storage.sql.exec("DELETE FROM players");
    this.ctx.storage.sql.exec(
      "UPDATE game_meta SET started_at = NULL, ended_at = NULL, game_over = 0, patient_zero_id = NULL WHERE id = 1",
    );
    this.ctx.storage.sql.exec(
      "UPDATE population SET num_players = 0, num_infected = 0 WHERE id = 1",
    );
    return this.broadcastState();
  }

  /** WebSocket upgrade. Everything else is RPC. */
  override async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.send(JSON.stringify(this.getState()));

    return new Response(null, { status: 101, webSocket: client });
  }

  /**
   * Clients only need to hold the socket open; a message is treated as a
   * request for the current state. If a message contains { device_id },
   * it returns state plus that device's role.
   */
  override async webSocketMessage(ws: WebSocket, message: ArrayBuffer | string): Promise<void> {
    try {
      if (typeof message === "string") {
        const parsed = JSON.parse(message);
        if (parsed.device_id) {
          const devState = this.getDeviceState(parsed.device_id);
          ws.send(JSON.stringify({ ...this.getState(), device: devState }));
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
    ws.close(code === 1006 ? 1000 : code, reason);
  }

  /** Persist first, then push: storage is written before this is called. */
  private broadcastState(): PopulationState {
    const state = this.getState();
    const payload = JSON.stringify(state);

    for (const ws of this.ctx.getWebSockets()) {
      try {
        ws.send(payload);
      } catch {
        // Socket died between getWebSockets() and send(); the close handler cleans up.
      }
    }

    return state;
  }
}

