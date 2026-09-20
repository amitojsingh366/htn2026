import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [cloudflareTest({ wrangler: { configPath: "./wrangler.jsonc" }, miniflare: { bindings: { ZT_GATEWAY_TOKEN: "test-token", ZT_HOST_MAC: "aabbccddeeff" } } })],
});
