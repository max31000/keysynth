// Cloud status card: linked account, this device, token expiry, preset sync, unlink (PROTOCOL.md "Cloud & samples").

import type { ReactNode } from 'react';
import { requestLogin } from '@mvv42/kit/platform';
import { cloudActions, useCloud } from '../../store';
import { MODE, PLATFORM_URL, SERVICE_ID } from '../../platform/mode';
import { formatTime } from './SyncPill';
import { canSend } from '../../platform/trust';

const SYNC_LABEL: Record<string, string> = {
  disabled: 'выключена',
  idle: 'готово',
  syncing: 'идёт…',
  offline: 'нет связи',
  error: 'ошибка',
};

export function AccountCard() {
  const cloud = useCloud((s) => s.cloud);
  const sync = useCloud((s) => s.sync);
  const link = useCloud((s) => s.link);
  const me = useCloud((s) => s.me);
  const plan = useCloud((s) => s.plan().kind);
  // through the relay the host's link is fixed (cloud_link / cloud_unlink are denied for remote sessions)
  const canLink = useCloud((s) => canSend(s.trust, 'cloud_link'));
  const canUnlink = useCloud((s) => canSend(s.trust, 'cloud_unlink'));
  const a = cloudActions();
  const platform = MODE.mode === 'platform';

  let body: ReactNode;
  if (!cloud) {
    body = <p className="muted">Движок не подключён.</p>;
  } else if (!platform && !cloud.linked) {
    body = (
      <>
        <p>Не привязан. Привязка — на mvv42.ru/keysynth после входа.</p>
        <a className="btn primary" href={PLATFORM_URL} target="_blank" rel="noopener">
          Открыть mvv42.ru/keysynth
        </a>
      </>
    );
  } else if (platform && plan === 'login' && !cloud.linked) {
    body = (
      <>
        <p>Не привязан.</p>
        <button type="button" className="btn primary" onClick={() => requestLogin(SERVICE_ID)}>
          Войти через Telegram
        </button>
      </>
    );
  } else if (platform && plan === 'no-access' && !cloud.linked) {
    body = <p>У аккаунта «{me.name}» нет доступа к keysynth.</p>;
  } else if (!cloud.linked) {
    body = (
      <>
        <p>{link.busy ? 'Привязка…' : link.held ? 'Отвязан. Пресеты только на этом ПК.' : 'Не привязан.'}</p>
        {!link.busy && canLink && (
          <button type="button" className="btn primary" onClick={() => void a.retryLink()}>
            Привязать к аккаунту{me.name ? ` «${me.name}»` : ''}
          </button>
        )}
      </>
    );
  } else {
    const foreign = platform && link.owner === 'foreign';
    body = (
      <>
        <dl className="kv">
          <dt>Аккаунт</dt>
          <dd>
            <b>{cloud.accountName ?? cloud.accountId}</b>
            {foreign && <span className="pill warn">не ваш</span>}
          </dd>
          <dt>Устройство</dt>
          <dd className="mono" title={cloud.deviceId}>
            {cloud.deviceId.slice(0, 8)}
          </dd>
          <dt>Ключ до</dt>
          <dd>{formatTime(cloud.expiresAt)}</dd>
          <dt>Связь</dt>
          <dd>
            <span className={`led ${cloud.online ? 'on' : 'warn'}`} /> {cloud.online ? 'в сети' : (cloud.error ?? 'нет связи')}
          </dd>
          <dt>Пресеты</dt>
          <dd>
            {SYNC_LABEL[sync?.state ?? 'disabled']}
            {sync?.lastSyncAt ? ` · ${formatTime(sync.lastSyncAt)}` : ''}
            {sync?.pending ? ` · ждут отправки: ${sync.pending}` : ''}
            {sync?.state === 'error' && sync.error ? ` · ${sync.error}` : ''}
          </dd>
        </dl>
        {foreign && (
          <p className="note warn">
            Привязан к «{cloud.accountName}», не к{me.name ? ` «${me.name}»` : ' вашему аккаунту'}.{' '}
            <button type="button" className="btn small" onClick={() => void a.retryLink()}>
              Привязать к моему
            </button>
          </p>
        )}
        {platform && plan === 'login' && (
          <p className="note">
            Вход не выполнен.{' '}
            <button type="button" className="btn small" onClick={() => requestLogin(SERVICE_ID)}>
              Войти
            </button>
          </p>
        )}
        {link.merged && link.merged.presets + link.merged.patterns > 0 && (
          <p className="note">
            Перенесено в аккаунт: пресетов {link.merged.presets}, паттернов {link.merged.patterns}.
          </p>
        )}
        {!!sync?.conflicts.length && (
          <div className="note warn">
            Конфликты, локальные копии:
            <ul>
              {sync.conflicts.map((c) => (
                <li key={c.file} className="mono">
                  {c.slug} · {formatTime(c.at)}
                </li>
              ))}
            </ul>
          </div>
        )}
        <div className="row gap">
          <button type="button" className="btn small" disabled={sync?.state === 'syncing'} onClick={() => void a.syncNow()}>
            Синхронизировать
          </button>
          {canUnlink && (
            <button
              type="button"
              className="btn small ghost"
              disabled={link.busy}
              onClick={() => {
                if (window.confirm('Отвязать движок от аккаунта? Синхронизация остановится, пресеты останутся на ПК.'))
                  void a.unlink();
              }}
            >
              Отвязать
            </button>
          )}
        </div>
      </>
    );
  }

  return (
    <section className="cloud-card">
      <div className="cloud-card-head">
        <span className="panel-label">Аккаунт и синхронизация</span>
      </div>
      {body}
      {link.error && canLink && (
        <p className="note bad" role="alert">
          {link.error}{' '}
          <button type="button" className="btn small" onClick={() => void a.retryLink()}>
            Повторить
          </button>
        </p>
      )}
    </section>
  );
}
