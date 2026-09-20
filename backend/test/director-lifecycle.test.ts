import { env, runInDurableObject } from 'cloudflare:test';
import { getAgentByName } from 'agents';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { DirectorObservation } from '../src/director-types';
import type { OutbreakDirector } from '../src/outbreak-director';

const ROUND = 'fedcba0987654321';
let requests: Array<Record<string, unknown>>;
let outputs: unknown[][];

beforeEach(() => {
  requests = [];
  outputs = [];
  vi.spyOn(globalThis, 'fetch').mockImplementation(async (input, init) => {
    const url = input instanceof Request ? input.url : String(input);
    if (url !== 'https://api.openai.com/v1/responses') throw new Error(`Network disabled in lifecycle tests: ${url}`);
    requests.push(JSON.parse(String(init?.body)));
    const output = outputs.shift();
    if (!output) throw new Error('Unexpected model call; live OpenAI calls are disabled.');
    return Response.json({
      id: `resp_MOCK_lifecycle_${requests.length}`, object: 'response', status: 'completed', model: 'gpt-4.1-mini', output,
      usage: { input_tokens: 30, output_tokens: 10, total_tokens: 40 },
    }, { headers: { 'x-request-id': `req_MOCK_lifecycle_${requests.length}` } });
  });
});

afterEach(() => vi.restoreAllMocks());

async function setup(name: string) {
  const room = env.GAME_ROOM.getByName(`lifecycle-${name}`);
  await runInDurableObject(room, (_instance, state) => {
    const meta = JSON.parse(state.storage.sql.exec<{ body: string }>('SELECT body FROM gateway_meta WHERE id=1').one().body);
    Object.assign(meta, {
      gameId: env.DIRECTOR_GAME_ID, roundId: ROUND, revision: 4, phase: 'running', startSeq: 1,
      startTime: Date.now() - 30_000, patientZero: 0,
      roster: [{ slot: 0, id: '111122223333', name: 'Host' }, { slot: 1, id: '444455556666', name: 'Player' }],
    });
    state.storage.sql.exec('UPDATE gateway_meta SET body=? WHERE id=1', JSON.stringify(meta));
  });
  const agent = await getAgentByName(env.OUTBREAK_DIRECTOR, room.id.toString());
  return { agent, observation: await room.getDirectorObservation() };
}

function pendingWake(instance: OutbreakDirector) {
  const pending = instance.state.pending;
  expect(pending).not.toBeNull();
  return { roundId: pending!.roundId, dueAt: pending!.dueAt };
}

async function cancelSchedules(instance: OutbreakDirector) {
  for (const schedule of await instance.listSchedules()) await instance.cancelSchedule(schedule.id);
}

const completed = [{
  type: 'message', id: 'msg_MOCK_lifecycle', role: 'assistant', status: 'completed',
  content: [{ type: 'output_text', text: 'No further action needed.', annotations: [] }],
}];

describe('Director lifecycle races (local mock API)', () => {
  it.each(['revision', 'round'] as const)('does not replace a newer %s when an old room read finishes late', async change => {
    const { agent, observation } = await setup(`late-${change}`);
    await agent.observe(observation);
    await runInDurableObject(agent, async (instance: OutbreakDirector) => {
      const payload = pendingWake(instance);
      const newer: DirectorObservation = {
        ...observation, revision: observation.revision + 1, observedAt: observation.observedAt + 1,
        ...(change === 'round' ? { roundId: null, phase: 'lobby' } : { infected: 2, humans: 0 }),
      };
      // Delay only the room boundary: use real observe(), state, scheduler and wake().
      const roomBoundary = instance as unknown as { room: () => { getDirectorObservation(): Promise<DirectorObservation> } };
      const read = vi.spyOn(roomBoundary, 'room').mockReturnValue({
        getDirectorObservation: async () => {
          await instance.observe(newer);
          return observation;
        },
      });
      try {
        await instance.wake(payload);
        expect(instance.state.latest).toEqual(newer);
        expect(instance.state.runs.at(-1)?.status).toBe('stale');
        expect(instance.state.dailyUsage.requests).toBe(0);
        expect(instance.state.actions).toHaveLength(0);
        expect(requests).toHaveLength(0);
      } finally {
        read.mockRestore();
        await cancelSchedules(instance);
      }
    });
  });

  it('cancels a durable follow-up created while the round is being reset', async () => {
    const { agent, observation } = await setup('reset-during-schedule');
    outputs.push([{
      type: 'function_call', id: 'fc_MOCK_schedule', call_id: 'call_MOCK_schedule', status: 'completed',
      name: 'schedule_follow_up', arguments: JSON.stringify({ delay_seconds: 60, reason: 'Recheck survivors.' }),
    }]);
    await agent.observe(observation);
    await runInDurableObject(agent, async (instance: OutbreakDirector) => {
      const payload = pendingWake(instance);
      const schedule = instance.schedule.bind(instance);
      let createdScheduleId: string | undefined;
      const scheduleRace = vi.spyOn(instance, 'schedule').mockImplementation(async (...args) => {
        const created = await schedule(...args);
        if (args[1] === 'followUp') {
          createdScheduleId = created.id;
          await instance.observe({ ...observation, roundId: null, phase: 'lobby', revision: 5, observedAt: Date.now() });
        }
        return created;
      });
      try {
        await instance.wake(payload);
        expect(createdScheduleId).toBeDefined();
        expect(instance.state.latest?.roundId).toBeNull();
        expect(instance.state.followUps).toHaveLength(0);
        expect((await instance.listSchedules()).some(schedule => schedule.id === createdScheduleId)).toBe(false);
        expect(instance.state.actions).toHaveLength(1);
        expect(instance.state.actions[0].result.status).toBe('rejected');
        expect(instance.state.runs.at(-1)?.status).toBe('stale');
        expect(requests).toHaveLength(1);
      } finally {
        scheduleRace.mockRestore();
        await cancelSchedules(instance);
      }
    });
  });

  it('ignores duplicate wake deliveries once the first turn consumes pending work', async () => {
    const { agent, observation } = await setup('duplicate-wake');
    outputs.push(completed);
    await agent.observe(observation);
    await runInDurableObject(agent, async (instance: OutbreakDirector) => {
      try {
        const payload = pendingWake(instance);
        await Promise.all([instance.wake(payload), instance.wake(payload)]);
        await instance.wake(payload);
        expect(requests).toHaveLength(1);
        expect(instance.state.dailyUsage.requests).toBe(1);
        expect(instance.state.runs).toHaveLength(1);
        expect(instance.state.runs[0].status).toBe('completed');
        expect(instance.state.pending).toBeNull();
      } finally {
        await cancelSchedules(instance);
      }
    });
  });

  it.each(['disabled', 'missing-key', 'blank-key'] as const)('stores observations without model calls when %s', async condition => {
    const { agent, observation } = await setup(condition);
    await runInDurableObject(agent, async (instance: OutbreakDirector) => {
      const runtimeEnv = (instance as unknown as { env: Env }).env;
      const savedEnabled = runtimeEnv.DIRECTOR_ENABLED;
      const savedKey = runtimeEnv.OPENAI_API_KEY;
      if (condition === 'disabled') runtimeEnv.DIRECTOR_ENABLED = 'false';
      else runtimeEnv.OPENAI_API_KEY = condition === 'blank-key' ? '  ' : undefined;
      try {
        await instance.observe(observation);
        const status = await instance.getStatus();
        expect(status.status).toBe('disabled');
        expect(status.enabled).toBe(condition !== 'disabled');
        expect(status.configured).toBe(condition === 'disabled');
        expect(status.latest).toEqual(observation);
        expect(status.pending).toBeNull();
        expect(status.runs).toHaveLength(0);
        expect(await instance.listSchedules()).toHaveLength(0);
        // Even a persisted wake from an earlier deployment must fail closed.
        const payload = { roundId: ROUND, dueAt: Date.now() };
        instance.setState({ ...instance.state, pending: { ...payload, trigger: 'round_started' } });
        await instance.wake(payload);
        expect(requests).toHaveLength(0);
        expect(instance.state.dailyUsage.requests).toBe(0);
        expect(JSON.stringify(status)).not.toContain(savedKey);
      } finally {
        instance.setState({ ...instance.state, pending: null });
        runtimeEnv.DIRECTOR_ENABLED = savedEnabled;
        runtimeEnv.OPENAI_API_KEY = savedKey;
        await cancelSchedules(instance);
      }
    });
  });
});
