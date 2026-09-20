import React from 'react';
import './Roster.css';

export interface RosterProps {
  /** Total registered players. Names are drawn from the pool in order. */
  totalPlayers: number;
  /** How many of them have turned. The first `infectedCount` names are marked infected. */
  infectedCount: number;
  /** Hard cap on rendered entries across both columns. Defaults to 10. */
  maxEntries?: number;
  className?: string;
}

/**
 * Placeholder call sign pool. The backend tracks head-counts only — swap this
 * for real names as soon as /population-state returns a roster.
 */
const CALL_SIGNS = [
  'Anton', 'Bob', 'Charles', 'David', 'Eric',
  'Farah', 'Grace', 'Hideo', 'Imani', 'Jonas',
];

/** Past the end of the pool, call signs repeat with a batch suffix (Anton-2). */
const nameFor = (i: number) => {
  const batch = Math.floor(i / CALL_SIGNS.length);
  return CALL_SIGNS[i % CALL_SIGNS.length] + (batch > 0 ? `-${batch + 1}` : '');
};

interface ColumnProps {
  label: string;
  tone: 'stable' | 'infected';
  names: string[];
  /** Entries in this camp that the cap left off the board. */
  hidden: number;
  emptyText: string;
}

const RosterColumn: React.FC<ColumnProps> = ({ label, tone, names, hidden, emptyText }) => (
  <div className={`roster-column tone-${tone}`}>
    <div className="roster-column-head">
      <span className="roster-column-dot" aria-hidden="true" />
      <span className="roster-column-label">{label}</span>
      <span className="roster-column-tally">{names.length + hidden}</span>
    </div>

    {names.length === 0 ? (
      <p className="roster-empty">{emptyText}</p>
    ) : (
      <ul className="roster-list">
        {names.map((name, i) => (
          <li key={`${name}-${i}`} className="roster-entry">
            <span className="roster-index">{String(i + 1).padStart(2, '0')}</span>
            <span className="roster-name">{name}</span>
          </li>
        ))}
      </ul>
    )}

    {hidden > 0 && <p className="roster-more">+{hidden} more</p>}
  </div>
);

export const Roster: React.FC<RosterProps> = ({
  totalPlayers,
  infectedCount,
  maxEntries = 10,
  className = '',
}) => {
  const total = Math.max(0, totalPlayers);
  const lost = Math.min(total, Math.max(0, infectedCount));
  const alive = total - lost;

  // Never render more than `maxEntries` names. Past the cap the two camps keep
  // their proportions, so the board still reads the way the outbreak looks.
  const cap = Math.max(0, maxEntries);
  const shownInfected = total <= cap ? lost : Math.min(lost, Math.round((lost / total) * cap));
  const shownStable = total <= cap ? alive : Math.min(alive, cap - shownInfected);

  // The infected are counted off the front of the list, matching the backend
  const infected = Array.from({ length: shownInfected }, (_, i) => nameFor(i));
  const stable = Array.from({ length: shownStable }, (_, i) => nameFor(lost + i));

  return (
    <section className={`roster ${className}`} aria-label="Survivor roster">
      <header className="roster-header">
        <h1 className="roster-title">Survivors</h1>
        <span className="roster-count">
          {alive} <span className="roster-count-of">of</span> {total}
        </span>
      </header>

      <div className="roster-panel">
        {total === 0 ? (
          <p className="roster-empty roster-empty-all">
            No signal. No one is registered on this frequency.
          </p>
        ) : (
          <div className="roster-columns">
            <RosterColumn
              label="Stable"
              tone="stable"
              names={stable}
              hidden={alive - shownStable}
              emptyText="None left standing"
            />
            <RosterColumn
              label="Infected"
              tone="infected"
              names={infected}
              hidden={lost - shownInfected}
              emptyText="No one has turned"
            />
          </div>
        )}
      </div>
    </section>
  );
};

export default Roster;
