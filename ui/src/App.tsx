import { useEffect, useState } from 'react';
import { actions, connectEngine, retryRelay, useCloud, useEngine, useRemote, useSongs } from './store';
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
import { RemoteView } from './components/remote/RemoteView';
import { useView } from './lib/view';
import { SongsDrawer } from './components/songs/SongsDrawer';
import { SongBar } from './components/songs/SongBar';

function Toast() {
  const err = useEngine((s) => s.lastError);
  const notice = useEngine((s) => s.notice);
  // an error first; a notice that arrived meanwhile shows after it
  const text = err ?? notice;
  const clear = () => (err ? actions().clearError() : actions().clearNotice());
  useEffect(() => {
    if (!text) return;
    const t = setTimeout(() => (err ? actions().clearError() : actions().clearNotice()), err ? 5000 : 8000);
    return () => clearTimeout(t);
  }, [text, err]);
  if (!text) return null;
  return (
    <div className={`toast ${err ? '' : 'notice'}`} role={err ? 'alert' : 'status'} onClick={clear}>
      {text}
    </div>
  );
}

function OfflineBanner() {
  const status = useEngine((s) => s.connection.status);
  const url = useEngine((s) => s.connection.url);
  const hasPatch = useEngine((s) => s.patch !== null);
  const phase = useCloud((s) => s.discovery.phase);
  const relay = useRemote((s) => (s.route === 'relay' ? s.relay : null));
  if (status === 'open') return null;
  if (relay) {
    const host = relay.hosts.find((h) => h.deviceId === relay.hostId);
    return (
      <div className="offline stale">
        <span className="led" />
        {relay.status !== 'open'
          ? 'Нет связи с mvv42.ru, переподключение…'
          : host && !host.online
            ? `«${host.name}» не в сети.`
            : host
              ? `Подключение к «${host.name}»…`
              : relay.hosts.some((h) => h.online)
                ? 'Движок не выбран.'
                : 'Нет движков в сети.'}
        <button type="button" className="btn small" onClick={() => retryRelay()}>
          Повторить
        </button>
      </div>
    );
  }
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
  const [songsOpen, setSongsOpen] = useState(false);
  const hasSong = useSongs((s) => !!s.state?.song);
  const [view, setView] = useView();
  const hasPatch = useEngine((s) => s.patch !== null);
  // a dev mock / another program on the engine port: never show it as the synth (platform/identity.ts)
  const notKeysynth = useEngine((s) => identityRejected(s.identity, MODE.dev));
  const platform = MODE.mode === 'platform';
  return (
    <>
      {platform && <PlatformBridge />}
      {notKeysynth || (platform && !hasPatch) ? (
        <EngineGate notKeysynth={notKeysynth} />
      ) : view === 'pult' ? (
        <>
          <RemoteView onEditor={() => setView('editor')} />
          <OfflineBanner />
          <UpdateBanner />
          <Toast />
          <SampleToast />
        </>
      ) : (
        <div className={`app${hasSong ? ' has-song' : ''}`}>
          <TopBar
            onAudioSettings={() => setAudioOpen(true)}
            onCloud={() => setCloudOpen(true)}
            onPult={() => setView('pult')}
            onSongs={() => setSongsOpen(true)}
          />
          <SongBar onOpen={() => setSongsOpen(true)} />
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
          <SongsDrawer open={songsOpen} onClose={() => setSongsOpen(false)} />
        </div>
      )}
      <LinkConfirm />
    </>
  );
}
