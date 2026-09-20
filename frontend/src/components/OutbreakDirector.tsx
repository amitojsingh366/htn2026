import { useEffect, useRef, useState } from 'react';
import type { DirectorAction, DirectorObservation, DirectorStatus } from './director-types';
import './OutbreakDirector.css';

const POLL_MS = 5000;
const TIMEOUT_MS = 8000;
const STALE_MS = 20000;

async function readStatus(response: Response): Promise<DirectorStatus> {
  const snapshot: DirectorStatus = await response.json();
  if (!snapshot || snapshot.version !== 1 || typeof snapshot.enabled !== 'boolean' ||
    typeof snapshot.operatorEnabled !== 'boolean' || typeof snapshot.serverEnabled !== 'boolean' ||
    typeof snapshot.configured !== 'boolean' || !snapshot.limits || !snapshot.dailyUsage ||
    !Array.isArray(snapshot.observations) || !Array.isArray(snapshot.actions) ||
    !Array.isArray(snapshot.runs) || !Array.isArray(snapshot.recaps) || !Array.isArray(snapshot.followUps)) {
    throw new Error('Director returned an unsupported status response.');
  }
  return snapshot;
}

function label(value: string) {
  return value.replaceAll('_', ' ');
}

function Timestamp({ at }: { at: number }) {
  return <time dateTime={new Date(at).toISOString()}>{new Date(at).toLocaleString()}</time>;
}

function JsonRecord({ value }: { value: Record<string, unknown> }) {
  return <pre className="director-json">{JSON.stringify(value, null, 2)}</pre>;
}

function actionOutcome(action: DirectorAction) {
  const status = typeof action.result.status === 'string' ? action.result.status : 'Result recorded';
  if (status !== 'queued') return label(status);
  const acknowledgments = Array.isArray(action.result.acknowledged) ? action.result.acknowledged.length : 0;
  return acknowledgments ? `Queued · ${acknowledgments} badge acknowledgments` : 'Queued · badge delivery unconfirmed';
}

function Observation({ value }: { value: DirectorObservation }) {
  return <>
    <div className="director-observation-counts">
      <span><strong>{value.humans}</strong> humans</span>
      <span><strong>{value.infected}</strong> zombies</span>
      <span><strong>{value.acceptedEvents}</strong> accepted tags</span>
    </div>
    <p className="director-meta">{label(value.phase)} · revision {value.revision} · <Timestamp at={value.observedAt} /></p>
    <p className="director-meta">{value.pendingEvents} pending · {value.rejectedEvents} rejected · host {value.hostConnected ? 'connected' : 'unavailable'} at observation</p>
    <p className="director-id">Round: {value.roundId ?? 'No active round'}</p>
  </>;
}

export function OutbreakDirector({ apiBaseUrl }: { apiBaseUrl: string }) {
  const [data, setData] = useState<DirectorStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [fetchedAt, setFetchedAt] = useState<number | null>(null);
  const [now, setNow] = useState(0);
  const [togglePending, setTogglePending] = useState(false);
  const [toggleError, setToggleError] = useState<string | null>(null);
  const requestVersion = useRef(0);
  const activePoll = useRef<AbortController | null>(null);
  const activeToggle = useRef<AbortController | null>(null);

  useEffect(() => {
    let disposed = false;
    let nextPoll: number | undefined;
    const clock = window.setInterval(() => setNow(Date.now()), 1000);

    const poll = async () => {
      if (activeToggle.current) {
        nextPoll = window.setTimeout(poll, POLL_MS);
        return;
      }
      const version = requestVersion.current;
      const controller = new AbortController();
      activePoll.current = controller;
      const timeout = window.setTimeout(() => controller.abort(), TIMEOUT_MS);
      try {
        const response = await fetch(`${apiBaseUrl}/director`, { signal: controller.signal, cache: 'no-store' });
        if (!response.ok) throw new Error(`Director returned HTTP ${response.status}.`);
        const snapshot = await readStatus(response);
        if (!disposed && version === requestVersion.current) {
          setData(snapshot);
          setError(null);
          setToggleError(null);
          setFetchedAt(Date.now());
          setNow(Date.now());
        }
      } catch (cause) {
        if (!disposed && version === requestVersion.current) setError(controller.signal.aborted ? 'Director request timed out.' : cause instanceof Error ? cause.message : 'Director unavailable.');
      } finally {
        window.clearTimeout(timeout);
        if (activePoll.current === controller) activePoll.current = null;
        // Serialize requests so an older response can never replace a newer one.
        if (!disposed) nextPoll = window.setTimeout(poll, POLL_MS);
      }
    };

    void poll();
    return () => {
      disposed = true;
      requestVersion.current += 1;
      activePoll.current?.abort();
      activeToggle.current?.abort();
      window.clearTimeout(nextPoll);
      window.clearInterval(clock);
    };
  }, [apiBaseUrl]);

  const stale = Boolean(error) || (fetchedAt !== null && now - fetchedAt > STALE_MS);
  const directorEnabled = Boolean(data?.enabled && data.configured);
  const toggleUnavailable = !data || stale || !data.configured || !data.serverEnabled;
  const toggleHelp = !data ? 'Waiting for the saved setting.' : stale ? 'Waiting for the server to reconnect.'
    : !data.configured ? 'Add the server API key to enable the director.'
      : !data.serverEnabled ? 'The director is disabled by server configuration.'
        : `${directorEnabled ? 'AI announcements, follow-ups, and recaps are enabled.' : 'AI announcements, follow-ups, and recaps are paused.'} This setting is saved for this game.`;

  const toggleDirector = async () => {
    if (toggleUnavailable || !data || activeToggle.current) return;
    const version = ++requestVersion.current;
    activePoll.current?.abort();
    const controller = new AbortController();
    activeToggle.current = controller;
    setTogglePending(true);
    setToggleError(null);
    const timeout = window.setTimeout(() => controller.abort(), TIMEOUT_MS);
    try {
      const response = await fetch(`${apiBaseUrl}/director/enabled`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: !data.operatorEnabled }),
        signal: controller.signal,
      });
      if (!response.ok) throw new Error(`Director returned HTTP ${response.status}.`);
      const snapshot = await readStatus(response);
      if (version === requestVersion.current) {
        // Only display the setting confirmed by the server, never an optimistic value.
        setData(snapshot);
        setError(null);
        setFetchedAt(Date.now());
        setNow(Date.now());
      }
    } catch {
      if (version === requestVersion.current) {
        setToggleError('Could not confirm the change. The last confirmed setting is shown; refreshing status.');
        setError('Checking the saved director setting.');
      }
    } finally {
      window.clearTimeout(timeout);
      if (activeToggle.current === controller) {
        activeToggle.current = null;
        setTogglePending(false);
      }
    }
  };

  const actions = data?.actions.toSorted((a, b) => b.at - a.at).slice(0, 8) ?? [];
  const runs = data?.runs.toSorted((a, b) => b.at - a.at).slice(0, 6) ?? [];
  const recaps = data?.recaps.toSorted((a, b) => b.at - a.at).slice(0, 3) ?? [];
  const observations = data?.observations
    .filter(value => value.roundId !== data.latest?.roundId || value.revision !== data.latest?.revision)
    .toSorted((a, b) => b.observedAt - a.observedAt).slice(0, 8) ?? [];

  return (
    <section className="director-panel" aria-labelledby="director-title">
      <header className="director-header">
        <div>
          <p className="director-eyebrow">Cloudflare Agents · OpenAI</p>
          <h2 id="director-title">Outbreak Director</h2>
        </div>
        <span className={`director-status director-status-${stale ? 'degraded' : data?.status ?? 'waiting'}`} role="status">
          {stale ? 'Feed unavailable' : data ? label(data.status) : 'Connecting'}
        </span>
      </header>

      <div className="director-control">
        <div>
          <p className="director-control-label">Director automation</p>
          <p id="director-toggle-help" className="director-meta">{toggleHelp}</p>
        </div>
        <button
          type="button"
          className={`director-toggle ${directorEnabled ? 'is-enabled' : ''}`}
          role="switch"
          aria-label="Outbreak Director automation"
          aria-checked={directorEnabled}
          aria-describedby="director-toggle-help"
          aria-busy={togglePending}
          disabled={togglePending || toggleUnavailable}
          onClick={() => void toggleDirector()}
        >
          <span className="director-toggle-track" aria-hidden="true"><span /></span>
          {togglePending ? 'Saving…' : directorEnabled ? 'Enabled' : 'Disabled'}
        </button>
      </div>
      {toggleError && <p className="director-toggle-error" role="alert">{toggleError}</p>}

      <p className="director-reason">{data?.reason ?? 'Reading the director’s saved state…'}</p>
      <p className={`director-feed ${stale ? 'director-warning' : ''}`}>
        {error && `${error} `}
        {stale ? 'Saved values may be stale. Retrying automatically.' : fetchedAt ? <>State fetched <Timestamp at={fetchedAt} />.</> : 'The agent feed loads independently of the game.'}
      </p>

      {data && <>
        <div className="director-config">
          <span>Model <code>{data.model}</code></span>
          <span>{directorEnabled ? 'Director enabled' : 'Director disabled'} · {data.configured ? 'Server API key configured' : 'Server API key not configured'}</span>
        </div>
        <dl className="director-budgets">
          <div><dt>Round requests</dt><dd>{data.roundRequests} <span>/ {data.limits.roundRequests}</span></dd></div>
          <div><dt>Daily requests</dt><dd>{data.dailyUsage.requests} <span>/ {data.limits.dailyRequests}</span></dd></div>
          <div><dt>Daily input tokens</dt><dd>{data.dailyUsage.inputTokens.toLocaleString()}</dd></div>
          <div><dt>Daily output tokens</dt><dd>{data.dailyUsage.outputTokens.toLocaleString()}</dd></div>
        </dl>
        <p className="director-meta">Usage day: {data.dailyUsage.day} UTC · max {data.limits.outputTokens} output tokens/request · {data.limits.cooldownMs / 1000}s minimum between runs</p>

        <div className="director-section">
          <h3>Cloudflare memory</h3>
          <p className="director-meta">Saved observations, actions and recaps persist on the server.</p>
          {data.latest ? <div className="director-observation"><Observation value={data.latest} /></div> : <p className="director-empty">No game observation saved yet.</p>}
          <details className="director-details">
            <summary>Previous observations · {observations.length} shown</summary>
            {observations.length ? <ol className="director-list">{observations.map(value => <li key={`${value.roundId}:${value.revision}:${value.observedAt}`}><Observation value={value} /></li>)}</ol> : <p className="director-empty">No earlier observations in retained memory.</p>}
          </details>
          {Boolean(data.latest?.history.length) && <details className="director-details">
            <summary>Recent infection evidence · latest {Math.min(10, data.latest!.history.length)}</summary>
            <ul className="director-list">{data.latest!.history.slice(-10).reverse().map(event => <li key={event.id}>
              <p>Slot {event.actor} → slot {event.victim} · {Math.round(event.elapsedMs / 1000)}s into round · {label(event.status)}</p>
              <p className="director-id">{event.id}</p>
            </li>)}</ul>
          </details>}
        </div>

        <div className="director-section">
          <h3>Recent actions</h3>
          <p className="director-meta">Queued announcements await badge confirmation. Receipts show badge acknowledgments when reported.</p>
          {actions.length ? <ol className="director-list">{actions.map(action => <li key={action.id}>
            <div className="director-row"><strong>{label(action.tool)}</strong><span className="director-outcome">{actionOutcome(action)}</span></div>
            {typeof action.arguments.text === 'string' && <p className="director-announcement">“{action.arguments.text}”</p>}
            {typeof action.result.reason === 'string' && <p className="director-meta">{action.result.reason}</p>}
            <p className="director-meta"><Timestamp at={action.at} /></p>
            <details className="director-details">
              <summary>Tool arguments and result</summary>
              <p className="director-id">Action: {action.id} · round: {action.roundId}</p>
              <h4>Arguments</h4><JsonRecord value={action.arguments} />
              <h4>Result</h4><JsonRecord value={action.result} />
            </details>
          </li>)}</ol> : <p className="director-empty">No tool actions recorded.</p>}
        </div>

        <div className="director-section">
          <h3>Scheduled follow-ups</h3>
          {data.followUps.length ? <ul className="director-list">{data.followUps.toSorted((a, b) => a.dueAt - b.dueAt).map(followUp => <li key={followUp.id}>
            <p>{followUp.reason}</p>
            <p className="director-meta">{followUp.dueAt <= now ? 'Due · awaiting execution' : 'Scheduled for'} <Timestamp at={followUp.dueAt} /></p>
            <p className="director-id">Round: {followUp.roundId}</p>
          </li>)}</ul> : <p className="director-empty">No follow-ups pending.</p>}
        </div>

        <div className="director-section">
          <h3>Post-game recaps</h3>
          {recaps.length ? recaps.map(recap => <article className="director-recap" key={`${recap.roundId}:${recap.at}`}>
            <div className="director-row"><h4>{recap.title}</h4><span className="director-outcome">{recap.final ? 'Final evidence' : 'Provisional evidence'}</span></div>
            <p>{recap.text}</p>
            <p className="director-meta"><Timestamp at={recap.at} /></p>
            <details className="director-details">
              <summary>Grounding facts and evidence</summary>
              <p className="director-id">Round: {recap.roundId}</p>
              <JsonRecord value={recap.facts} />
              <h4>Infection evidence IDs</h4>
              {recap.evidenceIds.length ? <ul className="director-evidence">{recap.evidenceIds.map(id => <li key={id}>{id}</li>)}</ul> : <p className="director-meta">No infection event IDs cited; see round facts above.</p>}
            </details>
          </article>) : <p className="director-empty">Recaps appear after a round ends and the director publishes one.</p>}
        </div>

        <details className="director-section director-details director-runs">
          <summary>OpenAI API activity · {runs.length} recent runs</summary>
          <p className="director-meta">Response IDs and token counts are recorded from API replies. A scheduled or failed run alone does not establish a successful model response.</p>
          {runs.length ? <ol className="director-list">{runs.map(run => <li key={run.id}>
            <div className="director-row"><strong>{label(run.trigger)}</strong><span className="director-outcome">{label(run.status)}</span></div>
            <p>{run.summary || 'No response summary recorded.'}</p>
            <p className="director-meta"><Timestamp at={run.at} /> · {run.model}</p>
            <p className="director-meta">{run.inputTokens.toLocaleString()} input tokens · {run.outputTokens.toLocaleString()} output tokens</p>
            <p className="director-id">Run: {run.id} · round: {run.roundId}</p>
            <p className="director-id">Response IDs: {run.responseIds.join(', ') || 'No OpenAI response recorded'}</p>
            <p className="director-id">Request IDs: {run.requestIds.join(', ') || 'None recorded'}</p>
          </li>)}</ol> : <p className="director-empty">No OpenAI API runs recorded yet.</p>}
        </details>
      </>}
    </section>
  );
}
