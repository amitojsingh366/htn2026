import { env, exports } from 'cloudflare:workers';
import { runInDurableObject } from 'cloudflare:test';
import { getAgentByName } from 'agents';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

beforeEach(() => {
  vi.spyOn(globalThis, 'fetch').mockRejectedValue(new Error('Status reads must not invoke an external service'));
});
afterEach(() => vi.restoreAllMocks());

describe('Director status before the first game observation', () => {
  it('reports an enabled, configured director as idle through the public status route', async () => {
    const response = await exports.default.fetch(`https://example.com/api/v1/games/${env.DIRECTOR_GAME_ID}/director`);
    expect(response.status).toBe(200);
    expect(await response.json()).toMatchObject({
      version: 1, enabled: true, configured: true, status: 'idle',
      reason: 'Waiting for a prepared live badge round.', latest: null,
      observations: [], actions: [], runs: [], pending: null,
      dailyUsage: { requests: 0 }, roundRequests: 0,
    });
    expect(globalThis.fetch).not.toHaveBeenCalled();
  });

  it.each([
    { enabled: 'false', key: 'test-key-never-real', configured: true, reason: 'Director is disabled by server configuration.' },
    { enabled: 'true', key: '', configured: false, reason: 'OPENAI_API_KEY is not configured on the Worker.' },
    { enabled: 'true', key: '   ', configured: false, reason: 'OPENAI_API_KEY is not configured on the Worker.' },
  ])('keeps disabled configuration explicit: $reason (key=$configured)', async ({ enabled, key, configured, reason }) => {
    const agent = await getAgentByName(env.OUTBREAK_DIRECTOR, `status-${enabled}-${configured}-${key.length}`);
    await runInDurableObject(agent, async instance => {
      const runtimeEnv = (instance as unknown as { env: Env }).env;
      const previousEnabled = runtimeEnv.DIRECTOR_ENABLED, previousKey = runtimeEnv.OPENAI_API_KEY;
      try {
        runtimeEnv.DIRECTOR_ENABLED = enabled;
        runtimeEnv.OPENAI_API_KEY = key;
        expect(await instance.getStatus()).toMatchObject({
          enabled: enabled === 'true', configured, status: 'disabled', reason,
          latest: null, dailyUsage: { requests: 0 }, runs: [],
        });
      } finally {
        runtimeEnv.DIRECTOR_ENABLED = previousEnabled;
        runtimeEnv.OPENAI_API_KEY = previousKey;
      }
    });
    expect(globalThis.fetch).not.toHaveBeenCalled();
  });
});
