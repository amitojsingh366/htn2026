/**
 * `Env` is generated into worker-configuration.d.ts by `npm run cf-typegen`.
 * Rerun it after changing bindings in wrangler.jsonc.
 */

/** Wire format sent over HTTP and pushed over the WebSocket. */
export interface PopulationState {
  num_players: number;
  num_infected: number;
  num_humans: number;
  survived_pct: number;
}

/** Game used by the legacy unprefixed routes. */
export const DEFAULT_GAME_ID = "default";
