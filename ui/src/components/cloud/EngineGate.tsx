// Platform page without an engine (spec §1 step 2, §11 Browser): download card, install hint, connect button,
// Local Network Access permission states. Shown instead of the synth until the first `state` arrives.

import { connectEngine, useCloud } from '../../store';
import { DOWNLOAD_URL } from '../../platform/version';
import { PLATFORM_URL } from '../../platform/mode';
import { INSIDE_SHELL } from '../../platform/PlatformBridge';
import type { EnginePhase } from '../../platform/discovery';
import { AccountButton } from './AccountButton';

const LOCAL_UI = 'http://127.0.0.1:7340/';

function Steps() {
  return (
    <ol className="gate-steps">
      <li>
        <b>Скачайте установщик.</b> Если Chrome пишет «Этот файл не часто скачивают» — откройте список загрузок и
        нажмите <q>Сохранить</q>.
      </li>
      <li>
        <b>Запустите keysynth-setup.exe.</b> Windows SmartScreen может предупредить о неизвестном издателе:{' '}
        <q>Подробнее</q> → <q>Выполнить в любом случае</q>.
      </li>
      <li>
        <b>Запустите keysynth из меню «Пуск»</b> (значок появится в трее) и нажмите «Подключить движок». Браузер
        спросит доступ к устройствам в локальной сети — нажмите <q>Разрешить</q>.
      </li>
    </ol>
  );
}

function LaunchLink() {
  // Custom protocol from the sandboxed shell iframe is unreliable: there the Start-menu hint is enough.
  if (INSIDE_SHELL) return null;
  return (
    <a className="btn ghost" href="keysynth://open" title="Если keysynth уже установлен">
      Запустить
    </a>
  );
}

function DeniedHelp() {
  return (
    <div className="gate-denied">
      <p>
        Странице нужен доступ к <code>127.0.0.1</code> — там работает движок. Сейчас браузер этот доступ блокирует.
      </p>
      <ul>
        <li>
          <b>Chrome / Edge:</b> значок настроек слева от адреса → «Настройки сайтов» → <q>Доступ к локальной сети</q>{' '}
          (Local network access) → «Разрешить», затем перезагрузите страницу.
        </li>
        <li>
          <b>Brave:</b> отключите щиты для mvv42.ru или разрешите <q>Localhost access</q> в настройках сайта.
        </li>
        {INSIDE_SHELL && (
          <li>
            Внутри портала разрешение может не передаваться во вкладку сервиса —{' '}
            <a href={PLATFORM_URL} target="_blank" rel="noopener">
              откройте keysynth отдельно
            </a>
            .
          </li>
        )}
        <li>
          Доступ уже разрешён? Тогда проверьте, что keysynth запущен (меню «Пуск» или значок в трее), и нажмите
          «Повторить».
        </li>
        <li>
          Без браузерного доступа всё равно можно играть: локальный интерфейс движка —{' '}
          <a href={LOCAL_UI} target="_blank" rel="noopener">
            {LOCAL_UI}
          </a>
          .
        </li>
      </ul>
    </div>
  );
}

const TITLE: Record<EnginePhase, string> = {
  'never-seen': 'Звук играет движок на вашем компьютере',
  connecting: 'Подключаюсь к движку…',
  'permission-denied': 'Браузер блокирует доступ к движку',
  'not-running': 'Движок keysynth не запущен',
  connected: 'Движок подключён — загружаю патч…',
};

export function EngineGate() {
  const d = useCloud((s) => s.discovery);
  const phase = d.phase;
  const busy = phase === 'connecting' || phase === 'connected';
  const showDownload = phase !== 'connected' && (phase !== 'connecting' || !d.seen);

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

      <section className={`gate-card phase-${phase}`} aria-live="polite">
        <div className="gate-status">
          <span className={`led ${phase === 'connected' ? 'on' : phase === 'connecting' ? 'blink' : phase === 'permission-denied' ? 'bad' : ''}`} />
          <span className="panel-label">
            {busy ? 'ws://127.0.0.1:7341' : phase === 'never-seen' ? 'Движок не подключён' : 'Нет связи с движком'}
          </span>
        </div>
        <h1>{TITLE[phase]}</h1>

        {phase === 'never-seen' && (
          <p className="gate-lead">
            keysynth — синтезатор с низкой задержкой: интерфейс открывается здесь, а звук, MIDI и сэмплы обрабатывает
            программа-движок для Windows. Установите её один раз — дальше страница подключается сама, пресеты
            синхронизируются с аккаунтом.
          </p>
        )}
        {phase === 'connecting' && (
          <p className="gate-lead">
            {d.seen
              ? 'Ищу движок на этом компьютере.'
              : 'Если браузер спрашивает о доступе к устройствам в локальной сети — нажмите «Разрешить». Без этого странице не достучаться до движка.'}
          </p>
        )}
        {phase === 'not-running' && (
          <p className="gate-lead">
            Не удаётся подключиться к <code>127.0.0.1:7341</code>. Запустите keysynth из меню «Пуск» (или из значка в
            трее) — страница подключится сама. Ещё не установлен? Скачайте ниже. Уже запущен, а связи нет? Скорее
            всего это старая версия без поддержки mvv42.ru — установите новую поверх.
          </p>
        )}
        {phase === 'permission-denied' && <DeniedHelp />}

        <div className="gate-actions">
          {showDownload && (
            <a className="btn primary big" href={DOWNLOAD_URL} rel="noopener">
              <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden>
                <path d="M8 2v8m0 0L4.5 6.5M8 10l3.5-3.5M3 13h10" fill="none" stroke="currentColor" strokeWidth="1.6" />
              </svg>
              Скачать для Windows
            </a>
          )}
          {!busy && (
            <button type="button" className="btn big" onClick={() => connectEngine()}>
              {phase === 'never-seen' ? 'Подключить движок' : 'Повторить'}
            </button>
          )}
          {!busy && <LaunchLink />}
          {busy && <span className="gate-spinner" aria-label="Подключение" />}
        </div>

        {(phase === 'never-seen' || phase === 'not-running') && <Steps />}
        <p className="gate-foot hint">
          Windows 10/11 x64 · бесплатно · установка без прав администратора. Исходники и все версии —{' '}
          <a href="https://github.com/max31000/keysynth/releases" target="_blank" rel="noopener">
            GitHub Releases
          </a>
          .
        </p>
      </section>
    </div>
  );
}
