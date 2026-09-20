import * as Sentry from "@sentry/cloudflare";

type Attributes = Record<string, string | number | boolean>;
type LogName = "game.registered" | "game.round_prepared" | "game.round_started" | "game.infections_processed" |
  "game.state_synced" | "connection.opened" | "connection.closed" | "connection.send_failed" |
  "gateway.rejected" | "host.diagnostics" | "backend.failure";
const LOG_NAMES = new Set<LogName>(["game.registered", "game.round_prepared", "game.round_started", "game.infections_processed",
  "game.state_synced", "connection.opened", "connection.closed", "connection.send_failed", "gateway.rejected", "host.diagnostics", "backend.failure"]);
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const HEX16 = /^[0-9a-f]{16}$/;
const NUMERIC_KEYS = new Set(["players", "infected", "events", "recipients", "failures", "close_code", "state_rev", "slot",
  "seq", "uptime_ms", "reconnects", "dropped_messages", "send_failures", "heap_free_bytes", "heap_min_free_bytes",
  "heap_largest_free_bytes", "gateway_stack_free_bytes", "websocket_stack_free_bytes", "last_error", "suppressed"]);

export function safeAttributes(input: Record<string, unknown> = {}): Attributes {
  const result: Attributes = {};
  for (const [key, value] of Object.entries(input)) {
    if (NUMERIC_KEYS.has(key) && typeof value === "number" && Number.isSafeInteger(value) && value >= 0) result[key] = value;
    else if (["round_id", "host_boot"].includes(key) && typeof value === "string" && HEX16.test(value)) result[key] = value;
    else if (key === "game_id" && typeof value === "string" && (HEX16.test(value) || value === "default" || value === "custom")) result[key] = value;
    else if (key === "action_id" && typeof value === "string" && UUID.test(value)) result[key] = value;
    else if (key === "fw" && typeof value === "string" && /^[a-zA-Z0-9._-]{8}$/.test(value)) result[key] = value;
    else if (key === "source" && ["worker", "durable_object", "host"].includes(String(value))) result[key] = String(value);
    else if (key === "connection" && ["host", "dashboard", "device"].includes(String(value))) result[key] = String(value);
    else if (key === "outcome" && ["registered", "rejoined", "accepted", "rejected", "failed"].includes(String(value))) result[key] = String(value);
    else if (key === "code" && typeof value === "string" && /^[A-Z_]{1,40}$/.test(value)) result[key] = value;
  }
  return result;
}

export function actionId(value: string | null | undefined): string | undefined { return value && UUID.test(value) ? value : undefined; }
export function setAction(value?: string): void { if (actionId(value)) Sentry.setTag("action_id", value!); }
export function setGame(gameId: string, roundId?: string | null, boot?: string): void {
  Sentry.setTags(safeAttributes({ game_id: HEX16.test(gameId) || gameId === "default" ? gameId : "custom", round_id: roundId, host_boot: boot }));
}

/** No free-form request paths, SQL, exception values, or payload-derived context leave the worker. */
export function scrubEvent<T extends Sentry.Event>(event: T): T {
  if (event.sdkProcessingMetadata) delete event.sdkProcessingMetadata.dynamicSamplingContext;
  delete event.request; delete event.user; delete event.extra; delete event.breadcrumbs; delete event.logentry;
  if (event.message) event.message = "Backend failure";
  event.tags = safeAttributes(event.tags ?? {});
  const trace = event.contexts?.trace;
  event.contexts = trace ? { trace: { trace_id: trace.trace_id, span_id: trace.span_id, parent_span_id: trace.parent_span_id,
    op: trace.op, status: trace.status, data: safeAttributes(trace.data) } } : {};
  for (const exception of event.exception?.values ?? []) {
    exception.value = "Backend operation failed (details omitted)";
    exception.type = /^[A-Za-z][A-Za-z0-9.]{0,60}$/.test(exception.type ?? "") ? exception.type : "Error";
    if (exception.mechanism) delete exception.mechanism.data;
    for (const frame of exception.stacktrace?.frames ?? []) {
      delete frame.vars; delete frame.pre_context; delete frame.post_context; delete frame.context_line;
      if (frame.filename) frame.filename = frame.filename.split(/[?#]/)[0];
    }
  }
  if (event.transaction) event.transaction = safeSpanName(event.transaction);
  if (event.spans) {
    // SDK spans finish from the inside out; retain the gameplay parent spans
    // even when an infection reconciliation produces many database children.
    const important = event.spans.filter(span => span.op?.startsWith("game."));
    const other = event.spans.filter(span => !span.op?.startsWith("game."));
    event.spans = [...important, ...other].slice(0, 100).sort((a, b) => a.start_timestamp - b.start_timestamp)
      .map(span => ({ ...span, description: safeSpanName(span.description ?? span.op ?? "operation"), data: safeAttributes(span.data) }));
  }
  return event;
}

function safeSpanName(value: string): string {
  if (/^game\.(registration|round\.prepare|round\.start|infection\.process|state\.sync)$/.test(value)) return value;
  if (/^(getState|getRankings|getDeviceState|startGame|recordDeviceEvent|addPlayer|addInfected|reset|webSocketMessage|webSocketClose|webSocketError|alarm)$/.test(value)) return value;
  const route = value.match(/(?:GET|POST|OPTIONS) (?:https?:\/\/[^/]+)?(?:\/api\/v1\/games\/[^/]+)?(\/(?:gateway\/(?:bootstrap|socket|control)|registrations|population-state|start-game|game\/start|device-event|esp\/event|device-state|add-player|add-infected|reset-game|reset-population|health|ws\/(?:population|device|esp)))(?:[?/#].*)?$/);
  return route ? `${value.startsWith("POST") ? "POST" : "GET"} ${route[1]}` : "backend.operation";
}

export function sentryOptions(env: Env): Sentry.CloudflareOptions {
  const sample = Number(env.SENTRY_TRACES_SAMPLE_RATE);
  // Options are per Worker invocation / DO instance, never shared global request state.
  let errorWindow = 0, errorCount = 0;
  return {
    dsn: env.SENTRY_DSN || undefined, enabled: Boolean(env.SENTRY_DSN),
    environment: env.SENTRY_ENVIRONMENT || "production", release: env.SENTRY_RELEASE || undefined,
    tracesSampleRate: Number.isFinite(sample) && sample >= 0 && sample <= 1 ? sample : 0.1,
    enableLogs: Boolean(env.SENTRY_DSN), enableMetrics: false, sendDefaultPii: false,
    dataCollection: { userInfo: false, cookies: false, httpHeaders: false, httpBodies: [], urlQueryParams: false,
      databaseQueryData: false, stackFrameVariables: false, frameContextLines: 0, graphQL: { document: false, variables: false }, genAI: { inputs: false, outputs: false } },
    maxBreadcrumbs: 0, sendClientReports: false, transportOptions: { bufferSize: 8 },
    enableRpcTracePropagation: true, rpcTracePropagationBindings: ["GAME_ROOM"],
    beforeSend: (event, hint) => {
      if (hint.originalException instanceof Error && hint.originalException.name === "GatewayError") return null;
      const window = Math.floor(Date.now() / 60_000);
      if (window !== errorWindow) { errorWindow = window; errorCount = 0; }
      if (++errorCount > 10) return null;
      return scrubEvent(event);
    },
    beforeSendTransaction: scrubEvent,
    beforeSendLog: log => LOG_NAMES.has(String(log.message) as LogName) ? { ...log, attributes: safeAttributes(log.attributes) } : null,
  };
}

export function reportFailure(error: unknown): void {
  if (error instanceof Error && error.name === "GatewayError") return;
  Sentry.captureException(error);
}

/** The SDK flushes via waitUntil; no Sentry I/O is awaited on gameplay paths. */
export class GameTelemetry {
  constructor(private ctx: DurableObjectState, private env: Env) {}
  initialize(): void {
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS sentry_budget (id INTEGER PRIMARY KEY CHECK(id=1), window INTEGER NOT NULL, count INTEGER NOT NULL, suppressed INTEGER NOT NULL, diagnostic_at INTEGER NOT NULL, boot TEXT NOT NULL, seq INTEGER NOT NULL)");
    this.ctx.storage.sql.exec("INSERT OR IGNORE INTO sentry_budget VALUES(1,0,0,0,0,'',0)");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS sentry_cooldowns (name TEXT PRIMARY KEY, last_at INTEGER NOT NULL)");
  }
  log(name: LogName, attributes: Record<string, unknown> = {}, level: "info" | "warn" | "error" = "info"): void {
    if (!this.env.SENTRY_DSN) return;
    // One durable row bounds logs to 30/min/game, even across reconnects/hibernation.
    try {
      const now = Date.now();
      const cooldown = name === "game.state_synced" || name.startsWith("connection.") ? 10_000 : name === "gateway.rejected" || name === "backend.failure" ? 5000 : 0;
      if (cooldown) {
        const last = this.ctx.storage.sql.exec<{last_at:number}>("SELECT last_at FROM sentry_cooldowns WHERE name=?", name).toArray()[0];
        if (last && now - last.last_at < cooldown) return;
        this.ctx.storage.sql.exec("INSERT INTO sentry_cooldowns VALUES(?,?) ON CONFLICT(name) DO UPDATE SET last_at=excluded.last_at", name, now);
      }
      const window = Math.floor(now / 60_000);
      const row = this.ctx.storage.sql.exec<{window:number;count:number;suppressed:number}>("SELECT window,count,suppressed FROM sentry_budget WHERE id=1").one();
      if (row.window === window && row.count >= 30) {
        this.ctx.storage.sql.exec("UPDATE sentry_budget SET suppressed=MIN(suppressed+1,4294967295) WHERE id=1"); return;
      }
      this.ctx.storage.sql.exec("UPDATE sentry_budget SET window=?,count=?,suppressed=0 WHERE id=1", window, row.window === window ? row.count + 1 : 1);
      Sentry.logger[level](name, safeAttributes({ ...Sentry.getCurrentScope().getScopeData().tags, ...attributes, suppressed: row.suppressed }));
    } catch { /* Telemetry must not affect gameplay or durable receipts. */ }
  }
  diagnostics(input: Record<string, unknown>, bytes: number, boot: string | undefined, round: string | null): void {
    if (!this.env.SENTRY_DSN || bytes > 1536 || input.host_boot !== boot || input.round_id !== round) return;
    const required = ["seq", "uptime_ms", "reconnects", "failures", "dropped_messages", "send_failures", "heap_free_bytes", "heap_min_free_bytes", "gateway_stack_free_bytes", "last_error"];
    const optional = ["websocket_stack_free_bytes", "heap_largest_free_bytes"];
    if (typeof input.fw !== "string" || !/^[a-zA-Z0-9._-]{8}$/.test(input.fw)) return;
    for (const field of [...required, ...optional.filter(key => input[key] !== undefined)]) {
      const value = input[field];
      if (typeof value !== "number" || !Number.isSafeInteger(value) || value < (field === "seq" ? 1 : 0) || value > (field === "uptime_ms" ? Number.MAX_SAFE_INTEGER : 0xffffffff)) return;
    }
    try {
      const row = this.ctx.storage.sql.exec<{diagnostic_at:number;boot:string;seq:number}>("SELECT diagnostic_at,boot,seq FROM sentry_budget WHERE id=1").one();
      const now = Date.now();
      if ((row.diagnostic_at && now - row.diagnostic_at < 30_000) || (row.boot === boot && Number(input.seq) <= row.seq)) return;
      this.ctx.storage.sql.exec("UPDATE sentry_budget SET diagnostic_at=?,boot=?,seq=? WHERE id=1", now, boot!, input.seq as number);
      // Diagnostics have their own reserved budget; gameplay logs cannot starve health reports.
      const health = Object.fromEntries([...required, ...optional, "fw", "host_boot", "round_id"].map(key => [key, input[key]]));
      Sentry.logger.info("host.diagnostics", safeAttributes({ ...Sentry.getCurrentScope().getScopeData().tags, ...health, source: "host" }));
    } catch { /* Best effort; never acknowledge, replay, or broadcast diagnostics. */ }
  }
}

export function stateTrace(round?: unknown): Record<string, string> | undefined {
  if (!Sentry.isEnabled()) return undefined;
  const trace = Sentry.getTraceData();
  const sentryTrace = trace["sentry-trace"];
  if (!sentryTrace) return undefined;
  const tags = Sentry.getCurrentScope().getScopeData().tags;
  return { trace_id: sentryTrace.split("-")[0], sentry_trace: sentryTrace,
    ...(actionId(String(tags.action_id ?? "")) ? { action_id: String(tags.action_id) } : {}),
    ...(typeof round === "string" && HEX16.test(round) ? { round_id: round } : {}) };
}

export { Sentry };
