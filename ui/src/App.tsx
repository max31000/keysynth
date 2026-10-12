import { useEffect, useState } from 'react';
import { actions, connectEngine, useCloud, useEngine } from './store';
import { TopBar } from './components/TopBar';
import { PresetBrowser } from './components/PresetBrowser';
import { Rack } from './components/Rack';
import { MasterSection } from './components/MasterSection';
import { Keyboard } from './components/Keyboard';
import { AudioSettings } from './components/AudioSettings';
import { RhythmPanel } from './components/RhythmPanel';
import { PluginToast } from './components/PluginToast';
import { EngineGate } from './components/cloud/EngineGate';
import { CloudDrawer } from './components/cloud/CloudDrawer';
import { LinkConfirm, SampleToast, UpdateBanner } from './components/cloud/CloudNotices';
import { PlatformBridge } from './platform/PlatformBridge';
import { MODE } from './platform/mode';

function Toast() {
  const err = useEngine((s) => s.lastError);
  useEffect(() => {
    if (!err) return;
    const t = setTimeout(() => actions().clearError(), 5000);
    return () => clearTimeout(t);
  }, [err]);
  if (!err) return null;
  return (
    <div className="toast" role="alert" onClick={() => actions().clearError()}>
      {err}
    </div>
  );
}

function OfflineBanner() {
  const status = useEngine((s) => s.connection.status);
  const url = useEngine((s) => s.connection.url);
  const hasPatch = useEngine((s) => s.patch !== null);
  const phase = useCloud((s) => s.discovery.phase);
  if (status === 'open') return null;
  if (MODE.mode === 'platform')
    return (
      <div className="offline stale">
        <span className="led" />
        {phase === 'permission-denied'
          ? 'Браузер заблокировал доступ к движку (локальная сеть) — разрешите его в настройках сайта.'
          : phase === 'not-running'
            ? 'Движок keysynth не отвечает — запустите его из меню «Пуск».'
            : 'Связь с движком потеряна — переподключаюсь…'}
        <button type="button" className="btn small" onClick={() => connectEngine()}>
          Повторить
        </button>
      </div>
    );
  return (
    <div className={`offline ${hasPatch ? 'stale' : ''}`}>
      <span className="led" /> Engine not reachable at <code>{url}</code> — retrying. Start <code>keysynth-engine</code> or{' '}
      <code>npm run mock</code>.
    </div>
  );
}

export function App() {
  const [audioOpen, setAudioOpen] = useState(false);
  const [cloudOpen, setCloudOpen] = useState(false);
  const hasPatch = useEngine((s) => s.patch !== null);
  const platform = MODE.mode === 'platform';
  return (
    <>
      {platform && <PlatformBridge />}
      {platform && !hasPatch ? (
        <EngineGate />
      ) : (
        <div className="app">
          <TopBar onAudioSettings={() => setAudioOpen(true)} onCloud={() => setCloudOpen(true)} />
          <PresetBrowser />
          <Rack />
          <MasterSection />
          <RhythmPanel />
          <Keyboard />
          <OfflineBanner />
          <UpdateBanner />
          <Toast />
          <PluginToast />
          <SampleToast />
          <AudioSettings open={audioOpen} onClose={() => setAudioOpen(false)} />
          <CloudDrawer open={cloudOpen} onClose={() => setCloudOpen(false)} />
        </div>
      )}
      <LinkConfirm />
    </>
  );
}
