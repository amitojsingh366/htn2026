import { env, runInDurableObject, SELF } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { HostGateway } from "../src/gateway";
import type { AnnouncementRequest } from "../src/director-types";

const GAME = "director-gateway-test";
const ROUND = "abcdef0123456789";
beforeEach(() => { vi.spyOn(globalThis, "fetch").mockRejectedValue(new Error("External network is disabled in gateway tests")); });
afterEach(() => { vi.restoreAllMocks(); });
type Fixture = { gateway: HostGateway; state: DurableObjectState; socket: WebSocket; now: number; request: AnnouncementRequest; messages: Array<Record<string, unknown>> };

function changeMeta(state: DurableObjectState, changes: Record<string, unknown>) {
  const current = JSON.parse(state.storage.sql.exec<{body:string}>("SELECT body FROM gateway_meta WHERE id=1").one().body);
  state.storage.sql.exec("UPDATE gateway_meta SET body=? WHERE id=1", JSON.stringify({ ...current, ...changes }));
}

async function withGateway(name: string, exercise: (fixture: Fixture) => void | Promise<void>) {
  const stub = env.GAME_ROOM.getByName(name);
  await runInDurableObject(stub, async (_instance, state) => {
    const now = Date.now();
    changeMeta(state, { gameId: GAME, roundId: ROUND, revision: 7, phase: "running", startSeq: 1,
      commandSeq: 1, startTime: now - 100_000, patientZero: 0, hostLastSeenAt: now,
      roster: [{ slot: 0, id: "001122334455", name: "Host" }, { slot: 1, id: "001122334466", name: "Human" }] });
    const pair = new WebSocketPair();
    state.acceptWebSocket(pair[1], ["gateway"]);
    pair[0].accept();
    pair[1].serializeAttachment({ is_gateway: true, gameId: GAME, welcomed: true, lastClientId: 0, lastSeenAt: now });
    const messages: Array<Record<string, unknown>> = [];
    pair[0].addEventListener("message", event => { messages.push(JSON.parse(String(event.data))); });
    const gateway = new HostGateway(state, { ...env, DIRECTOR_ENABLED: "true", DIRECTOR_GAME_ID: GAME }, () => {});
    const request = { actionId: "run:1:call:1", roundId: ROUND, revision: 7, text: "Two survivors remain. Stay alert!", expiresAt: now + 60_000 };
    try { await exercise({ gateway, state, socket: pair[1], now, request, messages }); }
    finally { pair[0].close(); pair[1].close(); }
  });
}

async function acknowledge(gateway: HostGateway, socket: WebSocket, seq: number, slot: number, id = 1) {
  await gateway.message(socket, JSON.stringify({ v: 1, t: "ack", id, ts: Date.now(), round_id: ROUND,
    applied: [{ seq, slot, state_rev: 7, result: "applied" }], ready: [], decision_applied: [], round_closed: [], presence: [] }));
}

describe("Director gateway integration", () => {
  it("reads canonical roles and only the most recent 40 events ordered by game time", async () => {
    await withGateway("director-observation", ({ gateway, state }) => {
      for (let index = 50; index > 0; index--) state.storage.sql.exec("INSERT INTO gateway_events VALUES(?,?,?,?,?,?,?,?,?)",
        `event-${index}`, ROUND, 1, index, JSON.stringify({ actor: 0, victim: 1, elapsed_ms: index * 1000 }),
        index <= 30 ? "accepted" : index <= 40 ? "rejected" : "pending_dependency", null, "{}", 1);
      const observation = gateway.directorObservation("room-id");
      expect(observation).toMatchObject({ roomId: "room-id", gameId: GAME, roundId: ROUND, revision: 7,
        phase: "running", players: 2, infected: 1, humans: 1, hostConnected: true,
        acceptedEvents: 30, rejectedEvents: 10, pendingEvents: 10 });
      expect(observation.history).toHaveLength(40);
      expect(observation.history[0].elapsedMs).toBe(11_000);
      expect(observation.history[39].elapsedMs).toBe(50_000);
      expect(JSON.stringify(observation)).not.toContain("001122334455");
    });
  });

  it("commits the command once, survives gateway recreation, and exposes real badge receipts", async () => {
    await withGateway("director-idempotency", async ({ gateway, state, socket, request, now }) => {
      const queued = gateway.sendDirectorAnnouncement(request);
      expect(queued).toMatchObject({ status: "queued", commandSeq: 2, acknowledged: [] });
      const command = JSON.parse(state.storage.sql.exec<{body:string}>("SELECT body FROM gateway_commands WHERE seq=2").one().body).commands[0];
      expect(command).toMatchObject({ type: "ANNOUNCE", text: request.text, target: 255, round_id: ROUND, valid_until_elapsed_ms: request.expiresAt - (now - 100_000) });
      const recovered = new HostGateway(state, { ...env, DIRECTOR_ENABLED: "true", DIRECTOR_GAME_ID: GAME }, () => {});
      expect(recovered.sendDirectorAnnouncement(request)).toMatchObject({ status: "duplicate", commandSeq: 2 });
      expect(recovered.sendDirectorAnnouncement({ ...request, text: "Different" })).toMatchObject({ status: "rejected", reason: "Action identity reused with different content" });
      expect(state.storage.sql.exec<{count:number}>("SELECT COUNT(*) AS count FROM gateway_commands").one().count).toBe(1);
      await acknowledge(gateway, socket, 2, 0);
      expect(recovered.directorAnnouncementResult(request.actionId)).toMatchObject({ acknowledged: [0] });
      await acknowledge(gateway, socket, 2, 1, 2);
      expect(recovered.directorAnnouncementResult(request.actionId)).toMatchObject({ acknowledged: [0, 1], reason: "Applied by all rostered badges" });
      expect(gateway.directorObservation("room").infected).toBe(1);
    });
  });

  it("rejects stale, disconnected, premature, expired, oversized and marked-up actions", async () => {
    await withGateway("director-guards", ({ gateway, state, socket, request, now }) => {
      const badRequests = [
        { ...request, revision: 6 }, { ...request, roundId: "ffffffffffffffff" },
        { ...request, expiresAt: now }, { ...request, expiresAt: now + 600_000 },
        { ...request, text: "x".repeat(97) }, { ...request, text: "Beware 🧟" },
        { ...request, text: "<b>Run</b>" }, { ...request, text: "**Run**" }, { ...request, text: "   " },
      ];
      badRequests.forEach((bad, index) => expect(gateway.sendDirectorAnnouncement({ ...bad, actionId: `invalid:${index}` }).status).toBe("rejected"));
      changeMeta(state, { hostLastSeenAt: now - 25_001 });
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "stale-host" }).reason).toBe("Host disconnected or stale");
      changeMeta(state, { hostLastSeenAt: now });
      const attachment = socket.deserializeAttachment() as Record<string, unknown>;
      socket.serializeAttachment({ ...attachment, welcomed: false });
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "disconnected-host" }).reason).toBe("Host disconnected or stale");
      socket.serializeAttachment(attachment);
      changeMeta(state, { hostLastSeenAt: now, startTime: now + 15_000 });
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "countdown" }).reason).toBe("Round is not active");
      changeMeta(state, { startTime: now - 100_000, phase: "final" });
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "closed-round" }).reason).toBe("Round is not active");
      expect(state.storage.sql.exec<{count:number}>("SELECT COUNT(*) AS count FROM gateway_commands").one().count).toBe(0);
    });
  });

  it("bounds outstanding announcements and preserves the command queue for gameplay", async () => {
    await withGateway("director-outbox", ({ gateway, state, request }) => {
      expect(gateway.sendDirectorAnnouncement(request).status).toBe("queued");
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "too-soon" }).reason).toContain("15 seconds");
      for (let index = 2; index <= 3; index++) {
        state.storage.sql.exec("UPDATE gateway_director_announcements SET created_at=created_at-15001 WHERE command_seq IS NOT NULL");
        expect(gateway.sendDirectorAnnouncement({ ...request, actionId: `queued:${index}` }).status).toBe("queued");
      }
      state.storage.sql.exec("UPDATE gateway_director_announcements SET created_at=created_at-15001 WHERE command_seq IS NOT NULL");
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "full" }).reason).toContain("queue is full");
      // A completed badge broadcast frees one slot even before its TTL elapses.
      state.storage.sql.exec("UPDATE gateway_commands SET acknowledged='[0,1]' WHERE seq=2");
      expect(gateway.sendDirectorAnnouncement({ ...request, actionId: "after-ack" }).status).toBe("queued");
      expect(gateway.directorObservation("room")).toMatchObject({ revision: 7, infected: 1, humans: 1 });
    });
  });

  it("drops expired and terminal-round announcements from replay while retaining gameplay commands", async () => {
    await withGateway("director-replay", async ({ gateway, state, socket, request, now, messages }) => {
      expect(gateway.sendDirectorAnnouncement(request).status).toBe("queued");
      await new Promise(resolve => setTimeout(resolve, 0));
      messages.length = 0;
      const row = state.storage.sql.exec<{body:string}>("SELECT body FROM gateway_commands WHERE seq=2").one();
      const body = JSON.parse(row.body);
      body.commands[0].valid_until_elapsed_ms = 99_999;
      state.storage.sql.exec("UPDATE gateway_commands SET body=? WHERE seq=2", JSON.stringify(body));
      state.storage.sql.exec("INSERT INTO gateway_commands VALUES(3,?,'[]')", JSON.stringify({ commands: [{ seq: 3, round_id: ROUND, type: "ROLE_SET", target: 1, role: "H", role_rev: 2, covered_seq: 0 }] }));
      const attachment = socket.deserializeAttachment() as Record<string, unknown>;
      socket.serializeAttachment({ ...attachment, commandSends: {} });
      await acknowledge(gateway, socket, 999, 0);
      await new Promise(resolve => setTimeout(resolve, 0));
      const replayed = messages.flatMap(message => (message.commands ?? []) as Array<{type:string}>);
      expect(replayed.some(command => command.type === "ANNOUNCE")).toBe(false);
      expect(replayed.some(command => command.type === "ROLE_SET")).toBe(true);

      messages.length = 0;
      body.commands[0].valid_until_elapsed_ms = 160_000;
      state.storage.sql.exec("UPDATE gateway_commands SET body=? WHERE seq=2", JSON.stringify(body));
      changeMeta(state, { phase: "final", end: { winner: "H", effectiveElapsed: 100_000, reason: "time_limit", final: true, missingSlots: 0, endSeq: 4 } });
      socket.serializeAttachment({ ...socket.deserializeAttachment() as Record<string, unknown>, commandSends: {} });
      await acknowledge(gateway, socket, 999, 0, 2);
      await new Promise(resolve => setTimeout(resolve, 0));
      expect(messages.flatMap(message => (message.commands ?? []) as Array<{type:string}>).some(command => command.type === "ANNOUNCE")).toBe(false);
      expect(gateway.directorAnnouncementResult(request.actionId).reason).toContain("round closed");
      expect(now).toBeLessThan(request.expiresAt);
    });
  });

  it("does not expose public agent mutation, tool or model trigger routes", async () => {
    for (const path of ["/agents/outbreak-director/arbitrary", "/api/v1/games/test/director/observe", "/api/v1/games/test/director"]) {
      expect((await SELF.fetch(`https://example.com${path}`, { method: "POST", body: "{}" })).status).toBe(404);
    }
    const response = await SELF.fetch("https://example.com/api/v1/games/unconfigured/director");
    expect(response.status).toBe(200);
    expect(await response.json()).toMatchObject({ enabled: false, status: "disabled" });
  });
});
