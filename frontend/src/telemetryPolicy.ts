import type { Event, spanToJSON } from '@sentry/react';
type SpanJSON = ReturnType<typeof spanToJSON>;

export const GAME_EVENTS = [
  'round.start.requested', 'round.start.completed', 'round.start.failed',
  'round.reset.requested', 'round.reset.completed', 'round.reset.failed',
  'state.changed', 'state.sync.failed', 'feed.connected', 'feed.disconnected',
  'feed.reconnecting', 'feed.failed', 'feed.invalid_message',
] as const;
export type GameEvent = typeof GAME_EVENTS[number];
export type Attributes = Record<string, string | number | boolean>;
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/i;
const numericKeys = new Set([
  'registered_players', 'num_players', 'num_infected', 'events_received', 'events_pending',
  'events_rejected', 'ready_players', 'start_applied_players', 'http.status_code',
  'http.response.status_code', 'retry_count', 'close_code', 'suppressed_count',
  'sentry.sample_rate', 'sentry.replay.segment_id',
]);
const enumValues: Record<string, readonly string[]> = {
  component: ['browser'], transport: ['http', 'websocket'],
  operation: ['round.start', 'round.reset', 'state.sync'],
  outcome: ['ok', 'http_error', 'network_error', 'invalid_message'],
  gateway_phase: ['lobby', 'prepared', 'running', 'expired_pending_sync', 'final', 'resetting'],
  'sentry.op': ['ui.action', 'state.sync', 'state.apply', 'http.client', 'pageload', 'navigation'],
  'sentry.origin': ['manual', 'auto.http.browser', 'auto.pageload.browser', 'auto.navigation.browser'],
  'sentry.source': ['custom', 'route'],
};

/** Deliberate allowlist: never accept payloads, names, badge IDs, headers or URLs. */
export function safeAttributes(input: Record<string, unknown> = {}): Attributes {
  const result: Attributes = {};
  for (const [key, value] of Object.entries(input)) {
    if (numericKeys.has(key) && typeof value === 'number' && Number.isFinite(value) && value >= 0) result[key] = value;
    else if (['game_over', 'host_connected'].includes(key) && typeof value === 'boolean') result[key] = value;
    else if (key === 'action_id' && typeof value === 'string' && uuid.test(value)) result[key] = value;
    else if (key === 'round_id' && typeof value === 'string' && /^[a-f0-9]{16}$/i.test(value)) result[key] = value;
    else if (['trace_id', 'sentry.replay_id'].includes(key) && typeof value === 'string' && /^[a-f0-9]{32}$/i.test(value)) result[key] = value;
    else if (typeof value === 'string' && enumValues[key]?.includes(value)) result[key] = value;
  }
  return result;
}

export function sampleRate(value: string | undefined, fallback: number): number {
  const number = value === undefined || value.trim() === '' ? fallback : Number(value);
  return Number.isFinite(number) && number >= 0 && number <= 1 ? number : fallback;
}

export function safeSpan(span: SpanJSON): SpanJSON {
  const names = ['dashboard.load', 'round.start', 'round.reset', 'state.sync.http', 'state.sync.websocket'];
  return {
    ...span,
    description: names.includes(span.description ?? '') ? span.description : span.op === 'http.client' ? 'game API request' : 'browser.activity',
    data: safeAttributes(span.data),
  };
}

/** Preserve actionable stack locations while removing dynamic error values and context. */
export function safeEvent<T extends Event>(event: T): T {
  const trace = event.contexts?.trace;
  const replayId = event.contexts?.replay?.replay_id;
  event.user = undefined;
  event.request = undefined;
  event.extra = undefined;
  event.logentry = undefined;
  event.message = event.message ? 'Browser error (details redacted)' : undefined;
  event.breadcrumbs = undefined;
  event.threads = undefined;
  event.tags = safeAttributes(event.tags);
  event.contexts = trace ? { trace: { trace_id: trace.trace_id, span_id: trace.span_id, parent_span_id: trace.parent_span_id, op: trace.op, status: trace.status, data: safeAttributes(trace.data) } } : {};
  if (typeof replayId === 'string' && /^[a-f0-9]{32}$/i.test(replayId)) event.contexts.replay = { replay_id: replayId };
  if (event.transaction) event.transaction = ['round.start', 'round.reset', 'state.sync.http', 'state.sync.websocket'].includes(event.transaction) ? event.transaction : 'dashboard.load';
  event.exception?.values?.forEach(exception => {
    exception.value = 'Browser error (details redacted)';
    exception.type = ['Error', 'TypeError', 'RangeError', 'SyntaxError', 'ReferenceError', 'URIError', 'EvalError'].includes(exception.type ?? '') ? exception.type : 'Error';
    if (exception.mechanism) exception.mechanism.data = undefined;
    exception.stacktrace?.frames?.forEach(frame => {
      frame.vars = undefined;
      frame.pre_context = undefined;
      frame.post_context = undefined;
      frame.context_line = undefined;
      // Only bundled static asset locations are useful for source-map lookup.
      if (frame.filename) {
        try {
          const url = new URL(frame.filename);
          frame.filename = /^\/assets\/[\w.-]+\.js$/.test(url.pathname) ? `${url.origin}${url.pathname}` : undefined;
        } catch { frame.filename = undefined; }
      }
      frame.abs_path = undefined;
    });
  });
  if (event.spans) event.spans = event.spans.map(safeSpan);
  if (event.type === 'replay_event') {
    // Replay metadata uses the normal event processor, not beforeSend.
    const replay = event as T & { urls?: string[] };
    replay.urls = [];
  }
  return event;
}

/** A fixed-memory minute budget plus per-event cooldown; diagnostics never queue. */
export function createBudget(limit: number, cooldownMs: number, now: () => number = Date.now) {
  let windowStart = now();
  let count = 0;
  const lastSent = new Map<string, number>();
  return (event: GameEvent) => {
    const time = now();
    if (time - windowStart >= 60_000) { windowStart = time; count = 0; }
    const last = lastSent.get(event);
    if (count >= limit || (last !== undefined && time - last < cooldownMs)) return false;
    count++;
    lastSent.set(event, time);
    return true;
  };
}
