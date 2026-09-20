import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [cloudflareTest({
    wrangler: { configPath: "./wrangler.jsonc" },
    miniflare: {
      bindings: {
        DIRECTOR_ENABLED: "true",
        OPENAI_API_KEY: "test-key-never-real",
        ZT_GATEWAY_TOKEN: "test-token",
        ZT_HOST_MAC: "aabbccddeeff",
        // Tests must never export data to the configured production project.
        SENTRY_DSN: "",
        SENTRY_ENVIRONMENT: "test",
        SENTRY_RELEASE: "test",
        SENTRY_TRACES_SAMPLE_RATE: "0",
      },
    },
  })],
});
