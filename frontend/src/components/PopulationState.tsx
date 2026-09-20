import React, { useEffect, useState, useCallback, useRef } from 'react';
import './PopulationState.css';

export interface PopulationStateData {
  num_players: number;
  num_infected: number;
  num_humans: number;
  survived_pct: number;
}

export interface PopulationStateProps {
  /** Base URL for the FastAPI backend (e.g. "http://localhost:8000") */
  apiBaseUrl?: string;
  /** Custom WebSocket URL (e.g. "ws://localhost:8000/ws/population"). Auto-derived from apiBaseUrl if omitted. */
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
  /** Callback fired whenever state updates */
  onUpdate?: (data: PopulationStateData) => void;
  /** Show live connection pulse indicator in header */
  showLiveIndicator?: boolean;
}

export const PopulationState: React.FC<PopulationStateProps> = ({
  apiBaseUrl = 'http://localhost:8000',
  wsUrl,
  useWebSocket = true,
  pollIntervalMs,
  fallbackPollIntervalMs = 2000,
  totalPlayers: manualTotal,
  infectedCount: manualInfected,
  className = '',
  onUpdate,
  showLiveIndicator = false,
}) => {
  const effectiveFallbackInterval = pollIntervalMs ?? fallbackPollIntervalMs;
  const [data, setData] = useState<PopulationStateData>({
    num_players: manualTotal ?? 0,
    num_infected: manualInfected ?? 0,
    num_humans: Math.max(0, (manualTotal ?? 0) - (manualInfected ?? 0)),
    survived_pct: 0.0,
  });
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [isWebSocketActive, setIsWebSocketActive] = useState<boolean>(false);
  const [error, setError] = useState<string | null>(null);

  const socketRef = useRef<WebSocket | null>(null);
  const reconnectTimeoutRef = useRef<number | null>(null);

  // Manual values override handler
  useEffect(() => {
    if (manualTotal !== undefined && manualInfected !== undefined) {
      const survivors = Math.max(0, manualTotal - manualInfected);
      const ratio = manualTotal > 0 ? (survivors / manualTotal) * 100 : 0;
      const customData: PopulationStateData = {
        num_players: manualTotal,
        num_infected: manualInfected,
        num_humans: survivors,
        survived_pct: Number(ratio.toFixed(1)),
      };
      setData(customData);
      onUpdate?.(customData);
    }
  }, [manualTotal, manualInfected, onUpdate]);

  // HTTP Fetch function (used on mount & as fallback)
  const fetchHttpState = useCallback(async () => {
    if (manualTotal !== undefined && manualInfected !== undefined) return;

    try {
      const res = await fetch(`${apiBaseUrl}/population-state`);
      if (res.ok) {
        const json = await res.json();
        const total = manualTotal ?? json.num_players ?? 0;
        const infected = manualInfected ?? json.num_infected ?? 0;
        const survivors = json.num_humans ?? Math.max(0, total - infected);
        const pct = json.survived_pct ?? (total > 0 ? (survivors / total) * 100 : 0);

        const updated: PopulationStateData = {
          num_players: total,
          num_infected: infected,
          num_humans: survivors,
          survived_pct: Number(pct.toFixed(1)),
        };

        setData(updated);
        setIsConnected(true);
        setError(null);
        onUpdate?.(updated);
      }
    } catch (err: unknown) {
      setError(err instanceof Error ? err.message : 'Backend unreachable');
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
          setIsConnected(true);
          setIsWebSocketActive(true);
          setError(null);
        };

        socket.onmessage = (event) => {
          try {
            const rawData = JSON.parse(event.data);
            const total = manualTotal ?? rawData.num_players ?? 0;
            const infected = manualInfected ?? rawData.num_infected ?? 0;
            const survivors = rawData.num_humans ?? Math.max(0, total - infected);
            const pct = rawData.survived_pct ?? (total > 0 ? (survivors / total) * 100 : 0);

            const updated: PopulationStateData = {
              num_players: total,
              num_infected: infected,
              num_humans: survivors,
              survived_pct: Number(pct.toFixed(1)),
            };

            setData(updated);
            setIsConnected(true);
            onUpdate?.(updated);
          } catch (e) {
            console.error('Error parsing WebSocket message:', e);
          }
        };

        socket.onclose = () => {
          setIsWebSocketActive(false);
          if (!isUnmounted) {
            // Schedule reconnection attempt
            reconnectTimeoutRef.current = window.setTimeout(connectWebSocket, 2000);
          }
        };

        socket.onerror = () => {
          setIsWebSocketActive(false);
          setIsConnected(false);
        };
      } catch (e) {
        setIsWebSocketActive(false);
        if (!isUnmounted) {
          reconnectTimeoutRef.current = window.setTimeout(connectWebSocket, 2000);
        }
      }
    };

    // Initial HTTP fetch so data shows up instantly while WS connects
    fetchHttpState();
    connectWebSocket();

    return () => {
      isUnmounted = true;
      if (reconnectTimeoutRef.current) {
        clearTimeout(reconnectTimeoutRef.current);
      }
      if (socketRef.current) {
        socketRef.current.close();
      }
    };
  }, [useWebSocket, wsUrl, apiBaseUrl, manualTotal, manualInfected, fetchHttpState, onUpdate]);

  // Optional Fallback HTTP polling (only runs when WebSocket is NOT active)
  useEffect(() => {
    if (isWebSocketActive || effectiveFallbackInterval <= 0) return;

    fetchHttpState();
    const interval = setInterval(fetchHttpState, effectiveFallbackInterval);
    return () => clearInterval(interval);
  }, [isWebSocketActive, effectiveFallbackInterval, fetchHttpState]);

  // Derived percentages for progress bar rendering
  const total = data.num_players > 0 ? data.num_players : data.num_humans + data.num_infected;
  const survivorPct = total > 0 ? Math.min(100, Math.max(0, (data.num_humans / total) * 100)) : 0;
  const infectedPct = total > 0 ? Math.min(100, Math.max(0, (data.num_infected / total) * 100)) : 0;

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
          <span className="population-state-heading">GAME POPULATION STATE</span>
          {showLiveIndicator && (
            <span
              className={`live-pulse-dot ${isConnected ? 'online' : 'offline'}`}
              title={
                isWebSocketActive
                  ? 'Real-time WebSocket connected (0ms push)'
                  : isConnected
                  ? 'HTTP polling connected'
                  : `Offline: ${error ?? 'Attempting reconnection'}`
              }
            />
          )}
        </div>

        {/* Survived percentage badge */}
        <div className="population-state-badge">
          {data.survived_pct.toFixed(1)}% Survived
        </div>
      </div>

      {/* Progress Bar */}
      <div className="population-progress-track">
        <div
          className="population-progress-survivors"
          style={{ width: `${survivorPct}%` }}
          aria-label={`${survivorPct.toFixed(1)}% survivors`}
        />
        <div
          className="population-progress-zombies"
          style={{ width: `${infectedPct}%` }}
          aria-label={`${infectedPct.toFixed(1)}% zombies`}
        />
      </div>

      {/* Footer Stats Row */}
      <div className="population-state-footer">
        <div className="population-stat-survivors">
          <span className="stat-dot dot-survivors" aria-hidden="true" />
          <span>Survivors: {data.num_humans}</span>
        </div>

        <div className="population-stat-zombies">
          <span>Zombies: {data.num_infected}</span>
          <span className="stat-dot dot-zombies" aria-hidden="true" />
        </div>
      </div>
    </div>
  );
};

export default PopulationState;
