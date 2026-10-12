// Gate card on the relay route (signed in, no local engine): relay state, host choice, local option.

import { connectEngine, retryRelay, selectHost, useCloud, useEngine, useRemote } from '../../store';

export function RelayCard() {
  const relay = useRemote((s) => s.relay);
  const status = useEngine((s) => s.connection.status);
  const me = useCloud((s) => s.me.name);
  const online = relay.hosts.filter((h) => h.online);
  const host = relay.hosts.find((h) => h.deviceId === relay.hostId);

  let title: string;
  let led = 'blink';
  if (relay.status === 'connecting' || (relay.status === 'reconnecting' && relay.failures < 2)) title = 'Подключение к mvv42.ru…';
  else if (relay.status !== 'open') {
    title = 'Нет связи с mvv42.ru';
    led = 'bad';
  } else if (online.length === 0) {
    title = 'Нет движков в сети';
    led = '';
  } else if (!host) title = 'Выберите движок';
  else title = status === 'open' ? `«${host.name}»: загрузка патча…` : `Подключение к «${host.name}»…`;

  return (
    <section className="gate-card relay-card" aria-live="polite">
      <div className="gate-status">
        <span className={`led ${led}`} />
        <span className="panel-label">mvv42.ru{relay.rtt !== null && relay.status === 'open' ? ` · ${relay.rtt} ms` : ''}</span>
      </div>
      <h1>{title}</h1>
      {relay.status === 'open' && online.length === 0 && (
        <p className="gate-lead">Запустите keysynth на ПК, привязанном к аккаунту{me ? ` «${me}»` : ''}.</p>
      )}
      {relay.status === 'open' && online.length > 1 && (
        <div className="relay-hosts">
          {online.map((h) => (
            <button
              key={h.deviceId}
              type="button"
              className={`btn big${h.deviceId === relay.hostId ? ' primary' : ''}`}
              onClick={() => selectHost(h.deviceId)}
            >
              {h.name}
            </button>
          ))}
        </div>
      )}
      <div className="gate-actions">
        {relay.status !== 'open' && (
          <button type="button" className="btn big" onClick={() => retryRelay()}>
            Повторить
          </button>
        )}
        <button type="button" className="btn ghost" onClick={() => connectEngine()} title="ws://127.0.0.1:7341">
          Движок на этом ПК
        </button>
      </div>
    </section>
  );
}
