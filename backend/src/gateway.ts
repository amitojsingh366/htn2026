import { Sentry, GameTelemetry, setGame, reportFailure } from "./telemetry";
/** Host-only gateway protocol; every event receipt follows durable ingestion. */
import type { AnnouncementRequest, AnnouncementResult, DirectorObservation } from "./director-types";

const RULES = { duration_ms: 600_000, tag_rssi: -58, tag_cooldown_ms: 3000 };
// One shared start timestamp for badges and dashboard after every PREPARE receipt.
// Demo countdown; use 60_000 here for the full-game head start.
const START_COUNTDOWN_MS = 5_000;
const ZERO = "0000000000000000";
const MAX_BYTES = 4096;
const RESET_PEER_WINDOW_MS = 15_000;
const COMMAND_BATCH_SIZE = 4; // Firmware gateway command queue capacity.
const REPLAY_RETRY_MS = 1000;
const CONTROL_HOST_FRESH_MS = 25_000;
const CONTROL_HISTORY_LIMIT = 512;
const DIRECTOR_ANNOUNCEMENT_INTERVAL_MS = 15_000;
const DIRECTOR_ANNOUNCEMENT_TTL_MS = 60_000;
const DIRECTOR_OUTBOX_LIMIT = 3;
const textEncoder = new TextEncoder();
type Json = Record<string, unknown>;
type Player = { slot: number; id: string; name: string };
type Phase = "lobby" | "prepared" | "running" | "expired_pending_sync" | "final";
type RoundEnd = { winner: "H" | "Z"; effectiveElapsed: number; reason: "time_limit" | "all_infected";
  final: boolean; missingSlots: number; endSeq: number; finalSeq?: number };
type Meta = {
  gameId: string; revision: number; serverSeq: number; commandSeq: number;
  snapshotId: number; roundId: string | null; phase: Phase; channel: number;
  roster: Player[]; rosterHash: string; prepareSeq: number; prepareSnapshot: number;
  startSeq: number; startTime: number; patientZero: number; ready: number[];
  resetSeq?: number;
  registrationIds?: Record<string, string>;
  hostBoot?: string;
  hostLastSeenAt?: number;
  resetDeadline?: number;
  resetOffered?: number[];
  resetCursor?: number;
  resetFinished?: boolean;
  end?: RoundEnd;
  closed?: Record<string, number>;
};
type Attachment = { is_gateway: true; gameId: string; welcomed: boolean; hostBoot?: string; lastClientId: number; lastSeenAt?: number; replayTurn?: number; commandCursor?: number; commandSends?: Record<string, number>; decisionCursor?: string; decisionRepeatAt?: number; needCursor?: number; needRepeatAt?: number; prepareSeeded?: number; endSent?: number; finalSent?: number; resetRound?: string };
type CommandRow = { seq: number; body: string; acknowledged: string };
type ControlRow = { request: string; response: string; status: number };
type AnnouncementRow = { request: string; result: string; command_seq: number | null; created_at: number; expires_at: number; round_id: string };
type Cause = { slot: number; seq: number };
type Role = { role: "H" | "Z"; role_rev: number; cause: Cause | null; covered_seq: number; infected_elapsed: number | null };
type Infection = { id: string; victim: number; seq: number; actor: number; actor_cause: Cause; actor_role_rev: number; victim_role_rev: number;
  attempt: { boot: string; seq: number }; elapsed_ms: number; uncertainty_ms: number; actor_rssi: number; victim_rssi: number };
type EventRow = { id: string; round_id: string; victim: number; seq: number; body: string; status: string; reason: string | null; decision: string; role_rev: number };
type Decision = { status: "accepted" | "rejected" | "pending_dependency"; reason: string | null };
function eventId(roundId: string, slot: number, seq: number): string { return `${roundId}/${slot.toString(16).padStart(2, "0")}/${seq.toString(16).padStart(4, "0")}`; }
function infection(value: unknown, roundId: string): Infection {
  const e = object(value), cause = object(e.actor_cause), attempt = object(e.attempt);
  const victim = integer(e.victim, 0, 19), seq = integer(e.seq, 1, 65535);
  if (e.id !== eventId(roundId, victim, seq)) throw new GatewayError("INVALID_PAYLOAD", "Event identity does not match its round, victim and sequence", 400);
  return { id: e.id as string, victim, seq, actor: integer(e.actor, 0, 19),
    actor_cause: { slot: integer(cause.slot, 0, 19), seq: integer(cause.seq, 0, 65535) },
    actor_role_rev: integer(e.actor_role_rev, 1, 65535), victim_role_rev: integer(e.victim_role_rev, 1, 65535),
    attempt: { boot: hex(attempt.boot, 16), seq: integer(attempt.seq, 1) },
    elapsed_ms: integer(e.elapsed_ms), uncertainty_ms: integer(e.uncertainty_ms, 0, 65535),
    actor_rssi: integer(e.actor_rssi, -128, 127), victim_rssi: integer(e.victim_rssi, -128, 127) };
}

export class GatewayError extends Error {
  constructor(readonly code: string, message: string, readonly status = 409) { super(message); this.name = "GatewayError"; }
}
function object(value: unknown): Json {
  if (typeof value !== "object" || value === null || Array.isArray(value)) throw new GatewayError("INVALID_PAYLOAD", "Expected an object", 400);
  return value as Json;
}
function integer(value: unknown, min = 0, max = 0xffffffff): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) throw new GatewayError("INVALID_PAYLOAD", "Integer out of range", 400);
  return value;
}
function hex(value: unknown, size: number): string {
  if (typeof value !== "string" || !new RegExp(`^[0-9a-f]{${size}}$`).test(value)) throw new GatewayError("INVALID_PAYLOAD", "Invalid identifier", 400);
  return value;
}
function ascii(value: unknown, min: number, max: number): string {
  if (typeof value !== "string" || value.length < min || value.length > max || /[^\x20-\x7e]/.test(value)) throw new GatewayError("INVALID_PAYLOAD", "Invalid printable text", 400);
  return value;
}
function boundedArray(value: unknown): unknown[] {
  if (!Array.isArray(value) || value.length > 8) throw new GatewayError("INVALID_PAYLOAD", "Array limit exceeded", 400);
  return value;
}
function round(value: unknown): string | null {
  if (value === null) return null;
  const id = hex(value, 16);
  if (id === ZERO) throw new GatewayError("INVALID_PAYLOAD", "Zero round identifier", 400);
  return id;
}
function initial(gameId = ""): Meta {
  return { gameId, revision: 1, serverSeq: 0, commandSeq: 0, snapshotId: 0, roundId: null, phase: "lobby", channel: 0,
    roster: [], rosterHash: ZERO, prepareSeq: 0, prepareSnapshot: 0, startSeq: 0, startTime: 0, patientZero: 255, ready: [] };
}
function json(body: unknown, status = 200): Response { return Response.json(body, { status }); }

async function readBody(request: Request): Promise<unknown> {
  if (!request.body) throw new GatewayError("INVALID_PAYLOAD", "Missing body", 400);
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    for (;;) {
      const chunk = await reader.read();
      if (chunk.done) break;
      size += chunk.value.byteLength;
      if (size > MAX_BYTES) { await reader.cancel(); throw new GatewayError("TOO_LARGE", "Body exceeds 4096 bytes", 413); }
      chunks.push(chunk.value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(size); let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  try { return JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes)); }
  catch { throw new GatewayError("INVALID_PAYLOAD", "Invalid JSON", 400); }
}

export class HostGateway {
  constructor(private ctx: DurableObjectState, private env: Env, private changed: () => void, private telemetry: GameTelemetry) {}

  initialize(): void {
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_meta (id INTEGER PRIMARY KEY CHECK(id=1), body TEXT NOT NULL)");
    this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_meta VALUES (1, ?)", JSON.stringify(initial()));
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_registrations (key TEXT PRIMARY KEY, request TEXT NOT NULL, response TEXT NOT NULL)");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_controls (request_id TEXT PRIMARY KEY, request TEXT NOT NULL, response TEXT NOT NULL, status INTEGER NOT NULL, created_at INTEGER NOT NULL)");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_director_announcements (action_id TEXT PRIMARY KEY, request TEXT NOT NULL, result TEXT NOT NULL, command_seq INTEGER, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL, round_id TEXT NOT NULL)");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_snapshots (snapshot_id INTEGER NOT NULL, page INTEGER NOT NULL, body TEXT NOT NULL, PRIMARY KEY(snapshot_id,page))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_commands (seq INTEGER PRIMARY KEY, body TEXT NOT NULL, acknowledged TEXT NOT NULL)");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_events (id TEXT PRIMARY KEY, round_id TEXT NOT NULL, victim INTEGER NOT NULL, seq INTEGER NOT NULL, body TEXT NOT NULL, status TEXT NOT NULL, reason TEXT, decision TEXT NOT NULL, role_rev INTEGER NOT NULL DEFAULT 0, UNIQUE(round_id,victim,seq))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_roles (round_id TEXT NOT NULL, slot INTEGER NOT NULL, body TEXT NOT NULL, PRIMARY KEY(round_id,slot))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_role_history (round_id TEXT NOT NULL, slot INTEGER NOT NULL, role_rev INTEGER NOT NULL, body TEXT NOT NULL, PRIMARY KEY(round_id,slot,role_rev))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_decision_acks (round_id TEXT NOT NULL, slot INTEGER NOT NULL, through_seq INTEGER NOT NULL, PRIMARY KEY(round_id,slot))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_conflicts (id TEXT NOT NULL, body TEXT NOT NULL, PRIMARY KEY(id,body))");
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS gateway_archives (round_id TEXT PRIMARY KEY, body TEXT NOT NULL, archived_at INTEGER NOT NULL)");
  }
  private get(): Meta { return JSON.parse(this.ctx.storage.sql.exec<{ body: string }>("SELECT body FROM gateway_meta WHERE id=1").one().body) as Meta; }
  private put(meta: Meta): void { this.ctx.storage.sql.exec("UPDATE gateway_meta SET body=? WHERE id=1", JSON.stringify(meta)); }
  enabled(): boolean { return this.get().roster.length > 0; }
  async recoverAlarm(): Promise<void> {
    const alarm = await this.ctx.storage.getAlarm();
    const m = this.get();
    const previousEnd = m.end?.endSeq;
    this.ctx.storage.transactionSync(() => { this.finishIfDue(m); this.put(m); });
    if (m.end?.endSeq !== previousEnd) this.announceCurrent();
    if (m.startSeq && !m.end && !m.resetSeq && alarm === null)
      await this.ctx.storage.setAlarm(Math.max(Date.now() + 1, m.startTime + RULES.duration_ms));
  }
  async alarm(): Promise<void> {
    const m = this.get();
    if (!m.startSeq || m.end || m.resetSeq) return;
    if (Date.now() < m.startTime + RULES.duration_ms) {
      await this.ctx.storage.setAlarm(m.startTime + RULES.duration_ms); return;
    }
    this.ctx.storage.transactionSync(() => { this.finishIfDue(m); this.put(m); });
    this.announceCurrent();
  }
  private role(m: Meta, slot: number): Role {
    const saved = m.roundId ? this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_roles WHERE round_id=? AND slot=?", m.roundId, slot).toArray()[0] : undefined;
    return saved ? JSON.parse(saved.body) as Role : { role: slot === m.patientZero ? "Z" : "H", role_rev: m.startSeq ? 1 : 0,
      cause: slot === m.patientZero ? { slot, seq: 0 } : null, covered_seq: 0, infected_elapsed: slot === m.patientZero ? 0 : null };
  }
  private saveRole(m: Meta, slot: number, role: Role): void {
    this.ctx.storage.sql.exec("INSERT INTO gateway_roles VALUES(?,?,?) ON CONFLICT(round_id,slot) DO UPDATE SET body=excluded.body", m.roundId, slot, JSON.stringify(role));
    this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_role_history VALUES(?,?,?,?)", m.roundId, slot, role.role_rev, JSON.stringify(role));
  }
  dashboard(): Json {
    const m = this.get();
    setGame(m.gameId || "default", m.roundId, m.hostBoot);
    const sockets = this.sockets();
    const start = m.startSeq ? this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq=?", m.startSeq).toArray()[0] : undefined;
    const startedSlots = start ? JSON.parse(start.acknowledged) as number[] : [];
    const resetRows = this.resetCommands(m);
    const events = this.ctx.storage.sql.exec<{status:string;count:number}>("SELECT status,COUNT(*) AS count FROM gateway_events WHERE round_id=? GROUP BY status", m.roundId ?? "").toArray();
    return { gateway_mode: m.roster.length > 0, gateway_phase: m.resetSeq ? "resetting" : m.phase, round_id: m.roundId,
      winner: m.end?.winner ?? null, ended_at: m.end ? m.startTime + m.end.effectiveElapsed : null,
      result_present: Boolean(m.end), result_final: Boolean(m.end?.final), result_complete: Boolean(m.end?.final && !m.end.missingSlots),
      missing_slots_bitmap: m.end?.missingSlots ?? 0, duration_ms: RULES.duration_ms,
      ready_players: m.ready.length, registered_players: m.roster.length,
      frozen_roster_players: m.roundId ? m.roster.length : 0,
      start_applied_players: startedSlots.length, ready_slots: m.ready, start_applied_slots: startedSlots,
      host_connected: sockets.length > 0,
      host_last_seen_at: Math.max(0, ...sockets.map(ws => (ws.deserializeAttachment() as Attachment).lastSeenAt ?? 0)) || null,
      reset_applied_players: resetRows.filter(r => (JSON.parse(r.acknowledged) as number[]).includes(r.target)).length,
      events_received: events.reduce((n, row) => n + row.count, 0),
      events_pending: events.find(r => r.status === "pending_dependency")?.count ?? 0,
      events_rejected: events.find(r => r.status === "rejected")?.count ?? 0,
      players: m.roster.map(p => ({ ...p, ...this.role(m, p.slot) })) };
  }
  /** Authoritative, bounded evidence for the director; excludes badge names and credentials. */
  directorObservation(roomId: string): DirectorObservation {
    const m = this.get();
    const counts = this.ctx.storage.sql.exec<{status:string;count:number}>(
      "SELECT status,COUNT(*) AS count FROM gateway_events WHERE round_id=? GROUP BY status", m.roundId ?? "").toArray();
    const history = this.ctx.storage.sql.exec<{id:string;body:string;status:string}>(
      "SELECT id,body,status FROM gateway_events WHERE round_id=? ORDER BY json_extract(body,'$.elapsed_ms') DESC,id DESC LIMIT 40", m.roundId ?? "").toArray().reverse().map(row => {
      const event = JSON.parse(row.body) as Infection;
      return { id: row.id, actor: event.actor, victim: event.victim, elapsedMs: event.elapsed_ms, status: row.status };
    });
    const infected = m.roster.filter(player => this.role(m, player.slot).role === "Z").length;
    return { roomId, gameId: m.gameId, roundId: m.roundId, revision: m.revision, observedAt: Date.now(),
      phase: m.resetSeq ? "resetting" : m.phase, startedAt: m.startTime || null,
      endedAt: m.end ? m.startTime + m.end.effectiveElapsed : null, durationMs: RULES.duration_ms,
      players: m.roster.length, infected, humans: m.roster.length - infected, winner: m.end?.winner ?? null,
      final: Boolean(m.end?.final), hostConnected: this.sockets().length > 0, hostLastSeenAt: m.hostLastSeenAt ?? null,
      pendingEvents: counts.find(row => row.status === "pending_dependency")?.count ?? 0,
      rejectedEvents: counts.find(row => row.status === "rejected")?.count ?? 0,
      acceptedEvents: counts.find(row => row.status === "accepted")?.count ?? 0, history };
  }
  directorAnnouncementResult(actionId: string): AnnouncementResult {
    const row = this.ctx.storage.sql.exec<AnnouncementRow>(
      "SELECT * FROM gateway_director_announcements WHERE action_id=?", actionId).toArray()[0];
    if (!row) return { status: "rejected", reason: "Unknown action" };
    const result = JSON.parse(row.result) as AnnouncementResult;
    if (!row.command_seq) return result;
    const command = this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq=?", row.command_seq).toArray()[0];
    const acknowledged = command ? JSON.parse(command.acknowledged) as number[] : [];
    const m = this.get();
    const roster = m.roundId === row.round_id ? m.roster : this.archive(row.round_id)?.roster ?? [];
    const delivered = roster.length > 0 && roster.every(player => acknowledged.includes(player.slot));
    const expired = Date.now() >= row.expires_at || m.roundId !== row.round_id || m.phase !== "running";
    return { ...result, status: delivered ? "applied" : expired ? "expired" : "queued", acknowledged, reason: delivered ? "Applied by all rostered badges" :
      expired ?
        "Expired or round closed; no further delivery attempts" : "Queued; awaiting badge acknowledgements" };
  }
  /** The action identity and command commit together before any gateway send. */
  sendDirectorAnnouncement(request: AnnouncementRequest): AnnouncementResult {
    if (typeof request?.actionId !== "string" || !/^[A-Za-z0-9/_:-]{1,160}$/.test(request.actionId))
      return { status: "rejected", reason: "Invalid action identity" };
    const canonical = JSON.stringify({ roundId: request.roundId, revision: request.revision, text: request.text, expiresAt: request.expiresAt });
    const saved = this.ctx.storage.sql.exec<AnnouncementRow>(
      "SELECT * FROM gateway_director_announcements WHERE action_id=?", request.actionId).toArray()[0];
    if (saved) {
      if (saved.request !== canonical) return { status: "rejected", reason: "Action identity reused with different content" };
      const result = this.directorAnnouncementResult(request.actionId);
      return result.status === "queued" ? { ...result, status: "duplicate" } : result;
    }
    const m = this.get(), now = Date.now();
    let reason: string | undefined;
    if (this.env.DIRECTOR_ENABLED !== "true" || m.gameId !== this.env.DIRECTOR_GAME_ID) reason = "Director disabled for this game";
    else if (!m.roundId || request.roundId !== m.roundId || request.revision !== m.revision) reason = "Stale round or revision";
    else if (m.phase !== "running" || m.resetSeq || m.end || !m.startSeq || now < m.startTime || now >= m.startTime + RULES.duration_ms) reason = "Round is not active";
    else if (!this.sockets().length || !m.hostLastSeenAt || now - m.hostLastSeenAt > CONTROL_HOST_FRESH_MS) reason = "Host disconnected or stale";
    else if (!Number.isSafeInteger(request.expiresAt) || request.expiresAt <= now || request.expiresAt > now + DIRECTOR_ANNOUNCEMENT_TTL_MS) reason = "Expiry must be within the next 60 seconds";
    else if (typeof request.text !== "string" || !request.text.trim() || request.text.length > 96 || /[^\x20-\x7e]|[<>`*_#\[\]{}]/.test(request.text)) reason = "Use 1 to 96 printable ASCII characters without markup";
    const recent = this.ctx.storage.sql.exec<AnnouncementRow>(
      "SELECT * FROM gateway_director_announcements WHERE round_id=? AND command_seq IS NOT NULL ORDER BY created_at DESC", m.roundId ?? "").toArray();
    const normalizedText = (text: string) => text.trim().replace(/\s+/g, " ").toLowerCase();
    if (!reason && recent.some(row => normalizedText((JSON.parse(row.request) as AnnouncementRequest).text) === normalizedText(request.text)))
      reason = "Announcement text already sent this round";
    if (!reason && recent.length && now - recent[0].created_at < DIRECTOR_ANNOUNCEMENT_INTERVAL_MS) reason = "Announcements require 15 seconds of spacing";
    if (!reason && recent.filter(row => {
      if (row.expires_at <= now) return false;
      const command = this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq=?", row.command_seq).toArray()[0];
      const acknowledged = command ? JSON.parse(command.acknowledged) as number[] : [];
      return m.roster.some(player => !acknowledged.includes(player.slot));
    }).length >= DIRECTOR_OUTBOX_LIMIT) reason = "Badge announcement queue is full";
    const expiresAt = Number.isSafeInteger(request.expiresAt) ? Math.min(request.expiresAt, m.startTime + RULES.duration_ms) : now;
    let result: AnnouncementResult = { status: "rejected", reason: reason ?? "Unable to queue announcement" };
    this.ctx.storage.transactionSync(() => {
      if (!reason) {
        const commandSeq = this.command(m, "ANNOUNCE", { text: request.text, valid_until_elapsed_ms: expiresAt - m.startTime });
        this.put(m);
        result = { status: "queued", reason: "Queued; awaiting badge acknowledgements", commandSeq, acknowledged: [] };
      }
      this.ctx.storage.sql.exec("INSERT INTO gateway_director_announcements VALUES(?,?,?,?,?,?,?)",
        request.actionId, canonical, JSON.stringify(result), result.commandSeq ?? null, now, expiresAt, typeof request.roundId === "string" ? request.roundId : "");
    });
    if (result.status === "queued") for (const ws of this.sockets()) this.replay(ws);
    return result;
  }
  assertLegacyMutation(): void {
    if (this.enabled()) throw new GatewayError("GATEWAY_GAME", "Live badge state is controlled by the host gateway.");
  }
  resetLobby(persistControl?: (next: Meta) => void): void {
    const m = this.get();
    this.ctx.storage.transactionSync(() => {
      if (m.roster.length) {
        // A lobby has no frozen round, but cleanup still needs a durable,
        // nonzero identity. Registration identities prevent reused slots from
        // clearing badges that have already joined a later lobby.
        const lobby = !m.roundId;
        m.roundId ??= crypto.randomUUID().replaceAll("-", "").slice(0, 16);
        m.registrationIds ??= {};
        for (const row of this.ctx.storage.sql.exec<{key:string;request:string}>("SELECT key,request FROM gateway_registrations ORDER BY rowid DESC").toArray()) {
          const request = JSON.parse(row.request) as {id:string};
          if (!m.registrationIds[request.id] && /^[0-9a-f]{16}$/.test(row.key) && row.key !== ZERO)
            m.registrationIds[request.id] = row.key;
        }
        m.resetSeq ??= m.commandSeq + 1;
        const existing = this.resetCommands(m);
        // Resetting server state never depends on badge acknowledgments. Keep
        // cleanup commands only for a stale host's separate catch-up connection.
        for (const player of m.roster) if (!existing.some(row => row.target === player.slot)) {
          const registrationId = m.registrationIds[player.id];
          if (lobby && !registrationId) continue; // Legacy lobby identities cannot safely be targeted.
          this.command(m, "RESET_GAME", { target: player.slot,
            ...(registrationId ? { target_mac: player.id, registration_id: registrationId } : {}) });
        }
        this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_archives VALUES(?,?,?)", m.roundId, JSON.stringify(m), Date.now());
      }
      this.ctx.storage.sql.exec("DELETE FROM gateway_registrations");
      this.ctx.storage.sql.exec("DELETE FROM players");
      this.ctx.storage.sql.exec("UPDATE population SET num_players=0,num_infected=0 WHERE id=1");
      this.ctx.storage.sql.exec("UPDATE game_meta SET started_at=NULL,ended_at=NULL,game_over=0,patient_zero_id=NULL WHERE id=1");
      const next = { ...initial(m.gameId), commandSeq: m.commandSeq, serverSeq: m.serverSeq, snapshotId: m.snapshotId,
        revision: m.revision + 1, hostBoot: m.hostBoot };
      this.put(next);
      persistControl?.(next);
    });
    for (const ws of this.ctx.getWebSockets("gateway")) {
      const a = ws.deserializeAttachment() as Attachment;
      ws.serializeAttachment({ ...a, welcomed: false });
      try { ws.close(1001, "Server reset completed; reconnect"); } catch { /* Already closed. */ }
    }
    this.changed();
  }
  private resetCommands(m: Meta): Array<CommandRow & {target:number}> {
    if (!m.resetSeq) return [];
    return this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq>=? ORDER BY seq", m.resetSeq).toArray().flatMap(row => {
      const c = (JSON.parse(row.body) as {commands:Array<{type:string;round_id:string;target:number}>}).commands[0];
      return c.type === "RESET_GAME" && c.round_id === m.roundId ? [{ ...row, target: c.target }] : [];
    });
  }
  private archive(roundId: string): Meta | undefined {
    const row = this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_archives WHERE round_id=?", roundId).toArray()[0];
    return row ? JSON.parse(row.body) as Meta : undefined;
  }
  private registrationCleanup(hostBoot: string, registrationId?: string): Meta | undefined {
    return this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_archives ORDER BY archived_at DESC").toArray()
      .map(row => JSON.parse(row.body) as Meta).find(archived => archived.resetSeq && !archived.resetFinished &&
        (registrationId ? archived.registrationIds?.[this.env.ZT_HOST_MAC] === registrationId :
          archived.hostBoot === hostBoot && !archived.roster.some(p => p.id === this.env.ZT_HOST_MAC)));
  }
  private sockets(): WebSocket[] {
    return this.ctx.getWebSockets("gateway").filter(ws => {
      const a = ws.deserializeAttachment() as Attachment | null;
      return ws.readyState === WebSocket.OPEN && a?.welcomed && !a.resetRound;
    });
  }
  private envelope(m: Meta, type: string, fields: Json): Json {
    if (m.serverSeq >= 0xffffffff) throw new GatewayError("INTERNAL", "Server sequence exhausted", 503);
    return { v: 1, t: type, id: ++m.serverSeq, ts: Date.now(), ...fields };
  }
  private send(ws: WebSocket, value: Json): void {
    const payload = JSON.stringify(value);
    if (textEncoder.encode(payload).length > MAX_BYTES) throw new Error("Gateway payload exceeds protocol bound");
    try { ws.send(payload); } catch { this.telemetry.log("connection.send_failed", { connection: "host" }, "warn"); /* Persisted messages replay on reconnect. */ }
  }
  private transient(ws: WebSocket, type: string, fields: Json): void {
    const m = this.get(); const value = this.envelope(m, type, fields); this.put(m); this.send(ws, value);
  }
  private syncPopulation(m: Meta): void {
    let infectedCount = 0;
    for (const player of m.roster) {
      const role = this.role(m, player.slot), infected = role.role === "Z";
      if (infected) infectedCount++;
      this.ctx.storage.sql.exec("INSERT INTO players(device_id,state,joined_at,infected_at) VALUES(?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET state=excluded.state, infected_at=excluded.infected_at",
        player.id, infected ? "infected" : "not infected", Date.now(), infected ? m.startTime + (role.infected_elapsed ?? 0) : null);
    }
    this.ctx.storage.sql.exec("UPDATE population SET num_players=?,num_infected=? WHERE id=1", m.roster.length, infectedCount);
    this.ctx.storage.sql.exec("UPDATE game_meta SET started_at=?,ended_at=?,game_over=?,patient_zero_id=? WHERE id=1",
      m.startTime || null, m.end ? m.startTime + m.end.effectiveElapsed : null, m.end ? 1 : 0,
      m.roster.find(p => p.slot === m.patientZero)?.id ?? null);
  }
  private snapshotFields(m: Meta, page: number): Json {
    const count = Math.max(1, Math.ceil(m.roster.length / 8));
    return { round_id: m.roundId, snapshot_id: m.snapshotId, state_rev: m.revision, roster_hash: m.rosterHash,
      roster_count: m.roster.length, page_index: page, page_count: count, next_page: page + 1 < count ? page + 1 : null,
      phase: m.end ? m.phase : m.startSeq ? "running" : "lobby", start_time_ms: m.startTime, end_time_ms: m.startTime ? m.startTime + RULES.duration_ms : 0,
      ...(m.end ? { result_present: true, result_final: m.end.final, result_complete: m.end.final && !m.end.missingSlots,
        winner: m.end.winner, effective_elapsed_ms: m.end.effectiveElapsed, missing_slots_bitmap: m.end.missingSlots } : {}),
      patient_zero_slot: m.patientZero, round_channel: m.channel, rules: RULES,
      players: m.roster.slice(page * 8, (page + 1) * 8).map(p => {
        const { infected_elapsed: _elapsed, ...role } = this.role(m, p.slot);
        return { ...p, ...role };
      }) };
  }
  private saveSnapshot(m: Meta): void {
    for (let page = 0; page < Math.ceil(m.roster.length / 8); page++) {
      const payload = this.envelope(m, "snapshot", this.snapshotFields(m, page));
      this.ctx.storage.sql.exec("INSERT INTO gateway_snapshots VALUES(?,?,?)", m.snapshotId, page, JSON.stringify(payload));
    }
  }
  private snapshot(ws: WebSocket, snapshotId: number, page?: number): void {
    const rows = page === undefined
      ? this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_snapshots WHERE snapshot_id=? ORDER BY page", snapshotId).toArray()
      : this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_snapshots WHERE snapshot_id=? AND page=?", snapshotId, page).toArray();
    if (!rows.length) { this.transient(ws, "error", { code: "CURSOR_EXPIRED", detail: "Request the current snapshot.", fatal: false }); return; }
    for (const row of rows) this.send(ws, JSON.parse(row.body) as Json);
  }
  private command(m: Meta, type: string, fields: Json): number {
    if (m.commandSeq >= 0xffffffff) throw new GatewayError("INTERNAL", "Command sequence exhausted", 503);
    const seq = ++m.commandSeq;
    const payload = this.envelope(m, "commands", { state_rev: m.revision, commands: [{ seq, round_id: m.roundId, type, target: 255, valid_until_elapsed_ms: 0xffffffff, ...fields }] });
    this.ctx.storage.sql.exec("INSERT INTO gateway_commands VALUES(?,?,?)", seq, JSON.stringify(payload), "[]");
    return seq;
  }
  private endCommand(m: Meta): void {
    const end = m.end!;
    end.endSeq = this.command(m, "END_ROUND", { effective_elapsed_ms: end.effectiveElapsed,
      reason: end.reason, winner: end.winner, provisional: !end.final });
  }
  private finishIfDue(m: Meta): void {
    if (!m.startSeq || m.resetSeq || m.end?.final) return;
    const allInfected = m.roster.length > 0 && m.roster.every(p => this.role(m, p.slot).role === "Z");
    if (!m.end && !allInfected && Date.now() < m.startTime + RULES.duration_ms) return;
    let changed = false;
    if (!m.end) {
      // Close gameplay immediately; evidence completeness is tracked separately.
      // The server's accepted roles are the only source of an all-zombie win.
      const elapsed = allInfected
        ? Math.min(RULES.duration_ms, Math.max(0, ...m.roster.map(p => this.role(m, p.slot).infected_elapsed ?? 0)))
        : RULES.duration_ms;
      m.end = { winner: allInfected ? "Z" : "H", effectiveElapsed: elapsed,
        reason: allInfected ? "all_infected" : "time_limit", final: false,
        missingSlots: (1 << m.roster.length) - 1, endSeq: 0 };
      m.phase = "expired_pending_sync"; changed = true;
    }
    const end = m.end;
    const winner = allInfected ? "Z" : "H";
    const winnerChanged = end.winner !== winner;
    end.winner = winner;
    let missing = 0;
    for (const player of m.roster) {
      const produced = m.closed?.[player.slot];
      if (produced === undefined || this.role(m, player.slot).covered_seq < produced) missing |= 1 << player.slot;
    }
    if (missing !== end.missingSlots) { end.missingSlots = missing; changed = true; }
    if (!missing) { end.final = true; m.phase = "final"; changed = true; }
    if (!changed && !winnerChanged) return;
    if (m.revision >= 0xffffffff || m.snapshotId >= 0xffffffff) throw new GatewayError("INTERNAL", "Result revision exhausted", 503);
    m.revision++; m.snapshotId++;
    if (!end.endSeq || winnerChanged) this.endCommand(m);
    if (end.final) end.finalSeq = this.command(m, "FINAL_RESULT", { winner: end.winner, complete: true,
      missing_slots_bitmap: 0, state_rev: m.revision });
    this.saveSnapshot(m); this.syncPopulation(m);
  }
  private eventRows(roundId: string): EventRow[] {
    return this.ctx.storage.sql.exec<EventRow>("SELECT * FROM gateway_events WHERE round_id=? ORDER BY victim,seq", roundId).toArray();
  }
  private eventDecision(m: Meta, e: Infection, rows: Map<string, EventRow>): Decision {
    const reject = (reason: string): Decision => ({ status: "rejected", reason });
    const pending: Decision = { status: "pending_dependency", reason: null };
    if (!m.roster.some(p => p.slot === e.victim)) return reject("NOT_ROSTERED");
    const victim = this.role(m, e.victim);
    // A missing earlier victim event might contain a rejected infection and its
    // canonical human correction. Arrival order cannot establish that history.
    if (e.seq !== victim.covered_seq + 1) return pending;
    if (!m.roster.some(p => p.slot === e.actor)) return reject("NOT_ROSTERED");
    if (!m.startSeq || m.end?.final || e.uncertainty_ms > 2000 || e.elapsed_ms + e.uncertainty_ms >= (m.end?.effectiveElapsed ?? RULES.duration_ms) ||
        Date.now() + e.uncertainty_ms < m.startTime + e.elapsed_ms) return reject("OUTSIDE_ROUND");
    if (e.actor === e.victim || e.actor_cause.slot !== e.actor) return reject("INVALID_PARENT");
    if (e.actor_rssi < RULES.tag_rssi || e.victim_rssi < RULES.tag_rssi || e.actor_rssi > 0 || e.victim_rssi > 0 || e.attempt.boot === ZERO) return reject("INVALID_PAYLOAD");
    if (victim.role !== "H") return reject("STALE_ROLE");
    if (e.victim_role_rev !== victim.role_rev) {
      const history = this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_role_history WHERE round_id=? AND slot=? AND role_rev=?", m.roundId, e.victim, e.victim_role_rev).toArray()[0];
      const initialHuman = e.victim_role_rev === 1 && e.victim !== m.patientZero;
      // A final rejection clears the provisional zombie before its separate
      // ROLE_SET arrives. A new tag can therefore still name that known human
      // revision. Contiguous victim sequences prevent a second winning tag.
      if (!initialHuman && (!history || (JSON.parse(history.body) as Role).role !== "H")) return reject("STALE_ROLE");
    }
    let provisionalActorRevision = 1;
    if (e.actor_cause.seq === 0) {
      if (e.actor !== m.patientZero) return reject("INVALID_PARENT");
    } else {
      const parentId = eventId(m.roundId!, e.actor, e.actor_cause.seq);
      const visiting = new Set<string>(), complete = new Set<string>();
      const stack: Array<{id:string;exit:boolean}> = [{ id: e.id, exit: false }];
      while (stack.length) {
        const step = stack.pop()!;
        if (step.exit) { visiting.delete(step.id); complete.add(step.id); continue; }
        if (complete.has(step.id)) continue;
        if (visiting.has(step.id)) return reject("INVALID_PARENT");
        const row = rows.get(step.id);
        if (!row) continue;
        visiting.add(step.id); stack.push({ id: step.id, exit: true });
        const body = JSON.parse(row.body) as Infection;
        if (body.actor_cause.seq) stack.push({ id: eventId(m.roundId!, body.actor, body.actor_cause.seq), exit: false });
        // Earlier victim events are also dependencies: they establish whether
        // a later infection followed a valid canonical correction to human.
        if (body.seq > 1) stack.push({ id: eventId(m.roundId!, body.victim, body.seq - 1), exit: false });
      }
      const parent = rows.get(parentId);
      if (!parent || parent.status === "pending_dependency") return pending;
      if (parent.status !== "accepted") return reject("INVALID_PARENT");
      const body = JSON.parse(parent.body) as Infection;
      if (e.elapsed_ms + e.uncertainty_ms < body.elapsed_ms - body.uncertainty_ms) return reject("INVALID_PARENT");
      provisionalActorRevision = body.victim_role_rev;
    }
    const actor = this.role(m, e.actor);
    if (actor.role !== "Z" || actor.cause?.slot !== e.actor || actor.cause.seq !== e.actor_cause.seq) return reject("INVALID_PARENT");
    const historical = this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_role_history WHERE round_id=? AND slot=? AND role_rev=?", m.roundId, e.actor, e.actor_role_rev).toArray()[0];
    const historyRole = historical ? JSON.parse(historical.body) as Role : undefined;
    const knownCanonicalRevision = actor.role_rev === e.actor_role_rev || (historyRole?.role === "Z" && historyRole.cause?.seq === e.actor_cause.seq);
    // Local offline infection preserves the previous revision until ROLE_SET.
    if (e.actor_role_rev !== provisionalActorRevision && !knownCanonicalRevision) return reject("STALE_ROLE");
    for (const row of rows.values()) {
      if (row.id === e.id || row.status !== "accepted") continue;
      const other = JSON.parse(row.body) as Infection;
      if (other.actor !== e.actor) continue;
      if (other.attempt.boot === e.attempt.boot && other.attempt.seq === e.attempt.seq) return reject("DUPLICATE_CONFLICT");
    }
    // Cooldown starts at the actor's first transmission. EVENT captures victim
    // reception, potentially a later retry, so it cannot prove that interval.
    // Firmware enforces cooldown; the server validates the evidence it has.
    return { status: "accepted", reason: null };
  }
  private settleEvents(m: Meta): void {
    const rows = new Map(this.eventRows(m.roundId!).map(row => [row.id, row]));
    let changed = false, progress = true;
    while (progress) {
      progress = false;
      for (const row of rows.values()) {
        if (row.status !== "pending_dependency") continue;
        const e = JSON.parse(row.body) as Infection, decision = this.eventDecision(m, e, rows);
        if (decision.status === "pending_dependency") continue;
        row.status = decision.status; row.reason = decision.reason;
        if (m.roster.some(p => p.slot === e.victim)) {
          const old = this.role(m, e.victim);
          if (old.role_rev >= 65535 || m.revision >= 0xffffffff) throw new GatewayError("INTERNAL", "Role or state revision exhausted", 503);
          const next: Role = { ...old, role_rev: old.role_rev + 1, covered_seq: e.seq };
          if (decision.status === "accepted") Object.assign(next, { role: "Z", cause: { slot: e.victim, seq: e.seq }, infected_elapsed: e.elapsed_ms });
          this.saveRole(m, e.victim, next); row.role_rev = next.role_rev; m.revision++;
          this.command(m, "ROLE_SET", { target: e.victim, role: next.role, role_rev: next.role_rev, cause: next.cause, covered_seq: next.covered_seq });
        }
        row.decision = JSON.stringify(this.envelope(m, "decisions", { decisions: [{ id: e.id, ...decision, archived: false }] }));
        this.ctx.storage.sql.exec("UPDATE gateway_events SET status=?,reason=?,decision=?,role_rev=? WHERE id=?", row.status, row.reason, row.decision, row.role_rev, row.id);
        progress = true; changed = true;
      }
    }
    if (changed) {
      if (m.snapshotId >= 0xffffffff) throw new GatewayError("INTERNAL", "Snapshot sequence exhausted", 503);
      m.snapshotId++; this.saveSnapshot(m); this.syncPopulation(m);
    }
    this.finishIfDue(m);
  }
  private ingest(ws: WebSocket, m: Meta, values: unknown[]): void {
    if (!m.startSeq) throw new GatewayError("WRONG_ROUND", "Infection evidence requires a started round", 409);
    if (!values.length) throw new GatewayError("INVALID_PAYLOAD", "An event batch cannot be empty", 400);
    const events = values.map(value => infection(value, m.roundId!));
    const existing = new Map(this.eventRows(m.roundId!).map(row => [row.id, row.body]));
    for (const e of events) {
      const body = JSON.stringify(e), prior = existing.get(e.id);
      if (prior && prior !== body) {
        this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_conflicts VALUES(?,?)", e.id, prior);
        this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_conflicts VALUES(?,?)", e.id, body);
        throw new GatewayError("INVALID_PAYLOAD", "DUPLICATE_CONFLICT: immutable event body differs; original evidence retained", 409);
      }
      existing.set(e.id, body);
    }
    if (existing.size > 2560) throw new GatewayError("RATE_LIMITED", "Round evidence capacity reached; retain pending events", 429);
    let response: Json;
    this.ctx.storage.transactionSync(() => {
      for (const e of events) {
        const status = m.end?.final ? "rejected" : "pending_dependency", reason = m.end?.final ? "OUTSIDE_ROUND" : null;
        const decision = this.envelope(m, "decisions", { decisions: [{ id: e.id, status, reason, archived: false }] });
        this.ctx.storage.sql.exec("INSERT OR IGNORE INTO gateway_events(id,round_id,victim,seq,body,status,reason,decision,role_rev) VALUES(?,?,?,?,?,?,?,?,0)",
          e.id, m.roundId, e.victim, e.seq, JSON.stringify(e), status, reason, JSON.stringify(decision));
      }
      this.settleEvents(m);
      response = this.envelope(m, "receipts", { received_events: [...new Set(events.map(e => e.id))] });
      this.put(m);
    });
    this.telemetry.log("game.infections_processed", { events: events.length, round_id: m.roundId, state_rev: m.revision });
    this.send(ws, response!); this.changed();
  }
  private replay(ws: WebSocket, replyWhenIdle = false): void {
    const m = this.get();
    const a = ws.deserializeAttachment() as Attachment;
    const saved = m.roundId ? this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq>=? ORDER BY seq", m.resetSeq ?? m.prepareSeq).toArray()
      .map(row => ({ ...row, command: (JSON.parse(row.body) as {commands:Array<Json & {round_id:string;target:number;type:string}>}).commands[0] })) : [];
    const latestRole = new Map<number, number>();
    for (const row of saved) if (row.command.round_id === m.roundId && row.command.type === "ROLE_SET") latestRole.set(row.command.target, row.seq);
    const commands = saved.filter(row => {
      const c = row.command;
      if (c.round_id !== m.roundId || (m.resetSeq ? c.type !== "RESET_GAME" : c.type === "RESET_GAME")) return false;
      if (c.type === "ANNOUNCE" && (m.phase !== "running" || m.end || Date.now() < m.startTime ||
        typeof c.valid_until_elapsed_ms !== "number" || Date.now() >= m.startTime + c.valid_until_elapsed_ms)) return false;
      // A reconnected host still needs the frozen PREPARE in its relay cache
      // for returning clients, even after every original readiness ACK arrived.
      if (m.startSeq && row.seq === m.prepareSeq) return a.prepareSeeded !== row.seq;
      // ROLE_SET carries the complete canonical role and covered frontier.
      // Older revisions cannot help a returning badge and may be rejected
      // forever after it has already learned the newer revision by snapshot.
      if (c.type === "ROLE_SET" && latestRole.get(c.target) !== row.seq) return false;
      if (m.end && ((c.type === "END_ROUND" && row.seq !== m.end.endSeq) || (c.type === "FINAL_RESULT" && row.seq !== m.end.finalSeq))) return false;
      const acknowledged = JSON.parse(row.acknowledged) as number[];
      return c.target === 255 ? m.roster.some(p => !acknowledged.includes(p.slot)) : !acknowledged.includes(c.target);
    });
    const now = Date.now();
    a.commandSends = Object.fromEntries(commands.filter(row => a.commandSends?.[row.seq] !== undefined).map(row => [row.seq, a.commandSends![row.seq]]));
    const readyCommands = commands.filter(row => now >= (a.commandSends?.[row.seq] ?? 0) + REPLAY_RETRY_MS);
    // A new result gets the first replay opportunity. Normal fair rotation still
    // retries it afterwards, including when the host's one-message RX is full.
    if (m.end && !m.resetSeq) {
      const results: typeof commands = [];
      for (const key of ["endSent", "finalSent"] as const) {
        const seq = key === "endSent" ? m.end.endSeq : m.end.finalSeq;
        const row = seq && a[key] !== seq ? commands.find(c => c.seq === seq) : undefined;
        if (row) { a[key] = row.seq; a.commandSends[row.seq] = now; results.push(row); }
      }
      if (results.length) {
        ws.serializeAttachment(a);
        this.transient(ws, "commands", { state_rev: m.revision, commands: results.map(row => row.command) }); return;
      }
    }
    const rows = m.roundId && !m.resetSeq ? this.eventRows(m.roundId) : [];
    const frontiers = new Map(this.ctx.storage.sql.exec<{slot:number;through_seq:number}>("SELECT slot,through_seq FROM gateway_decision_acks WHERE round_id=?", m.roundId ?? "").toArray().map(f => [f.slot, f.through_seq]));
    const decisions = rows.filter(row => row.seq > (frontiers.get(row.victim) ?? 0));
    const known = new Set(rows.map(row => row.id)), missing = new Set<string>();
    for (const player of m.roster) {
      const closed = m.closed?.[player.slot];
      if (closed === undefined) continue;
      const next = this.role(m, player.slot).covered_seq + 1;
      if (next <= closed && !known.has(eventId(m.roundId!, player.slot, next))) missing.add(eventId(m.roundId!, player.slot, next));
    }
    for (const row of rows.filter(r => r.status === "pending_dependency")) {
      const e = JSON.parse(row.body) as Infection;
      if (e.seq > 1) {
        const prior = eventId(m.roundId!, e.victim, this.role(m, e.victim).covered_seq + 1);
        if (!known.has(prior)) missing.add(prior);
      }
      if (e.actor_cause.seq) {
        const parent = eventId(m.roundId!, e.actor, e.actor_cause.seq);
        if (!known.has(parent)) missing.add(parent);
      }
    }
    // Rotate message classes and entries so an offline target or pending parent
    // cannot starve newer decisions, another target's command, or dependency repair.
    for (let attempt = 0; attempt < 3; attempt++) {
      const kind = (a.replayTurn ?? 0) % 3; a.replayTurn = kind + 1;
      if (kind === 0 && readyCommands.length) {
        const cursor = a.commandCursor ?? 0;
        const terminal = readyCommands.filter(row => row.command.type === "END_ROUND" || row.command.type === "FINAL_RESULT");
        const ordinary = readyCommands.filter(row => !terminal.includes(row));
        const selected = [...terminal, ...ordinary.filter(row => row.seq > cursor), ...ordinary.filter(row => row.seq <= cursor)].slice(0, COMMAND_BATCH_SIZE);
        for (const row of selected) a.commandSends[row.seq] = now;
        a.commandCursor = selected[selected.length - 1].seq; ws.serializeAttachment(a);
        this.transient(ws, "commands", { state_rev: m.revision, commands: selected.map(row => row.command) }); return;
      }
      if (kind === 1 && decisions.length) {
        const after = decisions.filter(d => d.id > (a.decisionCursor ?? ""));
        if (!after.length && now < (a.decisionRepeatAt ?? 0)) continue;
        const selected = (after.length ? after : decisions).slice(0, 8);
        a.decisionCursor = selected[selected.length - 1].id; a.decisionRepeatAt = now + REPLAY_RETRY_MS; ws.serializeAttachment(a);
        this.transient(ws, "decisions", { decisions: selected.map(r => ({ id: r.id, status: r.status, reason: r.reason, archived: false })) }); return;
      }
      if (kind === 2 && missing.size && now >= (a.needRepeatAt ?? 0)) {
        const ids = [...missing], cursor = (a.needCursor ?? 0) % ids.length;
        const selected = Array.from({ length: Math.min(8, ids.length) }, (_, i) => ids[(cursor + i) % ids.length]);
        a.needCursor = (cursor + selected.length) % ids.length;
        if (!a.needCursor) a.needRepeatAt = now + REPLAY_RETRY_MS;
        ws.serializeAttachment(a); this.transient(ws, "need_events", { need_events: selected }); return;
      }
    }
    ws.serializeAttachment(a);
    // Explicit idle replies release the gateway's single outstanding request
    // without another timeout or an unbounded stream of empty polling replies.
    if (replyWhenIdle) this.transient(ws, "commands", { state_rev: m.revision, commands: [] });
  }
  private announceCurrent(): void { for (const ws of this.sockets()) this.replay(ws); this.changed(); }

  private archivedResetMessage(ws: WebSocket, b: Json, archived: Meta): void {
    if (round(b.round_id) !== archived.roundId) throw new GatewayError("WRONG_ROUND", "Reset cleanup must name its archived round");
    const host = archived.roster.find(p => p.id === this.env.ZT_HOST_MAC);
    const commands = this.resetCommands(archived);
    let hostApplied = false;
    if (b.t === "ack") {
      const applied = boundedArray(b.applied);
      for (const field of ["ready", "decision_applied", "round_closed", "presence"]) boundedArray(b[field]);
      this.ctx.storage.transactionSync(() => {
        for (const value of applied) {
          const receipt = object(value), seq = integer(receipt.seq, 1), slot = integer(receipt.slot, 0, 19);
          integer(receipt.state_rev);
          const row = commands.find(c => c.seq === seq && c.target === slot);
          if (!row || receipt.result !== "applied") continue;
          const slots = JSON.parse(row.acknowledged) as number[];
          if (!slots.includes(slot)) slots.push(slot);
          row.acknowledged = JSON.stringify(slots);
          this.ctx.storage.sql.exec("UPDATE gateway_commands SET acknowledged=? WHERE seq=?", row.acknowledged, seq);
          if (slot === host?.slot) hostApplied = true;
        }
      });
    } else if (b.t !== "need" && b.t !== "events") {
      throw new GatewayError("INVALID_PAYLOAD", "Archived sessions only deliver operator reset cleanup", 400);
    }
    const peers = commands.filter(row => row.target !== host?.slot && !(JSON.parse(row.acknowledged) as number[]).includes(row.target));
    const offered = new Set(archived.resetOffered ?? []);
    const unoffered = peers.filter(row => !offered.has(row.seq));
    const now = Date.now();
    archived.resetDeadline ??= now + RESET_PEER_WINDOW_MS;
    // Give every peer a send opportunity, then allow a bounded retry window.
    // The active server lobby has already cleared; missing peers cannot hold it
    // or the host's own reset indefinitely. Send one bounded frame per request
    // so the firmware's single-message RX buffer is not flooded.
    const peerWindow = peers.length > 0 && (unoffered.length > 0 || now < archived.resetDeadline);
    let selected: Array<CommandRow & {target:number}> = [];
    if (!hostApplied && peerWindow) {
      const cursor = archived.resetCursor ?? 0;
      const ordered = [...peers.filter(row => row.seq > cursor), ...peers.filter(row => row.seq <= cursor)];
      selected = (unoffered.length ? unoffered : ordered).slice(0, COMMAND_BATCH_SIZE);
      for (const row of selected) offered.add(row.seq);
      archived.resetOffered = [...offered];
      archived.resetCursor = selected[selected.length - 1].seq;
    } else if (!hostApplied) {
      const cleanup = commands.find(row => row.target === host?.slot);
      if (cleanup) selected = [cleanup];
    }
    if (hostApplied || !selected.length) archived.resetFinished = true;
    this.ctx.storage.sql.exec("UPDATE gateway_archives SET body=? WHERE round_id=?", JSON.stringify(archived), archived.roundId);
    if (archived.resetFinished) {
      const a = ws.deserializeAttachment() as Attachment;
      ws.serializeAttachment({ ...a, welcomed: false });
      ws.close(1001, "Archived reset cleanup complete; reconnect to current lobby");
      return;
    }
    this.transient(ws, "commands", { state_rev: archived.revision,
      commands: selected.map(row => (JSON.parse(row.body) as {commands:Json[]}).commands[0]) });
  }

  private authenticate(request: Request): Response | null {
    const token = this.env.ZT_GATEWAY_TOKEN;
    const host = this.env.ZT_HOST_MAC;
    if (!token || !host || !/^[0-9a-f]{12}$/.test(host)) return json({ v: 1, code: "BAD_CONFIGURATION" }, 503);
    const expected = textEncoder.encode(`Bearer ${token}`);
    const actual = textEncoder.encode(request.headers.get("Authorization") ?? "");
    if (actual.length !== expected.length || !crypto.subtle.timingSafeEqual(actual, expected)) return json({ v: 1, code: "UNAUTHORIZED" }, 401);
    return null;
  }
  async fetch(request: Request, gameId: string, route: string): Promise<Response> {
    const failure = this.authenticate(request); if (failure) return failure;
    setGame(gameId);
    try {
      hex(gameId, 16);
      const m = this.get();
      if (!m.gameId) { m.gameId = gameId; this.put(m); }
      if (m.gameId !== gameId) throw new GatewayError("INVALID_PAYLOAD", "Wrong game", 404);
      const url = new URL(request.url);
      if (route === "/gateway/bootstrap" && request.method === "GET") {
        if (url.searchParams.get("host_id") !== this.env.ZT_HOST_MAC) return json({ v: 1, code: "HOST_MISMATCH" }, 403);
        const snapshot = m.roundId
          ? JSON.parse(this.ctx.storage.sql.exec<{ body: string }>("SELECT body FROM gateway_snapshots WHERE snapshot_id=? AND page=0", m.snapshotId).one().body) as Json
          : { ...this.snapshotFields({ ...m, roster: [], rosterHash: ZERO, snapshotId: 0, channel: 0 }, 0) };
        const { t: _t, id: _id, ts: _ts, ...fields } = snapshot;
        return json({ ...fields, v: 1, game_id: gameId, host_id: this.env.ZT_HOST_MAC, server_time_ms: Date.now(), max_players: 20,
          socket_path: `/api/v1/games/${gameId}/gateway/socket` });
      }
      if (route === "/registrations" && request.method === "POST") return await Sentry.startSpan({ name: "game.registration", op: "game.registration" }, async () => this.register(await readBody(request), request.headers.get("Idempotency-Key")));
      if (route === "/gateway/control" && request.method === "POST") return await this.control(await readBody(request), gameId);
      if (route === "/gateway/socket" && request.method === "GET") {
        if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return json({ v: 1, code: "UPGRADE_REQUIRED" }, 426);
        if (!request.headers.get("Sec-WebSocket-Protocol")?.split(",").map(s => s.trim()).includes("zt.v1")) return json({ v: 1, code: "INVALID_PAYLOAD" }, 400);
        const pair = new WebSocketPair();
        this.ctx.acceptWebSocket(pair[1], ["gateway"]);
        pair[1].serializeAttachment({ is_gateway: true, gameId, welcomed: false, lastClientId: 0 } satisfies Attachment);
        return new Response(null, { status: 101, webSocket: pair[0], headers: { "Sec-WebSocket-Protocol": "zt.v1" } });
      }
      return json({ v: 1, code: "NOT_FOUND" }, 404);
    } catch (error) {
      if (error instanceof GatewayError) return json({ v: 1, code: error.code, detail: error.message }, error.status);
      throw error;
    }
  }
  private controlRow(requestId: string, canonical: string): ControlRow | undefined {
    const saved = this.ctx.storage.sql.exec<ControlRow>("SELECT request,response,status FROM gateway_controls WHERE request_id=?", requestId).toArray()[0];
    if (saved && saved.request !== canonical) throw new GatewayError("BAD_CONFIGURATION", "Control request identity reused with a different payload");
    return saved;
  }
  private saveControl(requestId: string, canonical: string, body: Json, status: number): void {
    this.ctx.storage.sql.exec("INSERT INTO gateway_controls VALUES(?,?,?,?,?)", requestId, canonical, JSON.stringify(body), status, Date.now());
    this.ctx.storage.sql.exec("DELETE FROM gateway_controls WHERE request_id IN (SELECT request_id FROM gateway_controls ORDER BY created_at DESC,rowid DESC LIMIT -1 OFFSET ?)", CONTROL_HISTORY_LIMIT);
  }
  private async control(input: unknown, gameId: string): Promise<Response> {
    const b = object(input);
    if (b.v !== 1 || (b.action !== "reset" && b.action !== "start")) throw new GatewayError("INVALID_PAYLOAD", "Invalid host control", 400);
    const action = b.action, requestId = hex(b.request_id, 16), registrationId = hex(b.registration_id, 16), expectedRound = round(b.round_id);
    if (requestId === ZERO || registrationId === ZERO) throw new GatewayError("INVALID_PAYLOAD", "Zero control or registration identity", 400);
    const canonical = JSON.stringify({ action, registrationId, roundId: expectedRound });
    const saved = this.controlRow(requestId, canonical);
    if (saved) return json(JSON.parse(saved.response), saved.status);
    const accepted = (next: Meta): void => this.saveControl(requestId, canonical,
      { v: 1, accepted: true, action, request_id: requestId, round_id: next.roundId, state_rev: next.revision }, 200);
    try {
      const m = this.get();
      if (!m.roster.some(player => player.id === this.env.ZT_HOST_MAC) || m.registrationIds?.[this.env.ZT_HOST_MAC] !== registrationId)
        throw new GatewayError("STALE_REGISTRATION", "Register the host in this lobby before using its controls");
      if (action === "reset") {
        if (!expectedRound || m.roundId !== expectedRound) throw new GatewayError("WRONG_ROUND", "Reset must name the current completed round");
        if (!m.end) throw new GatewayError("ROUND_ACTIVE", "The host can reset after the server has declared a winner");
        // The accepted response commits in the same transaction as the reset.
        // Lost HTTPS responses can be retried without clearing a later lobby.
        this.resetLobby(accepted);
      } else {
        if (expectedRound !== null || m.roundId) throw new GatewayError("WRONG_ROUND", "Start requires the current lobby");
        await this.prepare(gameId, { registrationId, persist: accepted });
      }
    } catch (error) {
      // A concurrent duplicate may have completed during the roster digest.
      const completed = this.controlRow(requestId, canonical);
      if (completed) return json(JSON.parse(completed.response), completed.status);
      if (!(error instanceof GatewayError) || error.status >= 500) throw error;
      const m = this.get();
      this.ctx.storage.transactionSync(() => this.saveControl(requestId, canonical,
        { v: 1, accepted: false, action, request_id: requestId, code: error.code, detail: error.message,
          round_id: m.roundId, state_rev: m.revision }, error.status));
    }
    const completed = this.controlRow(requestId, canonical)!;
    return json(JSON.parse(completed.response), completed.status);
  }
  private register(input: unknown, key: string | null): Response {
    const b = object(input);
    if (b.v !== 1) throw new GatewayError("INVALID_PAYLOAD", "Invalid version", 400);
    const id = hex(b.badge_id, 12), name = ascii(b.name, 1, 12), fw = ascii(b.fw, 8, 8), known = round(b.known_round_id);
    if (!key || !/^[A-Za-z0-9._:-]{1,128}$/.test(key)) throw new GatewayError("INVALID_PAYLOAD", "Missing or invalid Idempotency-Key", 400);
    const canonical = JSON.stringify({ id, name, fw, known });
    const prior = this.ctx.storage.sql.exec<{request:string;response:string}>("SELECT request,response FROM gateway_registrations WHERE key=?", key).toArray()[0];
    if (prior) {
      if (prior.request !== canonical) throw new GatewayError("BAD_CONFIGURATION", "Idempotency key reused for another request");
      return json(JSON.parse(prior.response));
    }
    const m = this.get();
    // A delayed pre-reset HTTP retry must not silently repopulate the new
    // lobby. Pressing A again creates a new registration identity.
    const retired = this.ctx.storage.sql.exec<{body:string}>("SELECT body FROM gateway_archives").toArray()
      .some(row => (JSON.parse(row.body) as Meta).registrationIds?.[id] === key);
    if (retired) return json({ v: 1, code: "BAD_CONFIGURATION", round_id: m.roundId, state_rev: m.revision }, 409);
    let player = m.roster.find(p => p.id === id);
    if (!player && m.roundId) return json({ v: 1, code: "REGISTRATION_CLOSED", round_id: m.roundId, state_rev: m.revision }, 409);
    if (known && known !== m.roundId) return json({ v: 1, code: "BAD_CONFIGURATION", round_id: m.roundId, state_rev: m.revision }, 409);
    const existing = Boolean(player);
    if (!player) {
      if (m.roster.length >= 20) return json({ v: 1, code: "ROOM_FULL", round_id: m.roundId, state_rev: m.revision }, 409);
      player = { slot: m.roster.length, id, name }; m.roster.push(player); m.revision++;
    }
    if (/^[0-9a-f]{16}$/.test(key) && key !== ZERO) (m.registrationIds ??= {})[id] = key;
    const response = { v: 1, status: existing ? "rejoined" : "registered", slot: player.slot, round_id: m.roundId, state_rev: m.revision };
    this.ctx.storage.transactionSync(() => {
      this.put(m); this.syncPopulation(m);
      this.ctx.storage.sql.exec("INSERT INTO gateway_registrations VALUES(?,?,?)", key, canonical, JSON.stringify(response));
    });
    this.telemetry.log("game.registered", { players: m.roster.length, slot: player.slot, outcome: existing ? "rejoined" : "registered", round_id: m.roundId });
    this.changed();
    return json(response, existing ? 200 : 201);
  }

  async prepare(gameId: string, control?: { registrationId: string; persist: (next: Meta) => void }): Promise<Json> {
    return Sentry.startSpan({ name: "game.round.prepare", op: "game.round.prepare" }, () => this.prepareImpl(gameId, control));
  }
  private async prepareImpl(gameId: string, control?: { registrationId: string; persist: (next: Meta) => void }): Promise<Json> {
    let m = this.get();
    if (m.roundId) return this.startResult(m);
    if (m.roster.length < 2 || m.roster.length > 20) throw new GatewayError("ROSTER_SIZE", "Register 2–20 badges before starting.");
    if (!m.roster.some(p => p.id === this.env.ZT_HOST_MAC)) throw new GatewayError("HOST_NOT_REGISTERED", "Press A on the host badge to register it before starting.");
    const recentlyConnected = control && m.hostLastSeenAt !== undefined && Date.now() >= m.hostLastSeenAt && Date.now() - m.hostLastSeenAt <= CONTROL_HOST_FRESH_MS;
    if ((!this.sockets().length && !recentlyConnected) || m.channel < 1 || m.channel > 11)
      throw new GatewayError("HOST_OFFLINE", "Connect the designated host before starting.", control ? 503 : 409);
    const canonical = new Uint8Array(m.roster.length * 20);
    for (const [i, player] of m.roster.entries()) {
      const offset = i * 20; canonical[offset] = player.slot;
      for (let byte = 0; byte < 6; byte++) canonical[offset + 1 + byte] = parseInt(player.id.slice(byte * 2, byte * 2 + 2), 16);
      canonical[offset + 7] = player.name.length; canonical.set(textEncoder.encode(player.name), offset + 8);
    }
    const revision = m.revision;
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", canonical));
    m = this.get();
    if (control && m.registrationIds?.[this.env.ZT_HOST_MAC] !== control.registrationId)
      throw new GatewayError("STALE_REGISTRATION", "The host registration changed before Start completed");
    if (m.roundId) {
      if (control) throw new GatewayError("WRONG_ROUND", "Another Start already prepared this lobby");
      return this.startResult(m);
    }
    if (m.revision !== revision) throw new GatewayError("ROSTER_CHANGED", "The roster changed. Press Start again.");
    m.gameId = gameId; m.roundId = crypto.randomUUID().replaceAll("-", "").slice(0, 16); m.phase = "prepared";
    m.rosterHash = Array.from(digest.slice(0, 8)).reverse().map(b => b.toString(16).padStart(2, "0")).join("");
    m.revision++; m.snapshotId++; m.prepareSnapshot = m.snapshotId;
    this.ctx.storage.transactionSync(() => {
      this.saveSnapshot(m);
      m.prepareSeq = this.command(m, "PREPARE_ROUND", { snapshot_id: m.snapshotId, roster_hash: m.rosterHash, roster_count: m.roster.length, ...RULES, channel: m.channel });
      this.put(m);
      control?.persist(m);
    });
    setGame(m.gameId, m.roundId, m.hostBoot);
    this.telemetry.log("game.round_prepared", { players: m.roster.length, state_rev: m.revision });
    for (const ws of this.sockets()) this.snapshot(ws, m.snapshotId, 0);
    this.changed();
    return this.startResult(m);
  }
  private startResult(m: Meta): Json {
    return { message: m.startSeq ? "Start scheduled. Patient Zero is fixed for this round." : `Preparing badges: ${m.ready.length}/${m.roster.length} ready.`,
      game_id: m.gameId, phase: m.phase, round_id: m.roundId, started_at: m.startTime || null,
      patient_zero_id: m.roster.find(p => p.slot === m.patientZero)?.id ?? null, ready_players: m.ready.length, num_players: m.roster.length, num_infected: m.startSeq ? 1 : 0 };
  }
  private maybeStart(m: Meta): void {
    if (m.resetSeq || m.startSeq || !m.prepareSeq || !m.roster.every(p => m.ready.includes(p.slot))) return;
    Sentry.startSpan({ name: "game.round.start", op: "game.round.start" }, () => this.startRound(m));
  }
  private startRound(m: Meta): void {
    const random = new Uint32Array(1); crypto.getRandomValues(random);
    m.patientZero = m.roster[random[0] % m.roster.length].slot;
    m.startTime = Date.now() + START_COUNTDOWN_MS; m.phase = "running"; m.revision++; m.snapshotId++;
    m.startSeq = this.command(m, "START_ROUND", { snapshot_id: m.snapshotId, roster_hash: m.rosterHash, patient_zero_slot: m.patientZero,
      initial_role_rev: 1, start_time_ms: m.startTime, duration_ms: RULES.duration_ms });
    for (const player of m.roster) this.saveRole(m, player.slot, this.role(m, player.slot));
    this.saveSnapshot(m); this.syncPopulation(m);
  }
  async message(ws: WebSocket, raw: string | ArrayBuffer): Promise<void> {
    try {
      // A reset/replacement may have closed this socket while its next frame
      // was already queued. Do not turn that normal 1001 into an auth failure.
      if (ws.readyState !== WebSocket.OPEN) return;
      if (typeof raw !== "string") { ws.close(1003, "Text messages required"); return; }
      if (textEncoder.encode(raw).length > MAX_BYTES) { ws.close(1009, "Message too large"); return; }
      let parsed: unknown;
      try { parsed = JSON.parse(raw); } catch { throw new GatewayError("INVALID_PAYLOAD", "Invalid JSON", 400); }
      const b = object(parsed), a = ws.deserializeAttachment() as Attachment;
      // Host-local diagnostics use this authenticated socket only. They never
      // trigger receipts, replay, state broadcasts, or radio mesh messages.
      if (b.t === "diagnostics") {
        if (!a.welcomed || b.v !== 1 || typeof b.id !== "number" || !Number.isInteger(b.id) || b.id <= a.lastClientId || b.id > 0xffffffff || typeof b.ts !== "number" || !Number.isSafeInteger(b.ts) || b.ts < 0) return;
        a.lastClientId = b.id; ws.serializeAttachment(a);
        const meta = this.get(); setGame(a.gameId, meta.roundId, a.hostBoot);
        this.telemetry.diagnostics(b, textEncoder.encode(raw).length, a.hostBoot, a.resetRound ?? meta.roundId);
        return;
      }
      const context = this.get(); setGame(a.gameId, context.roundId, a.hostBoot);
      if (b.v !== 1 || typeof b.t !== "string") throw new GatewayError("INVALID_PAYLOAD", "Invalid envelope", 400);
      const id = integer(b.id, 1); integer(b.ts, 0, Number.MAX_SAFE_INTEGER);
      if (id <= a.lastClientId) throw new GatewayError("INVALID_PAYLOAD", "Client sequence must increase", 400);
      const due = this.get();
      if (due.startSeq && !due.end && !due.resetSeq && Date.now() >= due.startTime + RULES.duration_ms) {
        this.ctx.storage.transactionSync(() => { this.finishIfDue(due); this.put(due); });
        this.changed();
      }
      if (!a.welcomed) {
        if (b.t !== "hello" || b.proto !== 1 || b.game_id !== a.gameId || b.host_id !== this.env.ZT_HOST_MAC) { ws.close(1008, "Host handshake mismatch"); return; }
        const hostBoot = hex(b.host_boot, 16);
        const registrationId = b.registration_id === undefined ? undefined : hex(b.registration_id, 16);
        if (registrationId === ZERO) throw new GatewayError("INVALID_PAYLOAD", "Zero registration identity", 400);
        ascii(b.fw, 8, 8); const knownRound = round(b.round_id); integer(b.state_rev); integer(b.pending_events, 0, 65535);
        if (!Array.isArray(b.decided_through) || b.decided_through.length > 20) throw new GatewayError("INVALID_PAYLOAD", "Invalid frontier", 400);
        for (const item of b.decided_through) { const f = object(item); integer(f.slot, 0, 19); integer(f.seq, 0, 65535); }
        const last = integer(b.last_server_id), channel = integer(b.channel, 1, 11), m = this.get();
        const archived = knownRound && knownRound !== m.roundId ? this.archive(knownRound) :
          !knownRound ? this.registrationCleanup(hostBoot, registrationId) : undefined;
        // An unregistered transport host has no self-reset receipt. Once its
        // peer window finishes, let its retained cleanup round reconnect to the
        // active lobby instead of entering the same archive forever.
        const cleanup = archived?.resetSeq && (!archived.resetFinished || archived.roster.some(p => p.id === this.env.ZT_HOST_MAC)) ? archived : undefined;
        if (!cleanup && m.roundId && channel !== m.channel) throw new GatewayError("WRONG_ROUND", "Host channel differs from the frozen round");
        for (const previous of this.ctx.getWebSockets("gateway")) if (previous !== ws) {
          const attachment = previous.deserializeAttachment() as Attachment;
          if (cleanup && attachment.resetRound !== cleanup.roundId) continue;
          previous.serializeAttachment({ ...attachment, welcomed: false });
          previous.close(1001, "Host reconnected");
        }
        a.welcomed = true; a.hostBoot = hostBoot; a.lastClientId = id; a.lastSeenAt = Date.now(); a.resetRound = cleanup?.roundId ?? undefined; ws.serializeAttachment(a);
        if (!cleanup) { m.channel = channel; m.hostBoot = hostBoot; m.hostLastSeenAt = Date.now(); this.put(m); }
        const session = cleanup ?? m;
        this.transient(ws, "welcome", { server_time_ms: Date.now(), resume: last > m.serverSeq ? "reset" : "ok", phase: session.phase, round_id: session.roundId,
          state_rev: session.revision, resume_from: last + 1, snapshot_id: cleanup ? 0 : m.snapshotId, snapshot_pages: cleanup ? 0 : m.roundId ? Math.ceil(m.roster.length / 8) : 0,
          resetting: Boolean(cleanup || m.resetSeq), diagnostics: Boolean(this.env.SENTRY_DSN) });
        setGame(a.gameId, session.roundId, hostBoot);
        this.telemetry.log("connection.opened", { connection: "host", source: "durable_object" });
        this.changed(); return;
      }
      if (b.t === "hello") throw new GatewayError("INVALID_PAYLOAD", "Hello already received", 400);
      a.lastClientId = id; a.lastSeenAt = Date.now(); ws.serializeAttachment(a);
      if (!a.resetRound) { const live = this.get(); live.hostLastSeenAt = a.lastSeenAt; this.put(live); }
      if (b.t === "time_sync") {
        this.transient(ws, "time_sync_reply", { nonce: integer(b.nonce), server_time_ms: Date.now() }); return;
      }
      if (a.resetRound) {
        const archived = this.archive(a.resetRound);
        if (!archived) throw new GatewayError("WRONG_ROUND", "Archived reset is unavailable");
        this.archivedResetMessage(ws, b, archived); return;
      }
      const m = this.get();
      // A lobby host may miss the initial snapshot push while its bounded RX
      // buffer is occupied. An empty poll discovers the frozen round again.
      // It carries no receipts and does not mutate durable lobby state.
      if (b.t === "ack" && b.round_id === null) {
        for (const field of ["applied", "ready", "decision_applied", "round_closed", "presence"]) {
          if (boundedArray(b[field]).length) throw new GatewayError("INVALID_PAYLOAD", "A lobby poll cannot contain receipts", 400);
        }
        if (m.roundId) this.snapshot(ws, m.snapshotId, 0);
        else this.transient(ws, "commands", { state_rev: m.revision, commands: [] });
        return;
      }
      if (round(b.round_id) !== m.roundId || !m.roundId) throw new GatewayError("WRONG_ROUND", "Message does not name the active round");
      if (b.t === "need") {
        if (b.snapshot_page !== undefined && b.events === undefined) {
          const page = object(b.snapshot_page); this.snapshot(ws, integer(page.snapshot_id, 1), integer(page.page_index, 0, 2)); return;
        }
        if (b.events !== undefined && b.snapshot_page === undefined) {
          const ids = boundedArray(b.events);
          if (!ids.length || ids.some(id => typeof id !== "string" || !/^[0-9a-f]{16}\/[0-9a-f]{2}\/[0-9a-f]{4}$/.test(id) || !id.startsWith(`${m.roundId}/`))) throw new GatewayError("INVALID_PAYLOAD", "Invalid event identities", 400);
          const rows = this.eventRows(m.roundId).filter(row => ids.includes(row.id));
          if (rows.length) this.transient(ws, "decisions", { decisions: rows.map(r => ({ id: r.id, status: r.status, reason: r.reason, archived: false })) });
          else this.transient(ws, "need_events", { need_events: ids });
          return;
        }
        throw new GatewayError("INVALID_PAYLOAD", "Choose one need variant", 400);
      }
      if (b.t === "events") {
        Sentry.startSpan({ name: "game.infection.process", op: "game.infection.process" }, () => this.ingest(ws, m, boundedArray(b.events))); return;
      }
      if (b.t !== "ack") throw new GatewayError("INVALID_PAYLOAD", "Unsupported message type", 400);
      const applied = boundedArray(b.applied), ready = boundedArray(b.ready);
      const decisionApplied = boundedArray(b.decision_applied).map(value => {
        const f = object(value), slot = integer(f.slot, 0, 19), through = integer(f.through_seq, 0, 65535);
        if (!m.roster.some(p => p.slot === slot)) throw new GatewayError("INVALID_PAYLOAD", "Decision receipt slot is not rostered", 400);
        const rows = this.ctx.storage.sql.exec<{seq:number;status:string}>("SELECT seq,status FROM gateway_events WHERE round_id=? AND victim=? AND seq<=? ORDER BY seq", m.roundId, slot, through).toArray();
        if (rows.length !== through || rows.some((r, i) => r.seq !== i + 1 || r.status === "pending_dependency")) throw new GatewayError("INVALID_PAYLOAD", "Decision receipt exceeds contiguous final evidence", 400);
        return { slot, through };
      });
      const closed = boundedArray(b.round_closed).map(value => {
        const receipt = object(value), slot = integer(receipt.slot, 0, 19), produced = integer(receipt.produced_seq, 0, 65535);
        if (!m.startSeq || !m.roster.some(p => p.slot === slot)) throw new GatewayError("INVALID_PAYLOAD", "Invalid round-close receipt", 400);
        return { slot, produced };
      });
      boundedArray(b.presence);
      const validated = applied.map(value => {
        const receipt = object(value), seq = integer(receipt.seq, 1), slot = integer(receipt.slot, 0, 19), rev = integer(receipt.state_rev);
        if (!m.roster.some(p => p.slot === slot) || !["received", "applied", "prepared_ready", "rejected", "requires_snapshot"].includes(String(receipt.result))) throw new GatewayError("INVALID_PAYLOAD", "Invalid command receipt", 400);
        return { seq, slot, rev, result: receipt.result };
      });
      for (const value of ready) {
        const r = object(value); integer(r.slot, 0, 19); integer(r.snapshot_id);
        if (r.round_id !== m.roundId) throw new GatewayError("WRONG_ROUND", "Ready receipt round mismatch");
      }
      const previousStart = m.startSeq;
      this.ctx.storage.transactionSync(() => {
        for (const receipt of validated) {
          const row = this.ctx.storage.sql.exec<CommandRow>("SELECT seq,body,acknowledged FROM gateway_commands WHERE seq=?", receipt.seq).toArray()[0];
          if (!row) continue;
          const command = (JSON.parse(row.body) as {commands:Array<{round_id:string;target:number;type:string}>}).commands[0];
          if (command.round_id !== m.roundId || (command.target !== 255 && command.target !== receipt.slot)) continue;
          const prepared = receipt.seq === m.prepareSeq && receipt.result === "prepared_ready" && receipt.rev === m.prepareSnapshot;
          const complete = prepared || (receipt.seq !== m.prepareSeq && receipt.result === "applied");
          if (prepared && !m.ready.includes(receipt.slot)) m.ready.push(receipt.slot);
          if (complete) {
            const slots = JSON.parse(row.acknowledged) as number[];
            if (!slots.includes(receipt.slot)) slots.push(receipt.slot);
            this.ctx.storage.sql.exec("UPDATE gateway_commands SET acknowledged=? WHERE seq=?", JSON.stringify(slots), receipt.seq);
          }
        }
        for (const f of decisionApplied) this.ctx.storage.sql.exec("INSERT INTO gateway_decision_acks VALUES(?,?,?) ON CONFLICT(round_id,slot) DO UPDATE SET through_seq=MAX(through_seq,excluded.through_seq)", m.roundId, f.slot, f.through);
        for (const receipt of closed) {
          m.closed ??= {};
          m.closed[receipt.slot] = Math.max(m.closed[receipt.slot] ?? 0, receipt.produced);
        }
        this.maybeStart(m); this.finishIfDue(m); this.put(m);
      });
      const host = m.roster.find(player => player.id === this.env.ZT_HOST_MAC);
      if (validated.some(receipt => receipt.seq === m.prepareSeq && receipt.slot === host?.slot &&
        (receipt.result === "prepared_ready" || receipt.result === "applied"))) {
        a.prepareSeeded = m.prepareSeq; ws.serializeAttachment(a);
      }
      if (!previousStart && m.startSeq) {
        this.telemetry.log("game.round_started", { round_id: m.roundId, players: m.roster.length, state_rev: m.revision });
        await this.ctx.storage.setAlarm(m.startTime + RULES.duration_ms);
      }
      if (validated.some(r => r.result === "requires_snapshot")) this.snapshot(ws, m.startSeq ? m.snapshotId : m.prepareSnapshot, 0);
      else if (!previousStart && m.startSeq) this.announceCurrent();
      else this.replay(ws, true);
      this.changed();
    } catch (error) {
      if (error instanceof GatewayError) { this.telemetry.log("gateway.rejected", { code: error.code }, "warn"); this.transient(ws, "error", { code: ["WRONG_ROUND", "INVALID_PAYLOAD", "CURSOR_EXPIRED"].includes(error.code) ? error.code : "INTERNAL", detail: error.message, fatal: false }); return; }
      reportFailure(error);
      this.telemetry.log("backend.failure", { source: "durable_object" }, "error");
      this.transient(ws, "error", { code: "INTERNAL", detail: "Gateway operation failed; pending evidence must be retained.", fatal: false });
    }
  }
}
