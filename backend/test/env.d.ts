declare module "cloudflare:test" {
  // Gives `env` in tests the same bindings as the Worker.
  interface ProvidedEnv extends Env {}
}
