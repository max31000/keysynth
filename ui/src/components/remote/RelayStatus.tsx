// Relay connection indicator (host, round trip) and host switcher (docs/specs/2026-10-12-remote.md §6, §10.14).

import { connectEngine, selectHost, useEngine, useRemote } from '../../store';

/** LED + host name + RTT; a host switcher when more than one host is online. */
export function RelayPill() {
  const relay = useRemote((s) => s.relay);
  const status = useEngine((s) => s.connection.status);
  const host = relay.hosts.find((h) => h.deviceId === relay.hostId);
  const switcher = relay.hosts.filter((h) => h.online).length > 1;
  const state = relay.status !== 'open' ? 'reconnecting' : status === 'open' ? 'open' : 'connecting';
  const title =
    relay.status !== 'open'
      ? 'Нет связи с mvv42.ru'
      : host
        ? `${host.name} через mvv42.ru${host.version ? ` · v${host.version}` : ''}`
        : 'Движок не выбран';
  return (
    <div className="relay-pill">
      <div className={`conn conn-${state}`} title={title}>
        <span className="led" />
        {/* with a switcher the selected host is its value */}
        {!switcher && <span className="relay-host">{host?.name ?? 'mvv42.ru'}</span>}
        {relay.status === 'open' && relay.rtt !== null && <span className="mono relay-rtt">{relay.rtt} ms</span>}
      </div>
      <HostSwitcher />
    </div>
  );
}

/** Native select (touch friendly); rendered only with more than one host online. */
export function HostSwitcher() {
  const hosts = useRemote((s) => s.relay.hosts);
  const hostId = useRemote((s) => s.relay.hostId);
  const online = hosts.filter((h) => h.online);
  if (online.length < 2) return null;
  return (
    <select
      className="select relay-switch"
      aria-label="Движок"
      value={hostId ?? ''}
      onChange={(e) => {
        if (e.target.value === '__local') connectEngine();
        else if (e.target.value) selectHost(e.target.value);
      }}
    >
      {!hostId && <option value="">Движок…</option>}
      {online.map((h) => (
        <option key={h.deviceId} value={h.deviceId}>
          {h.name}
        </option>
      ))}
      <option value="__local">Этот ПК (локально)</option>
    </select>
  );
}
