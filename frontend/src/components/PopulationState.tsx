import React, { useEffect, useState, useCallback, useRef } from 'react';
import './PopulationState.css';

export interface PopulationStateData {
  gateway_mode?: boolean;
  gateway_phase?: string;
  ready_players?: number;
  registered_players?: number;
  frozen_roster_players?: number;
  start_applied_players?: number;
  ready_slots?: number[];
  start_applied_slots?: number[];
  reset_applied_players?: number;
  events_received?: number;
  events_pending?: number;
  events_rejected?: number;
  host_connected?: boolean;
  host_last_seen_at?: number | null;
  players?: Array<{slot: number; id: string; name: string; role?: 'H' | 'Z'; role_rev?: number; covered_seq?: number}>;
  num_players: number;
  num_infected: number;
  num_humans: number;
  survived_pct: number;
  game_over?: boolean;
  winner?: 'H' | 'Z' | null;
  ended_at?: number | null;
  result_final?: boolean;
  result_complete?: boolean;
  started_at?: number | null;
  server_time_ms?: number;
  patient_zero_id?: string | null;
  rankings?: Array<{
    rank: number;
    device_id: string;
    state: string;
    survival_time_seconds: number;
  }>;
}

export interface PopulationStateProps {
  /** Base URL for the Worker backend (e.g. "http://localhost:8787") */
  apiBaseUrl?: string;
  /** Custom WebSocket URL (e.g. "ws://localhost:8787/ws/population"). Auto-derived from apiBaseUrl if omitted. */
  wsUrl?: string;
  /** Whether to use WebSocket for 0-latency live streaming. Defaults to true. */
  useWebSocket?: boolean;
  /** Polling interval in ms (alias for fallbackPollIntervalMs). */
  pollIntervalMs?: number;
  /** Fallback polling interval in ms if WebSocket is unavailable. Set to 0 to disable polling. Defaults to 2000ms. */
  fallbackPollIntervalMs?: number;
  /** Optional manual override for total players */
  totalPlayers?: number;
  /** Optional manual override for infected count */
  infectedCount?: number;
  /** Optional CSS class name */
  className?: string;
  /** Callback fired whenever state updates, with an optional latency-adjusted server clock. */
  onUpdate?: (data: PopulationStateData, estimatedServerTime?: number) => void;
  /** Whether the dashboard has a working connection to the backend. */
  onConnectionChange?: (connected: boolean) => void;
  /** Show live connection pulse indicator in header */
  showLiveIndicator?: boolean;
}

export const PopulationState: React.FC<PopulationStateProps> = ({
  apiBaseUrl = 'https://htn2026-backend.amitoj.workers.dev/api/v1/games/005a544d454d4f01',
  wsUrl,
  useWebSocket = true,
  pollIntervalMs,
  fallbackPollIntervalMs = 2000,
  totalPlayers: manualTotal,
  infectedCount: manualInfected,
  className = '',
  onUpdate,
  onConnectionChange,
  showLiveIndicator = false,
}) => {
  const effectiveFallbackInterval = pollIntervalMs ?? fallbackPollIntervalMs;
  const [receivedData, setData] = useState<PopulationStateData>({
    num_players: manualTotal ?? 0,
    num_infected: manualInfected ?? 0,
    num_humans: Math.max(0, (manualTotal ?? 0) - (manualInfected ?? 0)),
    survived_pct: 0.0,
  });
  // Manual preview values are derived from props instead of copying props into state.
  const manualHumans = Math.max(0, (manualTotal ?? 0) - (manualInfected ?? 0));
  const data: PopulationStateData = manualTotal !== undefined && manualInfected !== undefined ? {
    num_players: manualTotal,
    num_infected: manualInfected,
    num_humans: manualHumans,
    survived_pct: Number((manualTotal > 0 ? (manualHumans / manualTotal) * 100 : 0).toFixed(1)),
  } : receivedData;
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [isWebSocketActive, setIsWebSocketActive] = useState<boolean>(false);
  const [error, setError] = useState<string | null>(null);

  const socketRef = useRef<WebSocket | null>(null);
  const reconnectTimeoutRef = useRef<number | null>(null);
  const updateSequenceRef = useRef(0);
  const httpPendingRef = useRef(false);
  useEffect(() => { onConnectionChange?.(isConnected); }, [isConnected, onConnectionChange]);

  // Manual values override handler
  useEffect(() => {
    if (manualTotal !== undefined && manualInfected !== undefined) {
      const humans = Math.max(0, manualTotal - manualInfected);
      const ratio = manualTotal > 0 ? (humans / manualTotal) * 100 : 0;
      const customData: PopulationStateData = {
        num_players: manualTotal,
        num_infected: manualInfected,
        num_humans: humans,
        survived_pct: Number(ratio.toFixed(1)),
      };
      onUpdate?.(customData);
    }
  }, [manualTotal, manualInfected, onUpdate]);

  // HTTP Fetch function (used on mount & as fallback)
  const fetchHttpState = useCallback(async () => {
    if ((manualTotal !== undefined && manualInfected !== undefined) || httpPendingRef.current) return;
    httpPendingRef.current = true;
    const sequence = updateSequenceRef.current;
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 8000);
    const requestStartedAt = performance.now();

    try {
      const res = await fetch(`${apiBaseUrl}/population-state`, { signal: controller.signal, cache: 'no-store' });
      if (!res.ok) throw new Error(`Backend returned ${res.status}`);
      if (res.ok) {
        const json = await res.json();
        // An older HTTP request must not overwrite a more recent pushed state.
        if (sequence !== updateSequenceRef.current) return;
        const total = manualTotal ?? json.num_players ?? 0;
        const infected = manualInfected ?? json.num_infected ?? 0;
        const humans = json.num_humans ?? Math.max(0, total - infected);
        const pct = json.survived_pct ?? (total > 0 ? (humans / total) * 100 : 0);

        const updated: PopulationStateData = {
          num_players: total,
          num_infected: infected,
          num_humans: humans,
          survived_pct: Number(pct.toFixed(1)),
          game_over: json.game_over,
          winner: json.winner, ended_at: json.ended_at,
          result_final: json.result_final, result_complete: json.result_complete,
          started_at: json.started_at,
          server_time_ms: json.server_time_ms,
          patient_zero_id: json.patient_zero_id,
          rankings: json.rankings,
          gateway_mode: json.gateway_mode, gateway_phase: json.gateway_phase,
          ready_players: json.ready_players, registered_players: json.registered_players,
          frozen_roster_players: json.frozen_roster_players, start_applied_players: json.start_applied_players,
          ready_slots: json.ready_slots, start_applied_slots: json.start_applied_slots,
          reset_applied_players: json.reset_applied_players, events_received: json.events_received,
          events_pending: json.events_pending, events_rejected: json.events_rejected,
          host_last_seen_at: json.host_last_seen_at,
          host_connected: json.host_connected, players: json.players,
        };

        setData(updated);
        updateSequenceRef.current++;
        setIsConnected(true);
        setError(null);
        onUpdate?.(updated, typeof json.server_time_ms === 'number'
          ? json.server_time_ms + (performance.now() - requestStartedAt) / 2
          : undefined);
      }
    } catch (err: unknown) {
      if (sequence !== updateSequenceRef.current) return;
      setIsConnected(false);
      setError(err instanceof Error ? err.message : 'Backend unreachable');
    } finally {
      clearTimeout(timeout);
      httpPendingRef.current = false;
    }
  }, [apiBaseUrl, manualTotal, manualInfected, onUpdate]);

  // WebSocket lifecycle management
  useEffect(() => {
    if (!useWebSocket || (manualTotal !== undefined && manualInfected !== undefined)) {
      return;
    }

    const targetWsUrl = wsUrl || `${apiBaseUrl.replace(/^http/, 'ws')}/ws/population`;
    let isUnmounted = false;

    const connectWebSocket = () => {
      if (isUnmounted) return;

      try {
        const socket = new WebSocket(targetWsUrl);
        socketRef.current = socket;

        socket.onopen = () => {
          if (isUnmounted) {
            socket.close();
            return;
          }
          setIsWebSocketActive(true);
          setError(null);
        };

        socket.onmessage = (event) => {
          try {
            const rawData = JSON.parse(event.data);
            const total = manualTotal ?? rawData.num_players ?? 0;
            const infected = manualInfected ?? rawData.num_infected ?? 0;
            const humans = rawData.num_humans ?? Math.max(0, total - infected);
            const pct = rawData.survived_pct ?? (total > 0 ? (humans / total) * 100 : 0);

            const updated: PopulationStateData = {
              num_players: total,
              num_infected: infected,
              num_humans: humans,
              survived_pct: Number(pct.toFixed(1)),
              game_over: rawData.game_over,
              winner: rawData.winner, ended_at: rawData.ended_at,
              result_final: rawData.result_final, result_complete: rawData.result_complete,
              started_at: rawData.started_at,
              server_time_ms: rawData.server_time_ms,
              patient_zero_id: rawData.patient_zero_id,
              rankings: rawData.rankings,
              gateway_mode: rawData.gateway_mode, gateway_phase: rawData.gateway_phase,
              ready_players: rawData.ready_players, registered_players: rawData.registered_players,
              frozen_roster_players: rawData.frozen_roster_players, start_applied_players: rawData.start_applied_players,
              ready_slots: rawData.ready_slots, start_applied_slots: rawData.start_applied_slots,
              reset_applied_players: rawData.reset_applied_players, events_received: rawData.events_received,
              events_pending: rawData.events_pending, events_rejected: rawData.events_rejected,
              host_last_seen_at: rawData.host_last_seen_at,
              host_connected: rawData.host_connected, players: rawData.players,
            };

            setData(updated);
            updateSequenceRef.current++;
            setIsConnected(true);
            setError(null);
            onUpdate?.(updated);
          } catch (e) {
            console.error('Error parsing WebSocket message:', e);
          }
        };

        socket.onclose = () => {
          setIsWebSocketActive(false);
          setIsConnected(false);
          if (!isUnmounted) {
            // Schedule reconnection attempt
            reconnectTimeoutRef.current = window.setTimeout(connectWebSocket, 2000);
          }
        };

        socket.onerror = () => {
          setIsWebSocketActive(false);
          setIsConnected(false);
        };
      } catch {
        setIsWebSocketActive(false);
        if (!isUnmounted) {
          reconnectTimeoutRef.current = window.setTimeout(connectWebSocket, 2000);
        }
      }
    };

    // Initial HTTP fetch so data shows up instantly while WS connects
    const initialConnection = window.setTimeout(() => {
      void fetchHttpState();
      connectWebSocket();
    }, 0);

    return () => {
      isUnmounted = true;
      window.clearTimeout(initialConnection);
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
      }
      if (socketRef.current) {
        socketRef.current.close();
      }
    };
  }, [useWebSocket, wsUrl, apiBaseUrl, manualTotal, manualInfected, fetchHttpState, onUpdate]);

  // Also refresh occasionally with a socket open: host connectivity can expire
  // without a population change, and an idle dashboard socket can be stale.
  useEffect(() => {
    if (effectiveFallbackInterval <= 0) return;

    const initialPoll = window.setTimeout(() => void fetchHttpState(), 0);
    const interval = setInterval(fetchHttpState, isWebSocketActive ? Math.max(10_000, effectiveFallbackInterval) : effectiveFallbackInterval);
    return () => {
      window.clearTimeout(initialPoll);
      clearInterval(interval);
    };
  }, [isWebSocketActive, effectiveFallbackInterval, fetchHttpState]);

  // Derived percentages for progress bar rendering
  const total = data.num_players > 0 ? data.num_players : data.num_humans + data.num_infected;
  const humanPct = total > 0 ? Math.min(100, Math.max(0, (data.num_humans / total) * 100)) : 0;
  const infectedPct = total > 0 ? Math.min(100, Math.max(0, (data.num_infected / total) * 100)) : 0;
  const rolesAssigned = Boolean(data.started_at);

  return (
    <div className={`population-state-card ${className}`} role="region" aria-label="Game Population State">
      {/* Header Row */}
      <div className="population-state-header">
        <div className="population-state-title">
          {/* Neon Group / Players Icon */}
          <svg
            className="population-state-icon"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            aria-hidden="true"
          >
            <path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2" />
            <circle cx="9" cy="7" r="4" />
            <path d="M22 21v-2a4 4 0 0 0-3-3.87" />
            <path d="M16 3.13a4 4 0 0 1 0 7.75" />
          </svg>
          <span className="population-state-heading">{data.gateway_mode ? rolesAssigned ? 'SERVER ROLE COUNTS' : 'REGISTERED BADGES' : 'GAME POPULATION STATE'}</span>
          {showLiveIndicator && (
            <span
              className={`live-pulse-dot ${isConnected ? 'online' : 'offline'}`}
              title={
                !isConnected
                  ? `Dashboard feed unavailable: ${error ?? 'Attempting reconnection'}`
                  : isWebSocketActive
                  ? 'Dashboard feed connected'
                  : 'HTTP polling connected'
              }
            />
          )}
        </div>

        {/* Survived percentage badge */}
        <div className="population-state-badge">
          {data.gateway_mode ? `${data.registered_players ?? total}/20 saved` : `${data.survived_pct.toFixed(1)}% Survived`}
        </div>
      </div>

      {/* Progress Bar */}
      {(!data.gateway_mode || rolesAssigned) && <div className="population-progress-track">
        <div
          className="population-progress-survivors"
          style={{ width: `${humanPct}%` }}
          aria-label={`${humanPct.toFixed(1)}% humans`}
        />
        <div
          className="population-progress-zombies"
          style={{ width: `${infectedPct}%` }}
          aria-label={`${infectedPct.toFixed(1)}% zombies`}
        />
      </div>}

      {/* Footer Stats Row */}
      {data.gateway_mode && !rolesAssigned ? <div className="population-registration-summary">
        <span>Roles unassigned</span>
        <span>Online badges: unknown</span>
      </div> : <div className="population-state-footer">
        <div className="population-stat-survivors">
          <span className="stat-dot dot-survivors" aria-hidden="true" />
          <span>{data.gateway_mode ? 'Server humans' : 'Humans'}: {data.num_humans}</span>
        </div>

        <div className="population-stat-zombies">
          <span>{data.gateway_mode ? 'Server zombies' : 'Zombies'}: {data.num_infected}</span>
          <span className="stat-dot dot-zombies" aria-hidden="true" />
        </div>
      </div>}
      <p className={`population-feed-status ${isConnected ? '' : 'unavailable'}`} role="status">
        {isConnected ? 'Dashboard feed connected.' : `Dashboard feed unavailable${error ? `: ${error}` : '; reconnecting'}. Values may be stale.`}
        {data.gateway_mode && (rolesAssigned ? ' Counts include accepted infection events. Offline tags appear after the host uploads their evidence.' : ' Registrations remain saved when badges are powered off. Badge online status is not reported.')}
      </p>
    </div>
  );
};

export default PopulationState;
