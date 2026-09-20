export type JsonScalar = string | number | boolean | null;
/** Tool audit fields are deliberately shallow and bounded. */
export type JsonValue = JsonScalar | JsonScalar[];

/** Bounded authoritative input; never accepts observations from a public client. */
export interface DirectorObservation {
  roomId: string;
  gameId: string;
  roundId: string | null;
  revision: number;
  observedAt: number;
  phase: string;
  startedAt: number | null;
  endedAt: number | null;
  durationMs: number;
  players: number;
  infected: number;
  humans: number;
  winner: 'H' | 'Z' | null;
  final: boolean;
  hostConnected: boolean;
  hostLastSeenAt: number | null;
  pendingEvents: number;
  rejectedEvents: number;
  acceptedEvents: number;
  history: Array<{ id: string; actor: number; victim: number; elapsedMs: number; status: string }>;
}
export interface AnnouncementRequest {
  actionId: string;
  roundId: string;
  revision: number;
  text: string;
  expiresAt: number;
}
export interface AnnouncementResult {
  status: 'queued' | 'duplicate' | 'rejected' | 'applied' | 'expired';
  reason: string;
  commandSeq?: number;
  acknowledged?: number[];
}
export interface DirectorAction {
  id: string;
  at: number;
  roundId: string;
  tool: string;
  arguments: Record<string, JsonValue>;
  result: Record<string, JsonValue>;
}
export interface DirectorRecap {
  roundId: string;
  at: number;
  title: string;
  text: string;
  final: boolean;
  facts: Record<string, string | number | boolean | null>;
  evidenceIds: string[];
}
export interface DirectorRun {
  id: string;
  at: number;
  trigger: string;
  roundId: string;
  status: 'running' | 'completed' | 'failed' | 'stale';
  model: string;
  responseIds: string[];
  requestIds: string[];
  inputTokens: number;
  outputTokens: number;
  summary: string;
}
export interface DirectorState {
  version: 1;
  /** Absent on state saved before the operator toggle; defaults to true. */
  operatorEnabled?: boolean;
  controlRevision?: number;
  status: 'disabled' | 'idle' | 'thinking' | 'waiting' | 'degraded' | 'budget_exhausted';
  reason: string;
  model: string;
  latest: DirectorObservation | null;
  observations: DirectorObservation[];
  actions: DirectorAction[];
  recaps: DirectorRecap[];
  runs: DirectorRun[];
  followUps: Array<{ id: string; token: string; roundId: string; dueAt: number; reason: string }>;
  dailyUsage: { day: string; requests: number; inputTokens: number; outputTokens: number };
  roundRequests: number;
  lastRunAt: number;
  pending: { roundId: string; trigger: string; dueAt: number; scheduleId?: string } | null;
  activeRun: { id: string; roundId: string; expiresAt: number } | null;
}
export interface DirectorStatus extends DirectorState {
  operatorEnabled: boolean;
  serverEnabled: boolean;
  enabled: boolean;
  configured: boolean;
  limits: { dailyRequests: number; roundRequests: number; outputTokens: number; cooldownMs: number };
}
