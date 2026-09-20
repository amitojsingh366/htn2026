import { useState, useEffect, useCallback } from 'react';
import { PopulationState } from './components/PopulationState';
import type { PopulationStateData } from './components/PopulationState';
import { Roster } from './components/Roster';
import './App.css';

// Points at `wrangler dev` by default; set VITE_API_BASE to the deployed Worker URL.
const API_BASE = import.meta.env.VITE_API_BASE ?? 'http://localhost:8787';

function App() {
  const [isSimulating, setIsSimulating] = useState(false);
  const [actionStatus, setActionStatus] = useState<string>('Ready');
  const [startedAt, setStartedAt] = useState<number | null>(null);
  const [patientZeroId, setPatientZeroId] = useState<string | null>(null);
  const [isGameOver, setIsGameOver] = useState<boolean>(false);
  const [elapsedSeconds, setElapsedSeconds] = useState<number>(0);
  const [rankings, setRankings] = useState<PopulationStateData['rankings']>([]);
  const [population, setPopulation] = useState<PopulationStateData>({
    num_players: 0,
    num_infected: 0,
    num_humans: 0,
    survived_pct: 0,
  });

  // Roster and round banner both render from whatever the live feed last reported
  const handleStateUpdate = useCallback((data: PopulationStateData) => {
    setPopulation(data);
    if (data.started_at !== undefined) {
      setStartedAt(data.started_at);
    }
    if (data.patient_zero_id !== undefined) {
      setPatientZeroId(data.patient_zero_id);
    }
    if (data.game_over !== undefined) {
      setIsGameOver(Boolean(data.game_over));
    }
    if (data.rankings) {
      setRankings(data.rankings);
    }
  }, []);

  // Timer tick effect
  useEffect(() => {
    if (!startedAt || isGameOver) return;

    const updateTimer = () => {
      const seconds = Math.max(0, Math.floor((Date.now() - startedAt) / 1000));
      setElapsedSeconds(seconds);
    };

    updateTimer();
    const interval = setInterval(updateTimer, 1000);
    return () => clearInterval(interval);
  }, [startedAt, isGameOver]);

  // Start Game Button Handler
  const handleStartGame = async () => {
    try {
      setActionStatus('Starting game & choosing Patient Zero...');
      const res = await fetch(`${API_BASE}/start-game`, { method: 'POST' });
      if (res.ok) {
        const data = await res.json();
        setStartedAt(data.started_at);
        setPatientZeroId(data.patient_zero_id);
        setIsGameOver(false);
        setElapsedSeconds(0);
        if (data.patient_zero_id) {
          setActionStatus(`Game started! ${data.patient_zero_id} infected as Patient Zero.`);
        } else {
          setActionStatus('Game started! Awaiting player devices...');
        }
      } else {
        setActionStatus('Error starting game');
      }
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  // Interactive buttons to trigger changes on the Worker backend
  const handleInfect = async () => {
    try {
      setActionStatus('Infecting a survivor...');
      const res = await fetch(`${API_BASE}/add-infected`, { method: 'POST' });
      setActionStatus(res.ok ? 'A survivor has turned.' : 'Error adding infected');
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  const handleAddPlayer = async () => {
    try {
      setActionStatus('Adding player...');
      const res = await fetch(`${API_BASE}/add-player`, { method: 'POST' });
      setActionStatus(res.ok ? 'Player added.' : 'Error adding player');
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  const handleReset = async () => {
    try {
      setActionStatus('Resetting population...');
      const res = await fetch(`${API_BASE}/reset-population`, { method: 'POST' });
      if (res.ok) {
        setStartedAt(null);
        setPatientZeroId(null);
        setIsGameOver(false);
        setElapsedSeconds(0);
        setRankings([]);
        setActionStatus('Population and game round cleared.');
      } else {
        setActionStatus('Error resetting population');
      }
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  // Auto-simulation interval
  useEffect(() => {
    if (!isSimulating) return;
    const interval = setInterval(async () => {
      try {
        await fetch(`${API_BASE}/add-infected`, { method: 'POST' });
      } catch (e) {
        console.error(e);
      }
    }, 1500);

    return () => clearInterval(interval);
  }, [isSimulating]);

  // Format seconds to mm:ss
  const formatTime = (totalSec: number) => {
    const mins = Math.floor(totalSec / 60);
    const secs = totalSec % 60;
    return `${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
  };

  const outbreakCritical = population.num_players > 0 && population.survived_pct < 40;

  return (
    <>
      <div className="atmosphere" aria-hidden="true" />
      <div className="interference" aria-hidden="true" />

      <div className={`app-container ${outbreakCritical ? 'is-critical' : ''}`}>
        <header className="app-header">
          <div className="header-badge">HTN 2026 · Outbreak Monitor</div>
        </header>

        {/* Round & Timer Banner */}
        <section className="round-status-banner">
          <div className="round-stat-item">
            <span className="round-stat-label">ROUND STATUS</span>
            <span className={`round-stat-value ${isGameOver ? 'status-over' : startedAt ? 'status-active' : 'status-waiting'}`}>
              {isGameOver ? '💀 GAME OVER' : startedAt ? '⚡ ACTIVE OUTBREAK' : '⏳ WAITING TO START'}
            </span>
          </div>

          <div className="round-stat-item">
            <span className="round-stat-label">SURVIVAL TIMER</span>
            <span className="round-timer-value">{formatTime(elapsedSeconds)}</span>
          </div>

          <div className="round-stat-item">
            <span className="round-stat-label">PATIENT ZERO</span>
            <span className={`patient-zero-tag ${patientZeroId ? 'active' : ''}`}>
              {patientZeroId ? `☣ ${patientZeroId}` : 'Not Assigned'}
            </span>
          </div>
        </section>

        <Roster
          totalPlayers={population.num_players}
          infectedCount={population.num_infected}
        />

        <section className="component-showcase">
          <PopulationState
            apiBaseUrl={API_BASE}
            pollIntervalMs={1000}
            showLiveIndicator={true}
            onUpdate={handleStateUpdate}
          />
        </section>

        {/* Interactive Backend Trigger Controls */}
        <section className="controls-panel">
          <div className="controls-header">
            <h3>Field Controls</h3>
            <span className="status-pill" role="status">{actionStatus}</span>
          </div>

          <div className="button-group">
            <button type="button" className="btn btn-start-game" onClick={handleStartGame}>
              🚀 Start Game
            </button>

            <button type="button" className="btn btn-infect" onClick={handleInfect}>
              ☣ Infect Survivor
            </button>

            <button type="button" className="btn btn-add-player" onClick={handleAddPlayer}>
              + Add Player
            </button>

            <button type="button" className="btn btn-reset" onClick={handleReset}>
              ↺ Reset Round
            </button>

            <button
              type="button"
              className={`btn btn-simulate ${isSimulating ? 'active' : ''}`}
              onClick={() => setIsSimulating(!isSimulating)}
            >
              {isSimulating ? '⏸ Pause Outbreak' : '⚡ Simulate Outbreak'}
            </button>
          </div>
        </section>

        {/* Final Game Over Leaderboard */}
        {isGameOver && rankings && rankings.length > 0 && (
          <section className="leaderboard-panel">
            <div className="leaderboard-header">
              <h3>🏆 Final Survival Rankings</h3>
            </div>
            <div className="table-responsive">
              <table className="leaderboard-table">
                <thead>
                  <tr>
                    <th>Rank</th>
                    <th>Device ID</th>
                    <th>Survival Time</th>
                    <th>Final Role</th>
                  </tr>
                </thead>
                <tbody>
                  {rankings.map((r) => (
                    <tr key={r.device_id} className={r.rank === 1 ? 'rank-winner' : ''}>
                      <td>
                        <span className="rank-badge">
                          {r.rank === 1 ? '🥇 #1' : r.rank === 2 ? '🥈 #2' : r.rank === 3 ? '🥉 #3' : `#${r.rank}`}
                        </span>
                      </td>
                      <td className="device-id-cell">{r.device_id}</td>
                      <td>{r.survival_time_seconds}s</td>
                      <td>
                        <span className={`role-badge ${r.state === 'infected' ? 'role-infected' : 'role-human'}`}>
                          {r.state}
                        </span>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </section>
        )}

        {/* Integration notes — folded away so the screen stays a game screen */}
        <details className="usage-guide">
          <summary>ESP device integration</summary>
          <p className="guide-desc">
            Devices query their assigned role at <code>GET /device-state?device_id=&lt;id&gt;</code>{' '}
            or connect to WebSocket <code>/ws/population</code>.
          </p>
          <pre className="code-block">
{`// 1. Start Game:
POST /start-game -> Picks 1 random ESP to infect & starts timer

// 2. ESP Checks Role:
GET /device-state?device_id=esp-01
-> { "device_id": "esp-01", "role": "infected", "game_started": true }

// 3. ESP Telemetry Event:
POST /device-event
-> { "device_id": "esp-01", "timestamp": 12000, "current_state": "infected" }`}
          </pre>
        </details>
      </div>
    </>
  );
}

export default App;
