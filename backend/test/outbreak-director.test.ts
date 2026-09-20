import { env } from 'cloudflare:workers';
import { runInDurableObject, evictDurableObject } from 'cloudflare:test';
import { getAgentByName } from 'agents';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { DirectorObservation } from '../src/director-types';
import { cleanAnnouncement, groundedRecap, limits, meaningfulChange } from '../src/director-policy';

const ROUND = '1234567890abcdef';
let requests: Array<Record<string, unknown>>;
let replies: Array<{ status?: number; output?: unknown[]; before?: () => Promise<void> }>;
beforeEach(() => {
  requests = []; replies = [];
  vi.spyOn(globalThis, 'fetch').mockImplementation(async (input, init) => {
    const url = input instanceof Request ? input.url : String(input);
    if (url !== 'https://api.openai.com/v1/responses') throw new Error(`Network is disabled in tests: ${url}`);
    requests.push(JSON.parse(String(init?.body)));
    const reply = replies.shift();
    if (!reply) throw new Error('Unexpected OpenAI call; real network is disabled.');
    await reply.before?.();
    return Response.json(reply.status && reply.status >= 400 ? { error: { message: 'Mock provider failure', type: 'server_error' } } : {
      id: `resp_MOCK_${requests.length}`, object: 'response', status: 'completed', model: 'gpt-4.1-mini',
      output: reply.output ?? [{ type: 'message', id: 'msg_mock', role: 'assistant', status: 'completed', content: [{ type: 'output_text', text: 'Observation complete.', annotations: [] }] }],
      usage: { input_tokens: 100, output_tokens: 20, total_tokens: 120 },
    }, { status: reply.status ?? 200, headers: { 'x-request-id': `req_MOCK_${requests.length}` } });
  });
});
afterEach(() => { vi.restoreAllMocks(); });

async function setup(name: string, final = false) {
  const room = env.GAME_ROOM.getByName(`agent-${name}`);
  await runInDurableObject(room, (_instance, state) => {
    const meta = JSON.parse(state.storage.sql.exec<{body:string}>('SELECT body FROM gateway_meta WHERE id=1').one().body);
    const now = Date.now();
    Object.assign(meta, { gameId: env.DIRECTOR_GAME_ID, roundId: ROUND, revision: 4, phase: final ? 'final' : 'running', startSeq: 1, startTime: now - 60_000,
      patientZero: 0, roster: [{ slot: 0, id: '001122334455', name: 'Host' }, { slot: 1, id: '001122334466', name: 'Player' }],
      ...(final ? { end: { winner: 'H', final: true, effectiveElapsed: 60_000, missingSlots: 0, endSeq: 2 } } : {}) });
    state.storage.sql.exec('UPDATE gateway_meta SET body=? WHERE id=1', JSON.stringify(meta));
  });
  const agent = await getAgentByName(env.OUTBREAK_DIRECTOR, room.id.toString());
  const observation = await room.getDirectorObservation();
  return { room, agent, observation };
}
async function wake(agent: Awaited<ReturnType<typeof setup>>['agent']) {
  const pending = (await agent.getStatus()).pending;
  expect(pending).not.toBeNull();
  await agent.wake({ roundId: pending!.roundId, dueAt: pending!.dueAt });
}
function call(name: string, args: Record<string, unknown>) {
  return [{ type: 'function_call', id: 'fc_mock', call_id: `${name}_mock`, name, arguments: JSON.stringify(args), status: 'completed' }];
}

describe('OpenAI director on the Cloudflare Agent runtime (mock API)', () => {
  it('calls Responses server-side, executes a recap tool, and preserves memory across eviction', async () => {
    const { agent, observation } = await setup('recap', true);
    replies.push({ output: call('publish_recap', { headline: 'outbreak_contained', event_ids: [] }) }, {});
    await agent.observe(observation);
    await wake(agent);
    const status = await agent.getStatus();
    expect(status.status).toBe('idle');
    expect(status.recaps[0]).toMatchObject({ roundId: ROUND, final: true, title: 'Outbreak contained' });
    expect(status.recaps[0].text).toContain('1 of 2 players infected');
    expect(status.actions[0].result.status).toBe('published');
    expect(status.runs[0]).toMatchObject({ status: 'completed', responseIds: ['resp_MOCK_1', 'resp_MOCK_2'], requestIds: ['req_MOCK_1', 'req_MOCK_2'], inputTokens: 200, outputTokens: 40 });
    expect(requests[0]).toMatchObject({ model: 'gpt-4.1-mini', store: false, max_output_tokens: 600, parallel_tool_calls: false });
    expect(requests[1].input).toEqual(expect.arrayContaining([expect.objectContaining({ type: 'function_call_output', call_id: 'publish_recap_mock' })]));
    expect(JSON.stringify(status)).not.toContain('test-key');
    await evictDurableObject(agent);
    const restored = await agent.getStatus();
    expect(restored.recaps).toEqual(status.recaps);
    expect(restored.dailyUsage).toEqual(status.dailyUsage);
    expect(restored.actions).toEqual(status.actions);
  });

  it('ignores duplicate heartbeats and old observations without spending another request', async () => {
    const { agent, observation } = await setup('duplicates');
    replies.push({});
    await agent.observe(observation); await wake(agent);
    await agent.observe({ ...observation, observedAt: observation.observedAt + 1000 });
    await agent.observe({ ...observation, revision: 3, infected: 2 });
    expect((await agent.getStatus()).pending).toBeNull();
    expect((await agent.getStatus()).latest?.revision).toBe(4);
    expect(requests).toHaveLength(1);
  });

  it('persists failed request reservations without provider retries, then leaves gameplay readable', async () => {
    const { agent, room, observation } = await setup('failure');
    replies.push({ status: 503 });
    await agent.observe(observation); await wake(agent);
    const status = await agent.getStatus();
    expect(status.status).toBe('degraded');
    expect(status.dailyUsage.requests).toBe(1);
    expect(status.runs[0].status).toBe('failed');
    expect(requests).toHaveLength(1);
    expect((await room.getState()).num_players).toBe(0); // Fixture changes only gateway state; the legacy store is untouched.
  });

  it('rejects late model actions when a new round arrives during the API request', async () => {
    const { agent, observation } = await setup('stale', true);
    await agent.observe(observation);
    await runInDurableObject(agent, async instance => {
      replies.push({ output: call('publish_recap', { headline: 'outbreak_contained', event_ids: [] }), before: async () => {
        await instance.observe({ ...observation, roundId: null, phase: 'lobby', revision: 5, observedAt: Date.now() });
      } });
      const pending = instance.state.pending!;
      await instance.wake({ roundId: pending.roundId, dueAt: pending.dueAt });
    });
    const status = await agent.getStatus();
    expect(status.recaps).toHaveLength(0);
    expect(status.actions).toHaveLength(0);
    expect(status.runs[0]).toMatchObject({status: 'stale', summary: expect.any(String)});
    expect(status.latest?.roundId).toBeNull();
  });

  it('bounds per-round usage across eviction and never sends an over-budget request', async () => {
    const { agent, observation } = await setup('budget');
    await agent.observe(observation);
    await runInDurableObject(agent, instance => instance.setState({ ...instance.state, roundRequests: 18 }));
    await evictDurableObject(agent);
    await wake(agent);
    expect((await agent.getStatus()).status).toBe('budget_exhausted');
    expect(requests).toHaveLength(0);
  });

  it('creates a durable follow-up and cancels it on reset without another model call', async () => {
    const { agent, observation } = await setup('schedule');
    replies.push({ output: call('schedule_follow_up', { delay_seconds: 60, reason: 'Check the outbreak again.' }) }, {});
    await agent.observe(observation); await wake(agent);
    const status = await agent.getStatus();
    expect(status.followUps).toHaveLength(1);
    const scheduleId = status.followUps[0].id;
    await evictDurableObject(agent);
    expect((await agent.getStatus()).followUps[0].id).toBe(scheduleId);
    await agent.observe({ ...observation, roundId: null, revision: 5, observedAt: Date.now() });
    expect((await agent.getStatus()).followUps).toHaveLength(0);
    expect(await runInDurableObject(agent, async instance => (await instance.listSchedules()).some(s => s.id === scheduleId))).toBe(false);
    expect(requests).toHaveLength(2);
  });

  it('stops repeated tool requests at four model calls and marks the final request tool-free', async () => {
    const { agent, observation } = await setup('loop');
    replies.push({ output: call('read_game_state', {}) }, { output: call('read_game_state', {}) }, { output: call('read_game_state', {}) }, {});
    await agent.observe(observation); await wake(agent);
    expect(requests).toHaveLength(4);
    expect(requests[3].tool_choice).toBe('none');
    expect((await agent.getStatus()).dailyUsage.requests).toBe(4);
  });
});

describe('Director policy', () => {
  it('only recaps accepted, known evidence with authoritative provisional/final facts', () => {
    const observation = { roundId: ROUND, endedAt: 2000, startedAt: 1000, winner: 'H', players: 3, infected: 1, humans: 2,
      acceptedEvents: 0, pendingEvents: 1, rejectedEvents: 1, final: false,
      history: [{ id: 'rejected', actor: 0, victim: 1, elapsedMs: 500, status: 'rejected' }] } as DirectorObservation;
    expect(() => groundedRecap(observation, { event_ids: ['invented'] }, 3)).toThrow('unknown');
    expect(() => groundedRecap(observation, { event_ids: ['rejected'] }, 3)).toThrow('unaccepted');
    const recap = groundedRecap(observation, { headline: 'zombies_take_round', event_ids: [] }, 3);
    expect(recap.title).toBe('Round summary');
    expect(recap.text).toContain('Humans win (provisional');
  });
  it('sanitizes cosmetic messages and clamps configurable budgets', () => {
    expect(cleanAnnouncement('  **Watch** <out>! 🧟\n')).toBe('Watch out!');
    expect(cleanAnnouncement('a'.repeat(200))).toHaveLength(96);
    expect(limits({ ...env, DIRECTOR_DAILY_REQUEST_LIMIT: '999999', DIRECTOR_ROUND_REQUEST_LIMIT: '-2', DIRECTOR_MAX_OUTPUT_TOKENS: 'NaN' }))
      .toMatchObject({ dailyRequests: 200, roundRequests: 1, outputTokens: 600 });
  });
  it('treats badge liveness changes alone as non-events', () => {
    const previous = { roundId: ROUND, revision: 1, startedAt: 1, infected: 1, acceptedEvents: 0, winner: null, endedAt: null, final: false } as DirectorObservation;
    expect(meaningfulChange(previous, { ...previous, hostConnected: false, observedAt: 200 })).toBeNull();
    expect(meaningfulChange(previous, { ...previous, infected: 2 })).toBe('infection_change');
  });
});
