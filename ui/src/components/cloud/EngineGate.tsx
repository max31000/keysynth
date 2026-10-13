// Platform page without an engine (spec §1 step 2, §11 Browser): state, download, connect, Local Network Access
// permission, and «not a keysynth engine» (platform/identity.ts). Shown instead of the synth until a keysynth
// engine's `state` arrives. Texts: facts and actions only.

import { connectEngine, reconnectEngine, useCloud, useEngine, useRemote } from '../../store';
import { RelayCard } from '../remote/RelayCard';
import { downloadUrl } from '../../platform/version';
import { OS } from '../../platform/os';
import { PLATFORM_URL } from '../../platform/mode';
import { INSIDE_SHELL } from '../../platform/PlatformBridge';
import type { EnginePhase } from '../../platform/discovery';
import { AccountButton } from './AccountButton';

const LOCAL_UI = 'http://127.0.0.1:7340/';

/** Where the user starts an installed engine. */
const START_HINT = OS === 'mac' ? 'из папки «Программы»' : 'из меню «Пуск»';

function Steps() {
  if (OS === 'mac')
    return (
      <ol className="gate-steps">
        <li>
          Открыть <code>keysynth-macos.dmg</code>, перетащить keysynth в <q>Программы</q>.
        </li>
        <li>
          Первый запуск: macOS не даст открыть приложение без подписи Apple → Системные настройки → Конфиденциальность и
          безопасность → <q>Всё равно открыть</q>.
        </li>
        <li>
          Значок keysynth появится в строке меню; запрос доступа к локальной сети — <q>Разрешить</q>.
        </li>
      </ol>
    );
  return (
    <ol className="gate-steps">
      <li>
        Chrome «файл не часто скачивают»: загрузки → <q>Сохранить</q>.
      </li>
      <li>
        SmartScreen: <q>Подробнее</q> → <q>Выполнить в любом случае</q>.
      </li>
      <li>
        Запрос доступа к локальной сети — <q>Разрешить</q>.
      </li>
    </ol>
  );
}

function LaunchLink() {
  // Custom protocol from the sandboxed shell iframe is unreliable: there the Start-menu / Applications hint is enough.
  if (INSIDE_SHELL) return null;
  return (
    <a className="btn ghost" href="keysynth://open" title="keysynth://open">
      Запустить
    </a>
  );
}

function DeniedHelp() {
  return (
    <div className="gate-denied">
      <ul>
        <li>
          <b>Chrome / Edge:</b> настройки сайта → <q>Доступ к локальной сети</q> → «Разрешить», перезагрузить.
        </li>
        <li>
          <b>Brave:</b> щиты для mvv42.ru → <q>Localhost access</q>.
        </li>
        {OS === 'mac' && (
          <li>
            <b>Safari:</b> если страница не видит запущенный движок — откройте локальный интерфейс (ниже) или
            Chrome.
          </li>
        )}
        {INSIDE_SHELL && (
          <li>
            <a href={PLATFORM_URL} target="_blank" rel="noopener">
              Открыть отдельно
            </a>{' '}
            (из портала разрешение может не передаваться).
          </li>
        )}
        <li>
          Локальный интерфейс:{' '}
          <a href={LOCAL_UI} target="_blank" rel="noopener">
            {LOCAL_UI}
          </a>
        </li>
      </ul>
    </div>
  );
}

/** Something answers on the engine port, but it is not a keysynth engine (dev mock, another program). */
function NotKeysynth() {
  const identity = useEngine((s) => s.identity);
  const info = useEngine((s) => s.engineInfo);
  return (
    <div className="gate-denied">
      <p>
        {identity === 'mock' ? 'Тестовый сервер разработчика (mock)' : 'Неизвестная программа'}
        {info?.pid ? <> · PID {info.pid}</> : null}
      </p>
      <ul>
        <li>
          Закройте её ({OS === 'mac' ? 'Мониторинг системы' : 'Диспетчер задач'}
          {info?.pid ? `, PID ${info.pid}` : ''}; mock:{' '}
          <code>{OS === 'mac' ? 'scripts/stop-dev.sh' : 'scripts/stop-dev.ps1'}</code>).
        </li>
        <li>Запустите keysynth {START_HINT} — страница переподключится сама.</li>
      </ul>
    </div>
  );
}

const TITLE: Record<EnginePhase, string> = {
  'never-seen': 'Движок не подключён',
  connecting: 'Подключение…',
  'permission-denied': 'Браузер блокирует доступ к локальной сети',
  'not-running': 'Движок не запущен',
  connected: 'Загрузка патча…',
};

export const notKeysynthTitle = (port: string) =>
  `На порту ${port} отвечает не движок keysynth (тестовый сервер / другая программа)`;

const portOf = (url: string) => /:(\d+)/.exec(url.replace(/^wss?:\/\//, ''))?.[1] ?? '7341';

export function EngineGate({ notKeysynth = false }: { notKeysynth?: boolean }) {
  const d = useCloud((s) => s.discovery);
  const relay = useRemote((s) => s.route === 'relay');
  const url = useEngine((s) => s.connection.url);
  const phase = d.phase;
  const busy = !notKeysynth && (phase === 'connecting' || phase === 'connected');
  const showDownload = !notKeysynth && phase !== 'connected' && (phase !== 'connecting' || !d.seen);
  const led = notKeysynth
    ? 'bad'
    : phase === 'connected'
      ? 'on'
      : phase === 'connecting'
        ? 'blink'
        : phase === 'permission-denied'
          ? 'bad'
          : '';

  return (
    <div className="gate" role="main">
      <header className="gate-top">
        <div className="brand">
          <span className="brand-mark" aria-hidden>
            <i />
            <i />
            <i />
          </span>
          <span className="brand-name">keysynth</span>
        </div>
        <div className="spacer" />
        <AccountButton />
      </header>

      {relay && !notKeysynth ? (
        <RelayCard />
      ) : (
        <section className={`gate-card phase-${notKeysynth ? 'not-keysynth' : phase}`} aria-live="polite">
          <div className="gate-status">
            <span className={`led ${led}`} />
            <span className="panel-label">{url}</span>
          </div>
          <h1>{notKeysynth ? notKeysynthTitle(portOf(url)) : TITLE[phase]}</h1>

          {notKeysynth && <NotKeysynth />}
          {!notKeysynth && phase === 'not-running' && (
            <p className="gate-lead">
              Нет ответа на <code>127.0.0.1:7341</code>. Запустите keysynth {START_HINT}. Запущен, но связи нет — обновите.
            </p>
          )}
          {!notKeysynth && phase === 'permission-denied' && <DeniedHelp />}

          <div className="gate-actions">
            {showDownload && (
              <a className="btn primary big" href={downloadUrl(OS)} rel="noopener">
                <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden>
                  <path d="M8 2v8m0 0L4.5 6.5M8 10l3.5-3.5M3 13h10" fill="none" stroke="currentColor" strokeWidth="1.6" />
                </svg>
                {OS === 'mac' ? 'Скачать для macOS' : 'Скачать для Windows'}
              </a>
            )}
            {!busy && (
              <button type="button" className="btn big" onClick={() => (notKeysynth ? reconnectEngine() : connectEngine())}>
                {phase === 'never-seen' && !notKeysynth ? 'Подключить' : 'Повторить'}
              </button>
            )}
            {!busy && !notKeysynth && <LaunchLink />}
            {busy && <span className="gate-spinner" aria-label="Подключение" />}
          </div>

          {!notKeysynth && (phase === 'never-seen' || phase === 'not-running') && <Steps />}
          <p className="gate-foot hint">
            <a href="https://github.com/max31000/keysynth/releases" target="_blank" rel="noopener">
              GitHub Releases
            </a>
          </p>
        </section>
      )}
    </div>
  );
}
