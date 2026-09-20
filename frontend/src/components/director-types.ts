// Read-only JSON contract from backend/src/director-types.ts. Never contains a key.
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

export interface DirectorAction {
  id: string;
  at: number;
  roundId: string;
  tool: string;
  arguments: Record<string, unknown>;
  result: Record<string, unknown>;
}

export interface DirectorStatus {
  version: 1;
  enabled: boolean;
  configured: boolean;
  status: 'disabled' | 'idle' | 'thinking' | 'waiting' | 'degraded' | 'budget_exhausted';
  reason: string;
  model: string;
  latest: DirectorObservation | null;
  observations: DirectorObservation[];
  actions: DirectorAction[];
  recaps: Array<{
    roundId: string;
    at: number;
    title: string;
    text: string;
    final: boolean;
    facts: Record<string, string | number | boolean | null>;
    evidenceIds: string[];
  }>;
  runs: Array<{
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
  }>;
  followUps: Array<{ id: string; roundId: string; dueAt: number; reason: string }>;
  dailyUsage: { day: string; requests: number; inputTokens: number; outputTokens: number };
  roundRequests: number;
  lastRunAt: number;
  activeRun: { id: string; roundId: string; expiresAt: number } | null;
  limits: { dailyRequests: number; roundRequests: number; outputTokens: number; cooldownMs: number };
}
