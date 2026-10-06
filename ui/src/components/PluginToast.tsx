// Plugin hot-reload status (docs/PROTOCOL.md `plugin_status`): compiling / ok / error with the compiler message.
// ok and removed auto-hide; errors and faults stay until dismissed and offer a reload.

import { useEffect } from 'react';
import { actions, useEngine } from '../store';
import type { PluginStatus } from '../protocol/types';

const LABEL: Record<PluginStatus['state'], string> = {
  compiling: 'compiling…',
  ok: 'loaded',
  error: 'error',
  faulted: 'faulted',
  removed: 'removed',
};

export function PluginToast() {
  const p = useEngine((s) => s.pluginNotice);
  useEffect(() => {
    if (!p || (p.state !== 'ok' && p.state !== 'removed')) return;
    const t = setTimeout(() => actions().dismissPluginNotice(), 3500);
    return () => clearTimeout(t);
  }, [p]);
  if (!p) return null;
  const bad = p.state === 'error' || p.state === 'faulted';
  const detail =
    p.state === 'ok'
      ? `v${p.version} · ${p.source}${p.kind ? ` ${p.kind}` : ''} · ${Math.round(p.compileMs)} ms${p.cached ? ' (cached)' : ''}`
      : null;
  return (
    <div className={`plugin-toast state-${p.state}`} role={bad ? 'alert' : 'status'} aria-live="polite">
      <div className="plugin-toast-head">
        <span className="led" />
        <span className="plugin-toast-name">{p.name}</span>
        <span className="plugin-toast-state">{LABEL[p.state]}</span>
        {detail && <span className="plugin-toast-detail">{detail}</span>}
        {bad && (
          <button type="button" className="plugin-toast-btn" onClick={() => void actions().reloadPlugin(p.name)}>
            Reload
          </button>
        )}
        <button
          type="button"
          className="plugin-toast-btn close"
          aria-label="Dismiss"
          onClick={() => actions().dismissPluginNotice()}
        >
          ×
        </button>
      </div>
      {bad && p.message && <pre className="plugin-toast-msg">{p.message}</pre>}
    </div>
  );
}
