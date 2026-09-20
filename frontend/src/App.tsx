import { useState, useEffect, useCallback } from 'react';
import { PopulationState } from './components/PopulationState';
import type { PopulationStateData } from './components/PopulationState';
import { Roster } from './components/Roster';
import './App.css';

const API_BASE = 'http://localhost:8000';

function App() {
  const [isSimulating, setIsSimulating] = useState(false);
  const [actionStatus, setActionStatus] = useState<string>('Ready');
  const [population, setPopulation] = useState<PopulationStateData>({
    num_players: 0,
    num_infected: 0,
    num_humans: 0,
    survived_pct: 0,
  });

  // Roster is rendered from whatever the live population feed last reported
  const handlePopulationUpdate = useCallback((data: PopulationStateData) => {
    setPopulation(data);
  }, []);

  // Interactive buttons to trigger changes on the FastAPI backend
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
      if (res.ok) setActionStatus('Population cleared.');
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

  const outbreakCritical = population.num_players > 0 && population.survived_pct < 40;

  return (
    <>
      <div className="atmosphere" aria-hidden="true" />
      <div className="interference" aria-hidden="true" />

      <div className={`app-container ${outbreakCritical ? 'is-critical' : ''}`}>
        <header className="app-header">
          <div className="header-badge">HTN 2026 · Outbreak Monitor</div>
        </header>

        <Roster
          totalPlayers={population.num_players}
          infectedCount={population.num_infected}
        />

        <section className="component-showcase">
          <PopulationState
            apiBaseUrl={API_BASE}
            pollIntervalMs={1000}
            showLiveIndicator={true}
            onUpdate={handlePopulationUpdate}
          />
        </section>

        {/* Interactive Backend Trigger Controls */}
        <section className="controls-panel">
          <div className="controls-header">
            <h3>Field Controls</h3>
            <span className="status-pill" role="status">{actionStatus}</span>
          </div>

          <div className="button-group">
            <button type="button" className="btn btn-infect" onClick={handleInfect}>
              ☣ Infect Survivor
            </button>

            <button type="button" className="btn btn-add-player" onClick={handleAddPlayer}>
              + Add Player
            </button>

            <button type="button" className="btn btn-reset" onClick={handleReset}>
              ↺ Reset Population
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

        {/* Integration notes — folded away so the screen stays a game screen */}
        <details className="usage-guide">
          <summary>Integration notes</summary>
          <pre className="code-block">
{`import { PopulationState } from './components/PopulationState';

// 1. Live real-time streaming via WebSockets (0ms latency, zero HTTP poll spam):
<PopulationState apiBaseUrl="http://localhost:8000" useWebSocket={true} />

// 2. Controlled / Static props mode:
<PopulationState totalPlayers={50} infectedCount={16} />`}
          </pre>
        </details>
      </div>
    </>
  );
}

export default App;
