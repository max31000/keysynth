import { useEffect, useState } from 'react';
import { actions, useEngine } from './store';
import { TopBar } from './components/TopBar';
import { PresetBrowser } from './components/PresetBrowser';
import { Rack } from './components/Rack';
import { MasterSection } from './components/MasterSection';
import { Keyboard } from './components/Keyboard';
import { AudioSettings } from './components/AudioSettings';

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
  if (status === 'open') return null;
  return (
    <div className={`offline ${hasPatch ? 'stale' : ''}`}>
      <span className="led" /> Engine not reachable at <code>{url}</code> — retrying. Start <code>keysynth-engine</code> or{' '}
      <code>npm run mock</code>.
    </div>
  );
}

export function App() {
  const [audioOpen, setAudioOpen] = useState(false);
  return (
    <div className="app">
      <TopBar onAudioSettings={() => setAudioOpen(true)} />
      <PresetBrowser />
      <Rack />
      <MasterSection />
      <Keyboard />
      <OfflineBanner />
      <Toast />
      <AudioSettings open={audioOpen} onClose={() => setAudioOpen(false)} />
    </div>
  );
}
