import { useState, useEffect } from 'react';
import { PopulationState } from './components/PopulationState';
import './App.css';

// Points at `wrangler dev` by default; set VITE_API_BASE to the deployed Worker URL.
const API_BASE = import.meta.env.VITE_API_BASE ?? 'http://localhost:8787';

function App() {
  const [isSimulating, setIsSimulating] = useState(false);
  const [actionStatus, setActionStatus] = useState<string>('Ready');

  // Interactive buttons to trigger changes on the Worker backend
  const handleInfect = async () => {
    try {
      setActionStatus('Infecting human...');
      const res = await fetch(`${API_BASE}/add-infected`, { method: 'POST' });
      if (res.ok) {
        setActionStatus('Infected human added!');
      } else {
        setActionStatus('Error adding infected');
      }
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  const handleAddPlayer = async () => {
    try {
      setActionStatus('Adding player...');
      const res = await fetch(`${API_BASE}/add-player`, { method: 'POST' });
      if (res.ok) {
        setActionStatus('Player added!');
      } else {
        setActionStatus('Error adding player');
      }
    } catch {
      setActionStatus('Backend unreachable');
    }
  };

  const handleReset = async () => {
    try {
      setActionStatus('Resetting population...');
      const res = await fetch(`${API_BASE}/reset-population`, { method: 'POST' });
      if (res.ok) {
        setActionStatus('Population reset to 50 players (16 zombies / 34 humans)!');
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

  return (
    <div className="app-container">
      <header className="app-header">
        <div className="header-badge">HTN 2026 GAME DASHBOARD</div>
        <h1>Game Population Monitor</h1>
        <p className="subtitle">
          Real-time survivor ratio tracking synced with the Workers backend
        </p>
      </header>

      {/* Main Showcase Component */}
      <section className="component-showcase">
        <PopulationState
          apiBaseUrl={API_BASE}
          pollIntervalMs={1000}
          showLiveIndicator={true}
        />
      </section>

      {/* Interactive Backend Trigger Controls */}
      <section className="controls-panel">
        <div className="controls-header">
          <h3>Live Controls</h3>
          <span className="status-pill">{actionStatus}</span>
        </div>

        <div className="button-group">
          <button
            type="button"
            className="btn btn-infect"
            onClick={handleInfect}
          >
            ☣ Infect Human (+1 Zombie)
          </button>

          <button
            type="button"
            className="btn btn-add-player"
            onClick={handleAddPlayer}
          >
            + Add Player
          </button>

          <button
            type="button"
            className="btn btn-reset"
            onClick={handleReset}
          >
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

      {/* Code Snippet / Integration Guide */}
      <section className="usage-guide">
        <h3>How to Use This Component</h3>
        <pre className="code-block">
{`import { PopulationState } from './components/PopulationState';

// 1. Live real-time streaming via WebSockets (0ms latency, zero HTTP poll spam):
<PopulationState apiBaseUrl="http://localhost:8787" useWebSocket={true} />

// 2. Controlled / Static props mode:
<PopulationState totalPlayers={50} infectedCount={16} />`}
        </pre>
      </section>
    </div>
  );
}

export default App;
