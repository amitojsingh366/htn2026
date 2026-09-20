import { env, exports } from "cloudflare:workers";
import { runInDurableObject } from "cloudflare:test";
import { afterEach, describe, expect, it, vi } from "vitest";
import { GameTelemetry, Sentry, sentryOptions, scrubEvent } from "../src/telemetry";
import { HostGateway } from "../src/gateway";

const boot = "0123456789abcdef";
const fakeEnv = () => ({ ...env, SENTRY_DSN: "https://public@example.invalid/1", SENTRY_TRACES_SAMPLE_RATE: "1" });
const diagnostics = (overrides: Record<string, unknown> = {}) => ({
  v: 1, t: "diagnostics", id: 2, ts: Date.now(), host_boot: boot, round_id: null, fw: "1234abcd", seq: 1,
  uptime_ms: 60000, reconnects: 2, failures: 3, dropped_messages: 4, send_failures: 1,
  heap_free_bytes: 12345, heap_min_free_bytes: 10000, gateway_stack_free_bytes: 1000, last_error: 7,
  ...overrides,
});

afterEach(() => vi.restoreAllMocks());

describe("Sentry telemetry safety", () => {
  it("scrubs real SDK envelopes, including inherited baggage, and exports only allowlisted logs", async () => {
    const envelopes: unknown[] = [];
    await Sentry.withIsolationScope(async () => Sentry.withScope(async scope => {
      const client = new Sentry.CloudflareClient({
        ...sentryOptions(fakeEnv()), integrations: [], stackParser: () => [],
        transport: () => ({ send: async envelope => { envelopes.push(envelope); return { statusCode: 200 }; }, flush: async () => true }),
      });
      scope.setClient(client); client.init();
      Sentry.continueTrace({ sentryTrace: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-1",
        baggage: "sentry-trace_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,sentry-public_key=public,sentry-transaction=PRIVATE_SENTINEL,sentry-user_id=PRIVATE_SENTINEL" }, () => {
        Sentry.startSpan({ name: "GET /api/v1/games/PRIVATE_SENTINEL/population-state?token=PRIVATE_SENTINEL", op: "http.server" }, () => {
          Sentry.setTag("action_id", "01234567-89ab-cdef-0123-456789abcdef");
          Sentry.setTag("secret", "PRIVATE_SENTINEL");
          Sentry.captureEvent({
            exception: { values: [{ type: "Error", value: "PRIVATE_SENTINEL", stacktrace: { frames: [{ filename: "worker.js?PRIVATE_SENTINEL", vars: { token: "PRIVATE_SENTINEL" } }] } }] },
            user: { email: "PRIVATE_SENTINEL" }, request: { url: "https://PRIVATE_SENTINEL", headers: { Authorization: "PRIVATE_SENTINEL" }, data: "PRIVATE_SENTINEL" },
            contexts: { payload: { name: "PRIVATE_SENTINEL" } }, extra: { body: "PRIVATE_SENTINEL" }, breadcrumbs: [{ message: "PRIVATE_SENTINEL" }],
          });
          Sentry.logger.info("host.diagnostics", { host_boot: boot, uptime_ms: 60000, name: "PRIVATE_SENTINEL" });
          Sentry.logger.info("PRIVATE_SENTINEL");
        });
      });
      await client.flush(2000); client.dispose();
    }));
    const serialized = JSON.stringify(envelopes);
    expect(serialized).not.toContain("PRIVATE_SENTINEL");
    expect(serialized).toContain("host.diagnostics");
    expect(serialized).toContain("transaction");
    expect(serialized).toContain("exception");
    expect(serialized).toContain("01234567-89ab-cdef-0123-456789abcdef");
  });

  it("preserves important flow spans when a reconciliation exceeds the span budget", () => {
    const base = { trace_id: "a".repeat(32), span_id: "b".repeat(16), start_timestamp: 1, timestamp: 2, data: {} };
    const event = scrubEvent({ type: "transaction", transaction: "webSocketMessage", spans: [
      ...Array.from({ length: 150 }, () => ({ ...base, op: "db.query", description: "PRIVATE_SQL", data: { name: "PRIVATE_NAME" } })),
      { ...base, op: "game.infection.process", description: "game.infection.process" },
    ] } satisfies Sentry.Event);
    expect(event.spans).toHaveLength(100);
    expect(event.spans?.some(span => span.description === "game.infection.process")).toBe(true);
    expect(JSON.stringify(event)).not.toContain("PRIVATE_");
  });

  it("bounds game logs durably and reserves an independent diagnostics budget", async () => {
    const info = vi.spyOn(Sentry.logger, "info").mockImplementation(() => {});
    await runInDurableObject(env.GAME_ROOM.getByName("log-budget"), (_instance, state) => {
      const telemetry = new GameTelemetry(state, fakeEnv()); telemetry.initialize();
      for (let i = 0; i < 40; i++) telemetry.log("game.registered", { players: 1 });
      expect(info).toHaveBeenCalledTimes(30);
      telemetry.diagnostics(diagnostics(), 500, boot, null);
      telemetry.diagnostics(diagnostics({ seq: 2 }), 500, boot, null);
      expect(info).toHaveBeenCalledTimes(31);
      const recovered = new GameTelemetry(state, fakeEnv()); recovered.initialize();
      recovered.log("game.registered"); recovered.diagnostics(diagnostics({ seq: 3 }), 500, boot, null);
      expect(info).toHaveBeenCalledTimes(31);
      expect(info.mock.calls[30][0]).toBe("host.diagnostics");
      state.storage.sql.exec("UPDATE sentry_budget SET diagnostic_at=0 WHERE id=1");
      recovered.diagnostics(diagnostics(), 500, boot, null); // Duplicate survives reconnection.
      expect(info).toHaveBeenCalledTimes(31);
      recovered.diagnostics(diagnostics({ seq: 4 }), 500, boot, null);
      expect(info).toHaveBeenCalledTimes(32);
    });
  });

  it("silently rejects malformed, oversized, wrong-boot and wrong-round diagnostics", async () => {
    const info = vi.spyOn(Sentry.logger, "info").mockImplementation(() => {});
    await runInDurableObject(env.GAME_ROOM.getByName("diag-validation"), (_instance, state) => {
      const telemetry = new GameTelemetry(state, fakeEnv()); telemetry.initialize();
      telemetry.diagnostics(diagnostics(), 1537, boot, null);
      telemetry.diagnostics(diagnostics({ uptime_ms: -1 }), 500, boot, null);
      telemetry.diagnostics(diagnostics({ failures: 0x1_0000_0000 }), 500, boot, null);
      telemetry.diagnostics(diagnostics({ host_boot: "ffffffffffffffff" }), 500, boot, null);
      telemetry.diagnostics(diagnostics({ round_id: "ffffffffffffffff" }), 500, boot, null);
      telemetry.diagnostics(diagnostics({ fw: "Name and password" }), 500, boot, null);
      telemetry.diagnostics(diagnostics({ seq: 0 }), 500, boot, null);
      expect(info).not.toHaveBeenCalled();
      telemetry.diagnostics(diagnostics({ name: "PRIVATE_SENTINEL", token: "PRIVATE_SENTINEL", websocket_stack_free_bytes: 500, heap_largest_free_bytes: 8000 }), 800, boot, null);
      expect(info).toHaveBeenCalledOnce();
      expect(JSON.stringify(info.mock.calls)).not.toContain("PRIVATE_SENTINEL");
    });
  });

  it("requires the authenticated host handshake and diagnostics never send gameplay replies", async () => {
    const info = vi.spyOn(Sentry.logger, "info").mockImplementation(() => {});
    await runInDurableObject(env.GAME_ROOM.getByName("diag-protocol"), async (_instance, state) => {
      const telemetry = new GameTelemetry(state, fakeEnv()); telemetry.initialize();
      const changed = vi.fn();
      const gateway = new HostGateway(state, fakeEnv(), changed, telemetry); gateway.initialize();
      const game = "abcdef0123456789";
      const unauthorized = await gateway.fetch(new Request(`https://example.com/api/v1/games/${game}/gateway/socket`), game, "/gateway/socket");
      expect(unauthorized.status).toBe(401);
      let attachment: Record<string, unknown> = { is_gateway: true, gameId: game, welcomed: false, lastClientId: 0 };
      const send = vi.fn();
      const socket = { readyState: WebSocket.OPEN, deserializeAttachment: () => attachment, serializeAttachment: (a: Record<string, unknown>) => { attachment = a; }, send, close: vi.fn() } as unknown as WebSocket;
      await gateway.message(socket, JSON.stringify(diagnostics()));
      expect(info).not.toHaveBeenCalled(); expect(send).not.toHaveBeenCalled();
      await gateway.message(socket, JSON.stringify({ v: 1, t: "hello", id: 1, ts: Date.now(), proto: 1, game_id: game, host_id: env.ZT_HOST_MAC,
        host_boot: boot, fw: "1234abcd", round_id: null, state_rev: 0, pending_events: 0, decided_through: [], last_server_id: 0, channel: 1 }));
      expect(JSON.parse(send.mock.calls[0][0]).diagnostics).toBe(true);
      send.mockClear(); changed.mockClear(); info.mockClear();
      await gateway.message(socket, JSON.stringify(diagnostics()));
      await gateway.message(socket, JSON.stringify(diagnostics({ id: 3, seq: 2, failures: "invalid" })));
      await gateway.message(socket, JSON.stringify(diagnostics({ id: 4, seq: 3, padding: "x".repeat(1700) })));
      expect(info).toHaveBeenCalledOnce();
      expect(send).not.toHaveBeenCalled(); expect(changed).not.toHaveBeenCalled(); expect(socket.close).not.toHaveBeenCalled();
    });
  });

  it("permits trace headers in CORS and returns a validated action identifier", async () => {
    const options = await exports.default.fetch("https://example.com/population-state", { method: "OPTIONS" });
    expect(options.headers.get("Access-Control-Allow-Headers")).toContain("sentry-trace");
    expect(options.headers.get("Access-Control-Allow-Headers")).toContain("baggage");
    const response = await exports.default.fetch("https://example.com/population-state", { headers: { "x-action-id": "bad-name-secret" } });
    expect(response.headers.get("x-action-id")).toMatch(/^[0-9a-f-]{36}$/);
    expect(response.headers.get("Access-Control-Expose-Headers")).toContain("x-action-id");
  });
});
