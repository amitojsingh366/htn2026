import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createBudget, safeAttributes, safeEvent, safeSpan, sampleRate } from '../src/telemetryPolicy.ts';

const action = 'fe220c3e-217e-46e6-b719-d0ef750d84d2';
const trace = 'ab'.repeat(16);
const secret = 'private-player-name auth-token-aa:bb:cc:dd:ee:ff';

test('telemetry admits only finite counters, known phases and opaque correlation identifiers', () => {
  assert.deepEqual(safeAttributes({ action_id: action, round_id: '0123456789abcdef', gateway_phase: 'expired_pending_sync', registered_players: 2, host_connected: true, token: secret, player_name: secret, player_id: secret, payload: { name: secret }, retry_count: Infinity, num_infected: -1, operation: secret }), { action_id: action, round_id: '0123456789abcdef', gateway_phase: 'expired_pending_sync', registered_players: 2, host_connected: true });
  assert.deepEqual(safeAttributes({ action_id: secret, round_id: secret, trace_id: secret }), {});
});

test('exception filtering strips payloads and raw messages but retains stack position and trace/replay links', () => {
  const event = safeEvent({
    message: secret, user: { name: secret }, request: { url: `https://dashboard/?token=${secret}` }, extra: { body: secret }, logentry: { message: secret }, breadcrumbs: [{ message: secret }], tags: { player_name: secret, action_id: action },
    contexts: { custom: { secret }, trace: { trace_id: trace, span_id: 'ab'.repeat(8), data: { token: secret } }, replay: { replay_id: trace } },
    exception: { values: [{ value: secret, type: secret, stacktrace: { frames: [{ filename: `https://dashboard/assets/index-abc.js?token=${secret}`, abs_path: secret, lineno: 12, colno: 3, vars: { token: secret }, context_line: secret }] } }] },
  });
  assert.ok(!JSON.stringify(event).includes(secret));
  assert.equal(event.exception.values[0].stacktrace.frames[0].filename, 'https://dashboard/assets/index-abc.js');
  assert.equal(event.exception.values[0].stacktrace.frames[0].lineno, 12);
  assert.equal(event.contexts.trace.trace_id, trace);
  assert.equal(event.contexts.replay.replay_id, trace);
  assert.equal(event.tags.action_id, action);
});

test('automatic spans and replay metadata cannot carry arbitrary URLs or request bodies', () => {
  const span = safeSpan({ trace_id: trace, span_id: 'ab'.repeat(8), start_timestamp: 1, op: 'http.client', description: secret, data: { url: secret, 'http.request.body': secret, 'http.response.status_code': 200 } });
  assert.equal(span.description, 'game API request');
  assert.deepEqual(span.data, { 'http.response.status_code': 200 });
  const replay = safeEvent({ type: 'replay_event', urls: [`https://dashboard/?${secret}`] });
  assert.deepEqual(replay.urls, []);
});

test('rate budget drops bursts and repeated connection events, then recovers without a queue', () => {
  let now = 1_000;
  const permit = createBudget(3, 5_000, () => now);
  assert.equal(permit('feed.failed'), true);
  assert.equal(permit('feed.failed'), false);
  assert.equal(permit('feed.connected'), true);
  assert.equal(permit('feed.disconnected'), true);
  now += 6_000;
  assert.equal(permit('feed.failed'), false);
  now += 60_000;
  assert.equal(permit('feed.failed'), true);
});

test('sampling configuration never permits invalid or unbounded sample rates', () => {
  assert.equal(sampleRate('', 0.1), 0.1);
  assert.equal(sampleRate('bad', 0.1), 0.1);
  assert.equal(sampleRate('2', 0.1), 0.1);
  assert.equal(sampleRate('-1', 0.1), 0.1);
  assert.equal(sampleRate('0', 0.1), 0);
  assert.equal(sampleRate('1', 0.1), 1);
});
