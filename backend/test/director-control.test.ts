import { env, exports } from 'cloudflare:workers';
import { evictDurableObject, runInDurableObject } from 'cloudflare:test';
import { getAgentByName } from 'agents';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { OutbreakDirector } from '../src/outbreak-director';

const ROUND = 'fedcba9876543210';
beforeEach(() => {
  vi.spyOn(globalThis, 'fetch').mockRejectedValue(new Error('Live provider calls are forbidden in control tests'));
});
afterEach(() => vi.restoreAllMocks());

async function setup(name: string) {
  const room = env.GAME_ROOM.getByName(`control-${name}`);
  await runInDurableObject(room, (_instance, state) => {
    const meta = JSON.parse(state.storage.sql.exec<{ body: string }>('SELECT body FROM gateway_meta WHERE id=1').one().body);
    Object.assign(meta, { gameId: env.DIRECTOR_GAME_ID, roundId: ROUND, revision: 4, phase: 'running', startSeq: 1,
      startTime: Date.now() - 30_000, patientZero: 0,
      roster: [{ slot: 0, id: '111122223333', name: 'Host' }, { slot: 1, id: '444455556666', name: 'Human' }] });
    state.storage.sql.exec('UPDATE gateway_meta SET body=? WHERE id=1', JSON.stringify(meta));
  });
  const agent = await getAgentByName(env.OUTBREAK_DIRECTOR, room.id.toString());
  await runInDurableObject(agent, instance => instance.setState({ ...instance.state, lastRunAt: Date.now() + 60_000 }));
  return { room, agent, observation: await room.getDirectorObservation() };
}

async function cancelSchedules(instance: OutbreakDirector) {
  for (const schedule of await instance.listSchedules()) await instance.cancelSchedule(schedule.id);
}

const announcement = [{ type: 'function_call', id: 'fc_control_mock', call_id: 'call_control_mock', status: 'completed',
  name: 'send_announcement', arguments: JSON.stringify({ text: 'Watch for zombies!' }) }];
function response(output = announcement) {
  return Response.json({ id: 'resp_control_mock', object: 'response', status: 'completed', model: 'gpt-4.1-mini', output,
    usage: { input_tokens: 20, output_tokens: 10, total_tokens: 30 } });
}

describe('Durable outbreak director operator setting', () => {
  it('persists disable across eviction, cancels pending work and keeps history and budgets', async () => {
    const { room, agent, observation } = await setup('persist');
    await agent.observe(observation);
    await runInDurableObject(agent, async instance => {
      const follow = await instance.schedule(120, 'followUp', { id: 'saved-token', roundId: ROUND, reason: 'Check later' });
      instance.setState({ ...instance.state, roundRequests: 5,
        dailyUsage: { day: '2026-09-20', requests: 9, inputTokens: 100, outputTokens: 30 },
        followUps: [{ id: follow.id, token: 'saved-token', roundId: ROUND, reason: 'Check later', dueAt: Date.now() + 120_000 }] });
    });
    const before = await agent.getStatus();
    expect(before.pending?.scheduleId).toBeTruthy();
    const disabled = await room.setDirectorEnabled(env.DIRECTOR_GAME_ID, false);
    expect(disabled).toMatchObject({ operatorEnabled: false, serverEnabled: true, enabled: false, configured: true,
      status: 'disabled', pending: null, followUps: [], activeRun: null, roundRequests: 5, dailyUsage: before.dailyUsage });
    expect(disabled.observations).toEqual(before.observations);
    expect(await runInDurableObject(agent, instance => instance.listSchedules())).toHaveLength(0);
    await evictDurableObject(agent);
    expect(await agent.getStatus()).toMatchObject({ operatorEnabled: false, enabled: false, dailyUsage: before.dailyUsage, roundRequests: 5 });
    await agent.observe({ ...observation, revision: 5, infected: 2, humans: 0, observedAt: Date.now() + 1 });
    expect((await agent.getStatus()).pending).toBeNull();
    expect(globalThis.fetch).not.toHaveBeenCalled();
  });

  it('enables from current canonical state once and does not reset same-round budgets', async () => {
    const { room, agent, observation } = await setup('enable');
    await agent.observe(observation);
    await room.setDirectorEnabled(env.DIRECTOR_GAME_ID, false);
    await runInDurableObject(agent, instance => instance.setState({ ...instance.state, roundRequests: 3 }));
    await runInDurableObject(room, (_instance, state) => {
      const meta = JSON.parse(state.storage.sql.exec<{ body: string }>('SELECT body FROM gateway_meta WHERE id=1').one().body);
      state.storage.sql.exec('UPDATE gateway_meta SET body=? WHERE id=1', JSON.stringify({ ...meta, revision: 5 }));
    });
    const enabled = await room.setDirectorEnabled(env.DIRECTOR_GAME_ID, true);
    expect(enabled).toMatchObject({ enabled: true, operatorEnabled: true, serverEnabled: true, configured: true,
      status: 'waiting', latest: { revision: 5, roundId: ROUND }, roundRequests: 3 });
    expect(enabled.pending?.scheduleId).toBeTruthy();
    const repeated = await room.setDirectorEnabled(env.DIRECTOR_GAME_ID, true);
    expect(repeated.pending).toEqual(enabled.pending);
    expect(repeated.controlRevision).toBe(enabled.controlRevision);
    expect(globalThis.fetch).not.toHaveBeenCalled();
    await runInDurableObject(agent, cancelSchedules);
  });

  it.each([false, true])('rejects late model tools after disable (quick re-enable=%s)', async reenable => {
    const { agent, observation } = await setup(`request-race-${reenable}`);
    await agent.observe(observation);
    await runInDurableObject(agent, async instance => {
      const pending = instance.state.pending!;
      vi.mocked(globalThis.fetch).mockImplementationOnce(async () => {
        await instance.setEnabled(false, observation);
        if (reenable) await instance.setEnabled(true, observation);
        return response();
      });
      try {
        await instance.wake({ roundId: pending.roundId, dueAt: pending.dueAt });
        const status = await instance.getStatus();
        expect(status.enabled).toBe(reenable);
        expect(status.status).toBe(reenable ? 'waiting' : 'disabled');
        expect(status.activeRun).toBeNull();
        expect(status.actions).toHaveLength(0);
        expect(status.runs).toHaveLength(1);
        expect(status.runs[0].status).toBe('stale');
        expect(status.dailyUsage.requests).toBe(1);
        expect(status.roundRequests).toBe(1);
        expect(status.pending !== null).toBe(reenable);
        expect(globalThis.fetch).toHaveBeenCalledTimes(1);
        // A delivered wake from the old generation cannot consume new work.
        if (status.pending) await instance.wake({ roundId: status.pending.roundId, dueAt: status.pending.dueAt, controlRevision: 0 });
        expect(globalThis.fetch).toHaveBeenCalledTimes(1);
      } finally { await cancelSchedules(instance); }
    });
  });

  it('keeps the server emergency gate and absent key authoritative when enabled from the dashboard', async () => {
    const { agent, observation } = await setup('server-gates');
    await runInDurableObject(agent, async instance => {
      const runtimeEnv = (instance as unknown as { env: Env }).env;
      const previousEnabled = runtimeEnv.DIRECTOR_ENABLED, previousKey = runtimeEnv.OPENAI_API_KEY;
      try {
        for (const gate of ['server', 'key']) {
          await instance.setEnabled(false, observation);
          runtimeEnv.DIRECTOR_ENABLED = gate === 'server' ? 'false' : 'true';
          runtimeEnv.OPENAI_API_KEY = gate === 'key' ? '' : previousKey;
          expect(await instance.setEnabled(true, observation)).toMatchObject({
            operatorEnabled: true, serverEnabled: gate !== 'server', configured: gate !== 'key', status: 'disabled', pending: null,
          });
        }
        expect(globalThis.fetch).not.toHaveBeenCalled();
      } finally { runtimeEnv.DIRECTOR_ENABLED = previousEnabled; runtimeEnv.OPENAI_API_KEY = previousKey; }
    });
  });

  it('does not let an old response overwrite a newer run after disable and re-enable', async () => {
    const { agent, observation } = await setup('newer-run');
    await agent.observe(observation);
    await runInDurableObject(agent, async instance => {
      const first = instance.state.pending!;
      vi.mocked(globalThis.fetch).mockImplementationOnce(async () => {
        await instance.setEnabled(false, observation);
        await instance.setEnabled(true, observation);
        const next = instance.state.pending!;
        vi.mocked(globalThis.fetch).mockResolvedValueOnce(Response.json({
          id: 'resp_newer_mock', object: 'response', status: 'completed', model: 'gpt-4.1-mini', output: [],
          usage: { input_tokens: 20, output_tokens: 10, total_tokens: 30 },
        }));
        await instance.wake({ roundId: next.roundId, dueAt: next.dueAt, controlRevision: instance.state.controlRevision });
        return response();
      });
      try {
        await instance.wake({ roundId: first.roundId, dueAt: first.dueAt });
        const status = await instance.getStatus();
        expect(status.status).toBe('idle');
        expect(status.reason).toBe('Waiting for meaningful game events.');
        expect(status.runs.map(run => run.status)).toEqual(['stale', 'completed']);
        expect(status.pending).toBeNull();
        expect(status.activeRun).toBeNull();
        expect(status.actions).toHaveLength(0);
        expect(status.dailyUsage.requests).toBe(2);
        expect(globalThis.fetch).toHaveBeenCalledTimes(2);
      } finally { await cancelSchedules(instance); }
    });
  });

  it('bounds HTTP controls, rejects other games, and returns the durable setting on GET', async () => {
    const url = `https://example.com/api/v1/games/${env.DIRECTOR_GAME_ID}/director/enabled`;
    for (const body of ['{}', '{"enabled":"false"}', '[]', 'null', '{"enabled":true,"other":1}', 'invalid']) {
      expect((await exports.default.fetch(url, { method: 'POST', body })).status).toBe(400);
    }
    expect((await exports.default.fetch(url, { method: 'POST', body: ' '.repeat(257) })).status).toBe(413);
    expect((await exports.default.fetch(url.replace(env.DIRECTOR_GAME_ID, 'unconfigured'), { method: 'POST', body: '{"enabled":true}' })).status).toBe(404);
    const off = await exports.default.fetch(url, { method: 'POST', body: '{"enabled":false}' });
    expect(off.status).toBe(200);
    expect(await off.json()).toMatchObject({ operatorEnabled: false, enabled: false, status: 'disabled' });
    const read = await exports.default.fetch(url.replace('/enabled', ''));
    expect(await read.json()).toMatchObject({ operatorEnabled: false, enabled: false });
    const on = await exports.default.fetch(url, { method: 'POST', body: '{"enabled":true}' });
    expect(on.status).toBe(200);
    expect(await on.json()).toMatchObject({ operatorEnabled: true, enabled: true, status: 'idle', pending: null });
    expect(globalThis.fetch).not.toHaveBeenCalled();
  });
});
