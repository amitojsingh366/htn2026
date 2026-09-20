import type { DirectorObservation, DirectorRecap, DirectorState } from './director-types';

export const COOLDOWN_MS = 15_000;
export const MAX_REQUESTS_PER_RUN = 4;
export const MAX_HISTORY = 40;
export function boundedInteger(value: string | undefined, fallback: number, min: number, max: number): number {
  const n = Number(value);
  return value && Number.isFinite(n) ? Math.max(min, Math.min(max, Math.floor(n))) : fallback;
}
export function limits(env: Env) {
  return {
    dailyRequests: boundedInteger(env.DIRECTOR_DAILY_REQUEST_LIMIT, 60, 1, 200),
    roundRequests: boundedInteger(env.DIRECTOR_ROUND_REQUEST_LIMIT, 18, 1, 40),
    outputTokens: boundedInteger(env.DIRECTOR_MAX_OUTPUT_TOKENS, 600, 128, 1200),
    cooldownMs: COOLDOWN_MS,
  };
}
export function initialDirectorState(): DirectorState {
  return { version: 1, operatorEnabled: true, controlRevision: 0, status: 'disabled', reason: 'Director is disabled.', model: '', latest: null,
    observations: [], actions: [], recaps: [], runs: [], followUps: [],
    dailyUsage: { day: '', requests: 0, inputTokens: 0, outputTokens: 0 },
    roundRequests: 0, lastRunAt: 0, activeRun: null, pending: null };
}
/** Heartbeats/ACKs without new game evidence never cause model calls. */
export function meaningfulChange(previous: DirectorObservation | null, next: DirectorObservation): string | null {
  if (!next.roundId) return null;
  if (!previous || previous.roundId !== next.roundId) return 'round_prepared';
  if (next.final && !previous.final) return 'result_finalized';
  if (next.winner !== previous.winner || next.endedAt !== previous.endedAt) return 'round_result';
  if (next.startedAt !== previous.startedAt) return 'round_started';
  if (next.infected !== previous.infected || next.acceptedEvents !== previous.acceptedEvents) return 'infection_change';
  return null;
}
export function cleanAnnouncement(value: unknown): string {
  if (typeof value !== 'string') throw new Error('Announcement must be text.');
  const clean = value.normalize('NFKD').replace(/[^\x20-\x7e]/g, ' ').replace(/[<>{}\[\]`*_#]/g, '').replace(/\s+/g, ' ').trim().slice(0, 96).trim();
  if (!clean) throw new Error('Announcement is empty.');
  return clean;
}
export function observationKey(o: DirectorObservation): string {
  return [o.roundId, o.revision, o.phase, o.infected, o.acceptedEvents, o.pendingEvents, o.rejectedEvents, o.winner, o.final].join(':');
}
/** No model-authored scores, winners, names or invented causal claims enter a recap. */
export function groundedRecap(o: DirectorObservation, args: Record<string, unknown>, now: number): DirectorRecap {
  if (!o.roundId || !o.endedAt || !o.winner) throw new Error('A recap requires an authoritative round result.');
  if (!Array.isArray(args.event_ids) || args.event_ids.length > 5 || args.event_ids.some(id => typeof id !== 'string')) throw new Error('Choose at most five evidence IDs.');
  const ids = [...new Set(args.event_ids as string[])];
  const evidence = ids.map(id => {
    const event = o.history.find(e => e.id === id && e.status === 'accepted');
    if (!event) throw new Error('Recap cites unknown or unaccepted evidence.');
    return event;
  });
  const title = args.headline === 'outbreak_contained' && o.winner === 'H' ? 'Outbreak contained'
    : args.headline === 'zombies_take_round' && o.winner === 'Z' ? 'Zombies take the round' : 'Round summary';
  const winner = o.winner === 'H' ? 'Humans' : 'Zombies';
  const durationSeconds = Math.max(0, Math.round((o.endedAt - (o.startedAt ?? o.endedAt)) / 1000));
  const text = `${winner} win${o.final ? ' (confirmed)' : ' (provisional; badge evidence is still syncing)'}. ${o.infected} of ${o.players} players infected; ${o.humans} humans remained after ${durationSeconds}s. ${o.acceptedEvents} accepted infection events; ${o.pendingEvents} pending and ${o.rejectedEvents} rejected.`
    + evidence.map(e => ` At ${Math.round(e.elapsedMs / 1000)}s, slot ${e.actor} infected slot ${e.victim} [${e.id}].`).join('');
  return { roundId: o.roundId, at: now, title, text, final: o.final,
    facts: { winner: o.winner, players: o.players, infected: o.infected, humans: o.humans, durationSeconds,
      acceptedEvents: o.acceptedEvents, pendingEvents: o.pendingEvents, rejectedEvents: o.rejectedEvents, final: o.final }, evidenceIds: ids };
}
