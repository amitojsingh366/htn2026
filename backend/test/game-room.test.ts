import { env, SELF } from "cloudflare:test";
import { describe, expect, it, vi } from "vitest";
import type { LeaderboardResponse, PopulationState } from "../src/types";

describe("GameRoom", () => {
  it("starts empty", async () => {
    const stub = env.GAME_ROOM.getByName("empty");
    expect(await stub.getState()).toEqual({
      num_players: 0,
      num_infected: 0,
      num_humans: 0,
      survived_pct: 0,
      game_over: false,
      started_at: null,
      patient_zero_id: null,
    });
  });

  it("tracks humans and survival percentage", async () => {
    const stub = env.GAME_ROOM.getByName("ratio");
    for (let i = 0; i < 4; i++) await stub.addPlayer();
    await stub.addInfected();

    expect(await stub.getState()).toEqual({
      num_players: 4,
      num_infected: 1,
      num_humans: 3,
      survived_pct: 75,
      game_over: false,
      started_at: null,
      patient_zero_id: null,
    });
  });

  it("never infects more players than are registered", async () => {
    const stub = env.GAME_ROOM.getByName("cap");
    await stub.addPlayer();
    await stub.addInfected();
    await stub.addInfected();

    const state = await stub.getState();
    expect(state.num_infected).toBe(1);
    expect(state.survived_pct).toBe(0);
    expect(state.game_over).toBe(true);
  });

  it("resets both counters and player roster", async () => {
    const stub = env.GAME_ROOM.getByName("reset");
    await stub.addPlayer();
    await stub.addInfected();
    await stub.reset();

    expect(await stub.getState()).toMatchObject({ num_players: 0, num_infected: 0, game_over: false });
  });

  it("keeps games isolated from each other", async () => {
    const a = env.GAME_ROOM.getByName("game-a");
    const b = env.GAME_ROOM.getByName("game-b");
    await a.addPlayer();

    expect((await a.getState()).num_players).toBe(1);
    expect((await b.getState()).num_players).toBe(0);
  });

  it("ingests ESP device events and ranks from longest survived to earliest infected", async () => {
    const stub = env.GAME_ROOM.getByName("ranking-test");

    // Game starts at t=1000
    // Device 1 joins not infected
    await stub.recordDeviceEvent({
      device_id: "esp-1",
      timestamp: 1000,
      current_state: "not infected",
    });

    // Device 2 joins not infected
    await stub.recordDeviceEvent({
      device_id: "esp-2",
      timestamp: 1000,
      current_state: "not infected",
    });

    // Device 3 joins not infected
    await stub.recordDeviceEvent({
      device_id: "esp-3",
      timestamp: 1000,
      current_state: "not infected",
    });

    let state = await stub.getState();
    expect(state.num_players).toBe(3);
    expect(state.num_infected).toBe(0);
    expect(state.game_over).toBe(false);

    // Patient Zero: esp-2 is infected early at t=5000 (survived 4s)
    await stub.recordDeviceEvent({
      device_id: "esp-2",
      timestamp: 5000,
      current_state: "infected",
    });

    state = await stub.getState();
    expect(state.num_infected).toBe(1);
    expect(state.game_over).toBe(false);

    // esp-1 is infected next at t=15000 (survived 14s)
    await stub.recordDeviceEvent({
      device_id: "esp-1",
      timestamp: 15000,
      current_state: "infected",
    });

    state = await stub.getState();
    expect(state.num_infected).toBe(2);
    expect(state.game_over).toBe(false);

    // Final player esp-3 is infected at t=30000 (survived 29s).
    // All participants are infected -> game ends!
    const finalState = await stub.recordDeviceEvent({
      device_id: "esp-3",
      timestamp: 30000,
      current_state: "infected",
    });

    expect(finalState.game_over).toBe(true);
    expect(finalState.num_players).toBe(3);
    expect(finalState.num_infected).toBe(3);
    expect(finalState.num_humans).toBe(0);
    expect(finalState.rankings).toBeDefined();

    // Verify ranking order:
    // Rank 1: esp-3 (longest survived, infected at 30s)
    // Rank 2: esp-1 (infected at 15s)
    // Rank 3: esp-2 (earliest infected, infected at 5s)
    const rankings = finalState.rankings!;
    expect(rankings[0].device_id).toBe("esp-3");
    expect(rankings[0].rank).toBe(1);
    expect(rankings[0].survival_time_seconds).toBe(29);

    expect(rankings[1].device_id).toBe("esp-1");
    expect(rankings[1].rank).toBe(2);
    expect(rankings[1].survival_time_seconds).toBe(14);

    expect(rankings[2].device_id).toBe("esp-2");
    expect(rankings[2].rank).toBe(3);
    expect(rankings[2].survival_time_seconds).toBe(4);
  });
});

describe("Worker routes", () => {
  it("serves the default game on unprefixed paths", async () => {
    await SELF.fetch("https://example.com/add-player", { method: "POST" });
    const res = await SELF.fetch("https://example.com/population-state");

    expect(res.status).toBe(200);
    expect(await res.json<PopulationState>()).toMatchObject({ num_players: 1 });
  });

  it("routes /api/v1/games/{id} to that game", async () => {
    await SELF.fetch("https://example.com/api/v1/games/htn26/add-player", {
      method: "POST",
    });
    const res = await SELF.fetch("https://example.com/api/v1/games/htn26/population-state");

    expect(await res.json<PopulationState>()).toMatchObject({ num_players: 1 });

    // The default game is untouched by writes to htn26.
    const other = await SELF.fetch("https://example.com/api/v1/games/other/population-state");
    expect(await other.json<PopulationState>()).toMatchObject({ num_players: 0 });
  });

  it("answers CORS preflight", async () => {
    const res = await SELF.fetch("https://example.com/add-infected", { method: "OPTIONS" });
    expect(res.status).toBe(204);
    expect(res.headers.get("Access-Control-Allow-Origin")).toBe("*");
  });

  it("handles /device-event and serves /rankings", async () => {
    const postRes1 = await SELF.fetch("https://example.com/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        device_id: "node-alpha",
        timestamp: 1000,
        current_state: "not infected",
      }),
    });
    expect(postRes1.status).toBe(200);

    const postRes2 = await SELF.fetch("https://example.com/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        device_id: "node-beta",
        timestamp: 1000,
        current_state: "not infected",
      }),
    });
    expect(postRes2.status).toBe(200);

    // Infect alpha first
    await SELF.fetch("https://example.com/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        device_id: "node-alpha",
        timestamp: 5000,
        current_state: "infected",
      }),
    });

    // Infect beta last -> ends game
    const endRes = await SELF.fetch("https://example.com/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        device_id: "node-beta",
        timestamp: 20000,
        current_state: "infected",
      }),
    });
    const endBody = await endRes.json<PopulationState>();
    expect(endBody.game_over).toBe(true);

    // Query /rankings
    const rankRes = await SELF.fetch("https://example.com/rankings");
    expect(rankRes.status).toBe(200);
    const leaderboard = await rankRes.json<LeaderboardResponse>();
    expect(leaderboard.game_over).toBe(true);
    expect(leaderboard.total_players).toBe(2);
    expect(leaderboard.rankings[0].device_id).toBe("node-beta");
    expect(leaderboard.rankings[1].device_id).toBe("node-alpha");
  });

  it("pushes state to WebSocket clients on change", async () => {
    const res = await SELF.fetch("https://example.com/api/v1/games/ws-game/ws/population", {
      headers: { Upgrade: "websocket" },
    });
    expect(res.status).toBe(101);

    const ws = res.webSocket!;
    const received: PopulationState[] = [];
    ws.accept();
    ws.addEventListener("message", (event) => {
      received.push(JSON.parse(event.data as string));
    });

    // First frame is the snapshot sent on connect.
    await vi.waitFor(() => expect(received).toHaveLength(1));
    expect(received[0]).toMatchObject({ num_players: 0 });

    await SELF.fetch("https://example.com/api/v1/games/ws-game/add-player", {
      method: "POST",
    });

    await vi.waitFor(() => expect(received).toHaveLength(2));
    expect(received[1]).toMatchObject({ num_players: 1, num_humans: 1 });

    // When game ends via device event, WebSocket should push game_over and rankings
    await SELF.fetch("https://example.com/api/v1/games/ws-game/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        device_id: "ws-player-1",
        timestamp: 1000,
        current_state: "infected",
      }),
    });

    await vi.waitFor(() => expect(received.length).toBeGreaterThanOrEqual(3));
    const lastMsg = received[received.length - 1];
    expect(lastMsg.game_over).toBe(true);
    expect(lastMsg.rankings).toBeDefined();
    expect(lastMsg.rankings![0].device_id).toBe("ws-player-1");

    ws.close();
  });

  it("describes the endpoint on a plain GET to the WebSocket path", async () => {
    const res = await SELF.fetch("https://example.com/ws/population");
    expect(res.status).toBe(200);
    expect(await res.json()).toHaveProperty("current_state");
  });

  it("starts the game, assigns random patient zero, and delivers role to devices", async () => {
    // Register 3 devices
    await SELF.fetch("https://example.com/api/v1/games/start-test/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device_id: "node-1", timestamp: 1000, current_state: "not infected" }),
    });
    await SELF.fetch("https://example.com/api/v1/games/start-test/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device_id: "node-2", timestamp: 1000, current_state: "not infected" }),
    });
    await SELF.fetch("https://example.com/api/v1/games/start-test/device-event", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device_id: "node-3", timestamp: 1000, current_state: "not infected" }),
    });

    // Call start-game endpoint
    const startRes = await SELF.fetch("https://example.com/api/v1/games/start-test/start-game", {
      method: "POST",
    });
    expect(startRes.status).toBe(200);
    const startData = await startRes.json<{
      patient_zero_id: string;
      started_at: number;
      num_players: number;
      num_infected: number;
    }>();

    expect(startData.started_at).toBeGreaterThan(0);
    expect(startData.num_players).toBe(3);
    expect(startData.num_infected).toBe(1);
    expect(["node-1", "node-2", "node-3"]).toContain(startData.patient_zero_id);

    // Query device-state for the patient zero device
    const zeroRes = await SELF.fetch(
      `https://example.com/api/v1/games/start-test/device-state?device_id=${startData.patient_zero_id}`,
    );
    expect(zeroRes.status).toBe(200);
    const zeroState = await zeroRes.json<{
      device_id: string;
      role: string;
      is_infected: boolean;
      game_started: boolean;
    }>();
    expect(zeroState.role).toBe("infected");
    expect(zeroState.is_infected).toBe(true);
    expect(zeroState.game_started).toBe(true);

    // Query device-state for one of the uninfected devices
    const otherId = ["node-1", "node-2", "node-3"].find((id) => id !== startData.patient_zero_id)!;
    const otherRes = await SELF.fetch(
      `https://example.com/api/v1/games/start-test/device-state?device_id=${otherId}`,
    );
    expect(otherRes.status).toBe(200);
    const otherState = await otherRes.json<{
      device_id: string;
      role: string;
      is_infected: boolean;
      game_started: boolean;
    }>();
    expect(otherState.role).toBe("not infected");
    expect(otherState.is_infected).toBe(false);
    expect(otherState.game_started).toBe(true);
  });

  it("404s an unknown route", async () => {
    const res = await SELF.fetch("https://example.com/nope");
    expect(res.status).toBe(404);
  });
});

