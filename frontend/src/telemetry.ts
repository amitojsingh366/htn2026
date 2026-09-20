import * as Sentry from '@sentry/react';
import { createBudget, GAME_EVENTS, safeAttributes, safeEvent, safeSpan, sampleRate, type Attributes, type GameEvent } from './telemetryPolicy';

export const GAME_ID = '005a544d454d4f01';
export const API_BASE = (import.meta.env.VITE_API_BASE ?? `https://htn2026-backend.amitoj.workers.dev/api/v1/games/${GAME_ID}`).replace(/\/$/, '');
const enabled = Boolean(import.meta.env.VITE_SENTRY_DSN);
const logBudget = createBudget(30, 5_000);
const errorBudget = createBudget(5, 60_000);
const logSendBudget = createBudget(30, 0);
const allErrorBudget = createBudget(5, 0);

export interface StateTelemetry {
  round_id?: string;
  telemetry?: { sentry_trace?: string; action_id?: string; round_id?: string };
}

export function initializeTelemetry() {
  if (!enabled) return;
  // This dashboard has no routed/query-driven UI. Do not record a URL that could
  // contain a pasted credential; replay's DOM recorder owns its initial URL.
  const replayAllowed = ['/', '/index.html'].includes(location.pathname) && !location.search && !location.hash;
  const apiPattern = new RegExp(`^${new URL(API_BASE, location.origin).href.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?:/|$)`);
  Sentry.init({
    dsn: import.meta.env.VITE_SENTRY_DSN,
    environment: import.meta.env.VITE_SENTRY_ENVIRONMENT ?? import.meta.env.MODE,
    release: import.meta.env.VITE_SENTRY_RELEASE || undefined,
    sendDefaultPii: false,
    dataCollection: { userInfo: false, cookies: false, httpHeaders: false, httpBodies: [], urlQueryParams: false, stackFrameVariables: false, frameContextLines: 0, databaseQueryData: false, graphQL: { document: false, variables: false }, genAI: { inputs: false, outputs: false } },
    enhanceFetchErrorMessages: false,
    enableLogs: true,
    maxBreadcrumbs: 0,
    integrations: [
      Sentry.breadcrumbsIntegration({ console: false, dom: false, fetch: false, xhr: false, history: false }),
      Sentry.browserTracingIntegration({
        enableInp: false,
        instrumentNavigation: false,
        beforeStartSpan: options => ({ ...options, name: 'dashboard.load', attributes: {} }),
        shouldCreateSpanForRequest: url => apiPattern.test(url),
      }),
      Sentry.replayIntegration({
        maskAllText: true, maskAllInputs: true, blockAllMedia: true,
        block: ['.sentry-block'],
        networkCaptureBodies: false, networkDetailAllowUrls: [],
        networkRequestHeaders: [], networkResponseHeaders: [],
        // Exclude arbitrary console/network/URL data; DOM replay remains masked.
        beforeAddRecordingEvent: () => null,
        maxReplayDuration: 15 * 60_000,
        mutationLimit: 5_000,
      }),
    ],
    tracePropagationTargets: [apiPattern],
    tracesSampleRate: sampleRate(import.meta.env.VITE_SENTRY_TRACES_SAMPLE_RATE, 0.1),
    replaysSessionSampleRate: replayAllowed ? sampleRate(import.meta.env.VITE_SENTRY_REPLAY_SESSION_SAMPLE_RATE, 0.05) : 0,
    replaysOnErrorSampleRate: replayAllowed ? sampleRate(import.meta.env.VITE_SENTRY_REPLAY_ON_ERROR_SAMPLE_RATE, 1) : 0,
    beforeSend: event => allErrorBudget('feed.failed') ? safeEvent(event) : null,
    beforeSendTransaction: safeEvent,
    beforeSendSpan: safeSpan,
    beforeSendLog: log => {
      if (!GAME_EVENTS.includes(log.message as GameEvent) || !logSendBudget(log.message as GameEvent)) return null;
      return { ...log, attributes: safeAttributes(log.attributes) };
    },
  });
  Sentry.addEventProcessor(safeEvent);
  Sentry.setTag('component', 'browser');
}

export function gameLog(event: GameEvent, attributes: Record<string, unknown> = {}, level: 'info' | 'warn' = 'info') {
  if (enabled && logBudget(event)) Sentry.logger[level](event, safeAttributes({ component: 'browser', ...attributes }));
}

export function reportFailure(event: GameEvent, error: unknown, attributes: Record<string, unknown> = {}) {
  gameLog(event, attributes, 'warn');
  if (!enabled || !errorBudget(event)) return;
  Sentry.withScope(scope => {
    scope.setTags(safeAttributes({ component: 'browser', ...attributes }));
    scope.setFingerprint(['browser', event]);
    // beforeSend removes original values but keeps a useful stack when available.
    Sentry.captureException(error instanceof Error ? error : new Error(event));
  });
}

/** The action UUID and W3C-compatible Sentry trace travel only over HTTP. */
export async function gameRequest<T>(operation: 'round.start' | 'round.reset' | 'state.sync', url: string, init?: RequestInit): Promise<{ response: Response; data: T }> {
  const actionId = crypto.randomUUID();
  return Sentry.startSpan({ name: operation === 'state.sync' ? 'state.sync.http' : operation, op: operation === 'state.sync' ? 'state.sync' : 'ui.action', attributes: { action_id: actionId, component: 'browser' } }, async span => {
    const attributes: Attributes = { action_id: actionId, operation, transport: 'http' };
    if (operation !== 'state.sync') gameLog(`${operation}.requested`, attributes);
    try {
      const headers = new Headers(init?.headers);
      headers.set('x-action-id', actionId);
      const response = await fetch(url, { ...init, headers });
      span.setAttribute('http.status_code', response.status);
      attributes['http.status_code'] = response.status;
      if (!response.ok) {
        span.setStatus({ code: 2, message: 'http_error' });
        reportFailure(`${operation}.failed`, new Error('Game API request failed'), { ...attributes, outcome: 'http_error' });
      }
      const data = await response.json() as T;
      if (response.ok && operation !== 'state.sync') gameLog(`${operation}.completed`, { ...attributes, outcome: 'ok' });
      return { response, data };
    } catch (error) {
      span.setStatus({ code: 2, message: 'network_error' });
      reportFailure(`${operation}.failed`, error, { ...attributes, outcome: 'network_error' });
      throw error;
    }
  });
}

export function traceStateUpdate<T>(state: StateTelemetry, transport: 'http' | 'websocket', update: () => T): T {
  const trace = state.telemetry?.sentry_trace;
  const attributes = safeAttributes({ transport, component: 'browser', action_id: state.telemetry?.action_id, round_id: state.round_id ?? state.telemetry?.round_id });
  const apply = () => Sentry.startSpan({ name: `state.sync.${transport}`, op: 'state.apply', attributes }, update);
  // Do not accept arbitrary baggage from state messages, only a validated trace.
  return transport === 'websocket' && trace && /^[a-f0-9]{32}-[a-f0-9]{16}(?:-[01])?$/i.test(trace)
    ? Sentry.continueTrace({ sentryTrace: trace, baggage: undefined }, apply)
    : apply();
}
