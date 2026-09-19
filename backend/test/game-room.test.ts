import { env, SELF } from "cloudflare:test";
import { describe, expect, it, vi } from "vitest";
import type { PopulationState } from "../src/types";

describe("GameRoom", () => {
  it("starts empty", async () => {
    const stub = env.GAME_ROOM.getByName("empty");
    expect(await stub.getState()).toEqual({
      num_players: 0,
      num_infected: 0,
      num_humans: 0,
      survived_pct: 0,
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
  });

  it("resets both counters", async () => {
    const stub = env.GAME_ROOM.getByName("reset");
    await stub.addPlayer();
    await stub.addInfected();
    await stub.reset();

    expect(await stub.getState()).toMatchObject({ num_players: 0, num_infected: 0 });
  });

  it("keeps games isolated from each other", async () => {
    const a = env.GAME_ROOM.getByName("game-a");
    const b = env.GAME_ROOM.getByName("game-b");
    await a.addPlayer();

    expect((await a.getState()).num_players).toBe(1);
    expect((await b.getState()).num_players).toBe(0);
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

    ws.close();
  });

  it("describes the endpoint on a plain GET to the WebSocket path", async () => {
    const res = await SELF.fetch("https://example.com/ws/population");
    expect(res.status).toBe(200);
    expect(await res.json()).toHaveProperty("current_state");
  });

  it("404s an unknown route", async () => {
    const res = await SELF.fetch("https://example.com/nope");
    expect(res.status).toBe(404);
  });
});
