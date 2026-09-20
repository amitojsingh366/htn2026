/** Optional server-side secret: absent keys disable model execution safely. */
declare namespace Cloudflare {
  interface Env { OPENAI_API_KEY?: string }
}
interface Env { OPENAI_API_KEY?: string }
