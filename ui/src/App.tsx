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
import { identityRejected } from './platform/identity';

function Toast() {
  const err = useEngine((s) => s.lastError);
  const notice = useEngine((s) => s.notice);
  const text = err ?? notice;
  useEffect(() => {
    if (!text) return;
    const t = setTimeout(() => actions().clearError(), err ? 5000 : 8000);
    return () => clearTimeout(t);
  }, [text, err]);
  if (!text) return null;
  return (
    <div className={`toast ${err ? '' : 'notice'}`} role={err ? 'alert' : 'status'} onClick={() => actions().clearError()}>
      {text}
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
          ? 'Нет доступа к локальной сети — разрешите в настройках сайта.'
          : phase === 'not-running'
            ? 'Движок не отвечает.'
            : 'Нет связи с движком, переподключение…'}
        <button type="button" className="btn small" onClick={() => connectEngine()}>
          Повторить
        </button>
      </div>
    );
  return (
    <div className={`offline ${hasPatch ? 'stale' : ''}`}>
      <span className="led" /> Engine not reachable at <code>{url}</code>, retrying.
    </div>
  );
}

export function App() {
  const [audioOpen, setAudioOpen] = useState(false);
  const [cloudOpen, setCloudOpen] = useState(false);
  const hasPatch = useEngine((s) => s.patch !== null);
  // a dev mock / another program on the engine port: never show it as the synth (platform/identity.ts)
  const notKeysynth = useEngine((s) => identityRejected(s.identity, MODE.dev));
  const platform = MODE.mode === 'platform';
  return (
    <>
      {platform && <PlatformBridge />}
      {notKeysynth || (platform && !hasPatch) ? (
        <EngineGate notKeysynth={notKeysynth} />
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
