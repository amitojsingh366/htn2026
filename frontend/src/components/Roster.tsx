import React from 'react';
import './Roster.css';

export interface RosterProps {
  players: { id: string; slot: number; name: string; role?: 'H' | 'Z' }[];
  rolesAssigned: boolean;
  countingDown?: boolean;
  loading?: boolean;
  maxEntries?: number;
  className?: string;
}

interface ColumnProps {
  label: string;
  tone: 'stable' | 'infected' | 'registered';
  players: RosterProps['players'];
  hidden: number;
  emptyText: string;
}

const RosterColumn: React.FC<ColumnProps> = ({ label, tone, players, hidden, emptyText }) => (
  <div className={`roster-column tone-${tone}`}>
    <div className="roster-column-head">
      <span className="roster-column-dot" aria-hidden="true" />
      <span className="roster-column-label">{label}</span>
      <span className="roster-column-tally">{players.length + hidden}</span>
    </div>

    {players.length === 0 ? (
      <p className="roster-empty">{emptyText}</p>
    ) : (
      <ul className="roster-list">
        {players.map(player => (
          <li key={player.id} className="roster-entry">
            <span className="roster-index">{String(player.slot).padStart(2, '0')}</span>
            <span className="roster-name">{player.name || player.id}</span>
          </li>
        ))}
      </ul>
    )}

    {hidden > 0 && <p className="roster-more">+{hidden} more</p>}
  </div>
);

export const Roster: React.FC<RosterProps> = ({
  players,
  rolesAssigned,
  countingDown = false,
  loading = false,
  maxEntries = 20,
  className = '',
}) => {
  const total = players.length;
  const stable = players.filter(player => player.role === 'H');
  const infected = players.filter(player => player.role === 'Z');
  const unknown = total - stable.length - infected.length;
  const cap = Math.max(0, Math.floor(maxEntries));
  const shownStable = stable.slice(0, cap);
  const shownInfected = infected.slice(0, Math.max(0, cap - shownStable.length));

  return (
    <section className={`roster ${className}`} aria-label="Saved badge roster">
      <header className="roster-header">
        <h1 className="roster-title">
          {loading ? '—' : rolesAssigned ? stable.length : total} {rolesAssigned ? 'Survivors' : 'Registered'}
        </h1>
        <span className="roster-count">
          <span className="roster-count-of">of</span> {loading ? '—' : rolesAssigned ? total : 20}
        </span>
      </header>

      <div className="roster-panel">
        <p className="roster-note">{rolesAssigned ? `${countingDown ? 'Roles assigned · gameplay begins at zero' : 'Saved server roles · online badge count unknown'}${unknown ? ` · ${unknown} roles unknown` : ''}` : 'Saved registrations · roles unassigned · online badge count unknown'}</p>
        {loading || total === 0 ? (
          <p className="roster-empty roster-empty-all">
            {loading ? 'Loading saved registrations…' : 'No saved registrations. Press A on each badge to join.'}
          </p>
        ) : !rolesAssigned ? (
          <RosterColumn label="Registered badges" tone="registered" players={players.slice(0, cap)} hidden={Math.max(0, total - cap)} emptyText="No saved registrations" />
        ) : (
          <div className="roster-columns">
            <RosterColumn
              label="Human"
              tone="stable"
              players={shownStable}
              hidden={stable.length - shownStable.length}
              emptyText="No human roles"
            />
            <RosterColumn
              label="Infected"
              tone="infected"
              players={shownInfected}
              hidden={infected.length - shownInfected.length}
              emptyText="No infected roles"
            />
          </div>
        )}
      </div>
    </section>
  );
};

export default Roster;
