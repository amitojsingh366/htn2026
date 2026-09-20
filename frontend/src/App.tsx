import { useState, useEffect, useCallback, useRef } from 'react';
import { PopulationState, type PopulationStateData } from './components/PopulationState';
import { Roster } from './components/Roster';
import { OutbreakDirector } from './components/OutbreakDirector';
import './App.css';
import { API_BASE, GAME_ID, gameRequest } from './telemetry';

function App() {
  const [actionStatus, setActionStatus] = useState('Press A on every badge to join.');
  const [state, setState] = useState<PopulationStateData | null>(null);
  const [feedConnected, setFeedConnected] = useState(false);
  const [busy, setBusy] = useState(false);
  const [resetBusy, setResetBusy] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const serverClock = useRef<{ time: number; observedAt: number } | null>(null);
  const handleStateUpdate = useCallback((data: PopulationStateData, estimatedServerTime?: number) => {
    const observedAt = performance.now();
    const timestamp = estimatedServerTime ?? data.server_time_ms;
    if (typeof timestamp === 'number' && Number.isFinite(timestamp)) {
      serverClock.current = { time: timestamp, observedAt };
    }
    const currentTime = serverClock.current
      ? serverClock.current.time + observedAt - serverClock.current.observedAt
      : Date.now();
    setNow(currentTime);
    setState(previous => ({
      ...data,
      // Older terminal responses may omit a cutoff. Freeze at first observation
      // until the authoritative ended_at arrives instead of running forever.
      ended_at: data.ended_at ?? (data.game_over
        ? previous?.game_over && previous.started_at === data.started_at
          ? previous.ended_at ?? currentTime
          : currentTime
        : null),
    }));
  }, []);
  useEffect(() => {
    // Keep the shared deadline stable even if the browser's wall clock changes.
    const timer = setInterval(() => setNow(serverClock.current
      ? serverClock.current.time + performance.now() - serverClock.current.observedAt
      : Date.now()), 100);
    return () => clearInterval(timer);
  }, []);

  const startGame = async () => {
    setBusy(true);
    try {
      const { response, data: result } = await gameRequest<{ message?: string; error?: string }>('round.start', `${API_BASE}/start-game`, { method: 'POST' });
      setActionStatus(response.ok ? result.message ?? 'Preparing the round.' : result.error ?? 'Unable to prepare the round.');
    } catch { setActionStatus('Backend unreachable.'); }
    finally { setBusy(false); }
  };

  const resetGame = async () => {
    if (!window.confirm('Reset the server game and clear all saved registrations now? This completes even when badges are offline. Uploaded evidence stays archived; badges that receive reset will clear their pending local tags.')) return;
    setResetBusy(true);
    try {
      const { response, data: result } = await gameRequest<PopulationStateData & { error?: string }>('round.reset', `${API_BASE}/reset-game`, { method: 'POST' });
      if (!response.ok) throw new Error(result.error ?? 'Unable to reset the game.');
      handleStateUpdate(result);
      setActionStatus('Game reset. Server state and saved registrations cleared. Register badges again to start a new game.');
    } catch (error) {
      setActionStatus(error instanceof Error ? error.message : 'Backend unreachable.');
    } finally { setResetBusy(false); }
  };

  const scheduled = state?.started_at ?? null;
  const finished = Boolean(state?.game_over);
  const winner = state?.winner === 'Z' ? 'ZOMBIES WIN' : state?.winner === 'H' ? 'HUMANS WIN' : 'ROUND ENDED';
  const resetting = state?.gateway_phase === 'resetting';
  const prepared = state?.gateway_phase === 'prepared';
  const registered = state?.registered_players ?? 0;
  const startApplied = state?.start_applied_players ?? 0;
  const countdown = scheduled && !finished ? Math.max(0, Math.ceil((scheduled - now) / 1000)) : 0;
  const countingDown = countdown > 0 && !resetting;
  const elapsed = scheduled ? Math.max(0, Math.floor(((state?.ended_at ?? now) - scheduled) / 1000)) : 0;
  const time = `${Math.floor(elapsed / 60).toString().padStart(2, '0')}:${(elapsed % 60).toString().padStart(2, '0')}`;
  const hostAge = state?.host_last_seen_at ? Math.max(0, Math.floor((now - state.host_last_seen_at) / 1000)) : null;
  const hostAvailable = feedConnected && Boolean(state?.host_connected) && hostAge !== null && hostAge <= 45;
  const incompleteStart = Boolean(scheduled) && startApplied < registered;
  const roundStatus = !state ? 'LOADING STATE' : resetting ? 'RESET REQUESTED' : finished ? winner : countdown ? `STARTING IN ${countdown}s` : scheduled ? incompleteStart ? 'START DELIVERY INCOMPLETE' : 'ROUND STARTED' : prepared ? 'PREPARING BADGES' : 'REGISTRATION OPEN';
  const outbreakCritical = !finished && Boolean(scheduled) && !countingDown && (state?.num_players ?? 0) > 0 && (state?.survived_pct ?? 100) < 40;

  return (
    <>
      <div className="atmosphere" aria-hidden="true" />
      <div className="interference" aria-hidden="true" />

      <div className={`app-container ${outbreakCritical ? 'is-critical' : ''}`}>
        <header className="app-header">
          <div className="header-badge">HTN 2026 · Outbreak Monitor</div>
          <p className="connection-caption">
            Game {GAME_ID} · {!feedConnected ? 'Dashboard feed unavailable' : hostAvailable ? `Host gateway active · heard ${hostAge}s ago` : 'No active host gateway link'}
          </p>
        </header>

        <section className="round-status-banner">
          <div className="round-stat-item">
            <span className="round-stat-label">ROUND STATUS</span>
            <span className={`round-stat-value ${finished ? state?.winner === 'Z' ? 'status-zombies' : 'status-humans' : countingDown ? 'status-countdown' : scheduled && !incompleteStart && !resetting ? 'status-active' : 'status-waiting'}`}>{roundStatus}</span>
          </div>
          <div className="round-stat-item">
            <span className="round-stat-label">{finished ? 'FINAL TIME' : 'ROUND TIMER'}</span>
            <span className="round-timer-value">{time}</span>
          </div>
          <div className="round-stat-item">
            <span className="round-stat-label">PATIENT ZERO</span>
            <span className={`patient-zero-tag sentry-block ${state?.patient_zero_id ? 'active' : ''}`}>
              {state?.players?.find(player => player.id === state.patient_zero_id)?.name ?? state?.patient_zero_id ?? 'Unassigned'}
            </span>
          </div>
        </section>

        {countingDown && (
          <section className="round-countdown" role="status" aria-live="polite" aria-atomic="true">
            <p className="countdown-label">ALL BADGES READY · ROUND STARTS IN</p>
            <p className="countdown-value">{countdown}<span>seconds</span></p>
            <p className="countdown-description">Check your role below. Controls unlock together when the countdown ends.</p>
          </section>
        )}

        {finished && (
          <section className={`game-result ${state?.winner === 'Z' ? 'result-zombies' : 'result-humans'}`} role="status" aria-live="polite">
            <p className="game-result-label">GAME OVER</p>
            <h2>{winner}</h2>
            <p>{state?.result_final ? 'Result confirmed.' : 'Play has stopped. Syncing the remaining badge records.'}</p>
          </section>
        )}

        <div className="sentry-block"><Roster players={state?.players ?? []} rolesAssigned={Boolean(scheduled)} countingDown={countingDown} loading={!state} /></div>

        <section className="component-showcase">
          <PopulationState apiBaseUrl={API_BASE} pollIntervalMs={2000} showLiveIndicator onUpdate={handleStateUpdate} onConnectionChange={setFeedConnected} />
        </section>

        <section className="controls-panel">
          <div className="controls-header">
            <h3>Field Controls</h3>
            <span className="status-pill" role="status">{actionStatus}</span>
          </div>
          <div className="control-readouts">
            <p>{registered}/20 saved registrations · {state?.frozen_roster_players ?? 0}/20 frozen roster · {state?.ready_players ?? 0} prepared · {startApplied} applied Start</p>
            {scheduled && <p>{state?.events_received ?? 0} infection events received · {state?.events_pending ?? 0} waiting for causal evidence · {state?.events_rejected ?? 0} rejected</p>}
          </div>
          <div className="button-group live-controls">
            <button type="button" className="btn btn-start-game" disabled={busy || resetBusy || resetting || !hostAvailable || registered < 2 || prepared || Boolean(scheduled)} onClick={startGame}>
              {busy ? 'Preparing…' : 'Start Game'}
            </button>
            <button type="button" className="btn btn-reset" disabled={busy || resetBusy || countingDown || !feedConnected} onClick={resetGame}>
              {resetBusy ? 'Resetting…' : 'Reset Game'}
            </button>
          </div>
          <p className="guide-desc">Start freezes the roster and waits for every badge to prepare. Once all are ready, roles appear and a shared countdown begins. Gameplay controls stay locked until zero.</p>
          <p className="guide-desc">
            {finished ? state?.result_final ? 'Round finished. Reset Game to register for a new round.' : 'Play has stopped. Keep badges connected while the last tag records sync.' : prepared ? `Waiting for ${Math.max(0, registered - (state?.ready_players ?? 0))} badge preparation acknowledgments. Keep all badges in radio range.` : countingDown ? `All badges prepared. Starting in ${countdown}s · ${startApplied}/${registered} badges have acknowledged applying Start.` : scheduled ? `${startApplied}/${registered} badges have acknowledged applying Start. The scheduled time alone does not confirm delivery.` : 'Before Start, the frozen roster is 0/20 even after registration. After Start, a badge still at 0/20 has not admitted the round snapshot.'}
          </p>
          <p className="guide-desc">Reset clears the server immediately, even when badges are offline or have forgotten the game. An offline badge may keep its old game until it receives a reset.</p>
          {resetting && <p className="guide-desc" role="status">An earlier reset is still waiting. Press Reset Game to clear the server immediately.</p>}
        </section>

        <OutbreakDirector apiBaseUrl={API_BASE} />

        <details className="usage-guide saved-records sentry-block">
          <summary>Registered badges · saved acknowledgments</summary>
          <p className="guide-desc">These records remain after badges power off. They do not count online badges.</p>
          <div className="table-responsive">
            <table className="leaderboard-table">
              <thead><tr><th>Slot</th><th>Name</th><th>Badge</th><th>Server role</th><th>Saved acknowledgment</th></tr></thead>
              <tbody>{state?.players?.map(player => (
                <tr key={player.id}>
                  <td>{player.slot}</td><td>{player.name}</td><td className="device-id-cell">{player.id}</td>
                  <td>{scheduled ? player.role === 'Z' ? player.id === state.patient_zero_id ? 'Zombie · Patient Zero' : 'Zombie' : player.role === 'H' ? 'Human' : 'Unknown' : 'Unassigned'}</td>
                  <td>{state.start_applied_slots?.includes(player.slot) ? 'Start applied' : state.ready_slots?.includes(player.slot) ? 'Prepared' : prepared || scheduled ? 'Waiting for preparation' : 'Registration saved'}</td>
                </tr>
              ))}</tbody>
            </table>
          </div>
        </details>

        <details className="usage-guide">
          <summary>Join the live round</summary>
          <p className="guide-desc">Boot ami with AUX1 on and wait for Wi-Fi. Press A on every badge to register. With at least two players registered, press B on the host or Start Game here. Keep badges within mesh range until all are ready, check your role during the countdown, and wait for zero before playing.</p>
          <p className="guide-desc">Zombies press A to infect the nearby human selected on their screen. After a winner is shown, the host can press B to reset. Register the badges again with A, then press B on the host to start the next round.</p>
          <p className="guide-desc">Local tags synchronize through the host. Zombies win when the server confirms every player is infected; humans win when the time limit expires with survivors. Badges catch up on missed state and tag records when they return to mesh range during the same boot. Reset Game clears the server round and registrations immediately.</p>
        </details>
      </div>
    </>
  );
}

export default App;
