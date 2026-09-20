/**
 * `Env` is generated into worker-configuration.d.ts by `npm run cf-typegen`.
 * Rerun it after changing bindings in wrangler.jsonc.
 */

/** Wire format sent over HTTP and pushed over the WebSocket. */
export interface PlayerRanking {
  rank: number;
  device_id: string;
  state: "infected" | "not infected";
  infected_at: number | null;
  survival_time_seconds: number;
}

export interface PopulationState {
  num_players: number;
  num_infected: number;
  num_humans: number;
  survived_pct: number;
  game_over?: boolean;
  started_at?: number | null;
  ended_at?: number | null;
  patient_zero_id?: string | null;
  rankings?: PlayerRanking[];
}

/** Ingested payload from ESP devices */
export interface DeviceEventInput {
  device_id?: string;
  deviceId?: string;
  timestamp?: number | string;
  time_stamp?: number | string;
  time?: number | string;
  current_state?: "infected" | "not infected" | boolean | string;
  state?: "infected" | "not infected" | boolean | string;
}

export interface LeaderboardResponse {
  game_id: string;
  game_over: boolean;
  total_players: number;
  num_infected: number;
  rankings: PlayerRanking[];
}

export interface StartGameResponse {
  message: string;
  game_id: string;
  started_at: number;
  patient_zero_id: string | null;
  num_players: number;
  num_infected: number;
  state: PopulationState;
}

export interface DeviceStateResponse {
  device_id: string;
  role: "infected" | "not infected";
  is_infected: boolean;
  game_started: boolean;
  started_at: number | null;
  game_over: boolean;
}

/** Game used by the legacy unprefixed routes. */
export const DEFAULT_GAME_ID = "default";
