// Account button in the synth style (the kit's AccountButton needs Mantine — see PlatformBridge.tsx).
// Platform standalone: «Войти» or name + menu «Выйти». Inside the shell the shell shows the account, so only «Войти»
// for an anonymous visitor. Local mode: a link to the platform page.

import { useEffect, useRef, useState } from 'react';
import { logout, requestLogin, useMe } from '@mvv42/kit/platform';
import { MODE, PLATFORM_URL, SERVICE_ID } from '../../platform/mode';
import { INSIDE_SHELL } from '../../platform/PlatformBridge';

/** Telegram avatars only (same rule as the kit's safeAvatarUrl). */
function safeAvatar(url: string | null | undefined): string | null {
  if (!url) return null;
  try {
    const u = new URL(url);
    if (u.protocol !== 'https:') return null;
    const h = u.hostname;
    return h === 't.me' || h.endsWith('.telegram.org') || /^cdn\d*\.telegram-cdn\.org$/.test(h) ? u.href : null;
  } catch {
    return null;
  }
}

function PlatformAccount() {
  const { status, user } = useMe();
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener('mousedown', close);
    return () => document.removeEventListener('mousedown', close);
  }, [open]);

  if (status === 'loading') return <span className="account-btn muted">…</span>;
  if (status === 'unavailable')
    return (
      <span className="account-btn muted" title="Сервис входа mvv42.ru недоступен">
        вход недоступен
      </span>
    );
  if (!user)
    return (
      <button type="button" className="btn small primary" onClick={() => requestLogin(SERVICE_ID)}>
        Войти
      </button>
    );
  if (INSIDE_SHELL) return null;
  const avatar = safeAvatar(user.photoUrl);
  return (
    <div className="account" ref={ref}>
      <button type="button" className="account-btn" aria-haspopup="menu" aria-expanded={open} onClick={() => setOpen((v) => !v)}>
        {avatar ? <img src={avatar} alt="" referrerPolicy="no-referrer" /> : <span className="avatar">{user.name.slice(0, 1).toUpperCase()}</span>}
        <span className="account-name">{user.name}</span>
      </button>
      {open && (
        <div className="account-menu" role="menu">
          <div className="account-menu-head">
            <b>{user.name}</b>
            {user.username && <span className="muted">@{user.username}</span>}
          </div>
          <a role="menuitem" href="/">
            Портал mvv42.ru
          </a>
          <button
            type="button"
            role="menuitem"
            onClick={() => {
              setOpen(false);
              void logout();
            }}
          >
            Выйти
          </button>
        </div>
      )}
    </div>
  );
}

export function AccountButton() {
  if (MODE.mode !== 'platform')
    return (
      <a className="btn small ghost" href={PLATFORM_URL} target="_blank" rel="noopener" title={PLATFORM_URL}>
        mvv42.ru
      </a>
    );
  return <PlatformAccount />;
}
