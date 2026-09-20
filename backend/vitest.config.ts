import { cloudflareTest } from '@cloudflare/vitest-pool-workers';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: { configPath: './wrangler.jsonc' },
    // The newest test-pool runtime currently supports dates through 2026-08-22.
    // Production keeps 2026-09-19 and is separately checked with Wrangler.
    miniflare: { compatibilityDate: '2026-08-22', bindings: { DIRECTOR_ENABLED: 'true', OPENAI_API_KEY: 'test-key-never-real' } },
  })],
});
