import { DurableObject } from "cloudflare:workers";
import type { PopulationState } from "./types";

interface PopulationRow extends Record<string, SqlStorageValue> {
  num_players: number;
  num_infected: number;
}

/**
 * One instance per game id. Owns that game's population counters and every
 * dashboard WebSocket watching it.
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
    });

    // Answered by the runtime without waking the object from hibernation.
    this.ctx.setWebSocketAutoResponse(
      new WebSocketRequestResponsePair("ping", "pong"),
    );
  }

  getState(): PopulationState {
    const row = this.ctx.storage.sql
      .exec<PopulationRow>("SELECT num_players, num_infected FROM population WHERE id = 1")
      .one();

    const humans = Math.max(0, row.num_players - row.num_infected);
    const survivedRatio = row.num_players > 0 ? humans / row.num_players : 0;

    return {
      num_players: row.num_players,
      num_infected: row.num_infected,
      num_humans: humans,
      survived_pct: Math.round(survivedRatio * 1000) / 10,
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
   * request for the current state.
   */
  override async webSocketMessage(ws: WebSocket): Promise<void> {
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
