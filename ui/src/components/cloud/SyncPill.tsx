// Top-bar cloud indicator: link + preset sync state (PROTOCOL.md `sync_status`, `cloud_status`). Opens the drawer.

import { useShallow } from 'zustand/react/shallow';
import { useCloud } from '../../store';
import type { CloudState } from '../../store/cloudStore';

export interface SyncView {
  tone: 'ok' | 'busy' | 'warn' | 'bad' | 'off';
  label: string;
  title: string;
}

export function formatTime(iso: string | null | undefined): string {
  if (!iso) return '—';
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '—';
  const sameYear = d.getFullYear() === new Date().getFullYear();
  return d.toLocaleString('ru-RU', { day: '2-digit', month: '2-digit', ...(sameYear ? {} : { year: 'numeric' }), hour: '2-digit', minute: '2-digit' });
}

export function syncView(s: Pick<CloudState, 'cloud' | 'sync' | 'link' | 'mode' | 'me'>): SyncView {
  const { cloud, sync, link } = s;
  if (link.busy) return { tone: 'busy', label: 'Привязка…', title: 'Привязываю движок к аккаунту' };
  if (link.error) return { tone: 'bad', label: 'Не привязан', title: link.error };
  if (!cloud?.linked) {
    if (s.mode === 'platform' && s.me.status === 'anonymous') return { tone: 'off', label: 'Войдите', title: 'Войдите, чтобы синхронизировать пресеты' };
    return { tone: 'off', label: 'Локально', title: 'Движок не привязан к аккаунту: пресеты хранятся только на этом ПК' };
  }
  if (link.owner === 'foreign') return { tone: 'warn', label: 'Чужой аккаунт', title: `Движок привязан к аккаунту «${cloud.accountName ?? '?'}»` };
  const conflicts = sync?.conflicts.length ?? 0;
  if (conflicts) return { tone: 'warn', label: `Конфликты: ${conflicts}`, title: 'Есть конфликтующие копии пресетов — откройте панель' };
  switch (sync?.state) {
    case 'syncing':
      return { tone: 'busy', label: 'Синхронизация', title: 'Обмен пресетами с mvv42.ru' };
    case 'offline':
      return { tone: 'warn', label: 'Офлайн', title: 'Нет связи с mvv42.ru — изменения уйдут позже' };
    case 'error':
      return { tone: 'bad', label: 'Ошибка синхр.', title: `Ошибка синхронизации: ${sync.error ?? 'unknown'}` };
    default: {
      const pending = sync?.pending ?? 0;
      if (!cloud.online) return { tone: 'warn', label: 'Офлайн', title: cloud.error ?? 'Нет связи с mvv42.ru' };
      return pending
        ? { tone: 'busy', label: `Ждут: ${pending}`, title: `${pending} изменений ещё не отправлены` }
        : { tone: 'ok', label: 'Синхронизировано', title: `Аккаунт ${cloud.accountName ?? ''} · последняя синхронизация ${formatTime(sync?.lastSyncAt)}` };
    }
  }
}

export function SyncPill({ onOpen }: { onOpen: () => void }) {
  const v = useCloud(useShallow(syncView));
  return (
    <button type="button" className={`sync-pill tone-${v.tone}`} onClick={onOpen} title={v.title} aria-label={`Облако: ${v.label}`}>
      <svg viewBox="0 0 20 14" width="16" height="12" aria-hidden>
        <path
          d="M5.5 12.5h9a3.5 3.5 0 0 0 .4-6.98A5 5 0 0 0 5.3 5.1 3.7 3.7 0 0 0 5.5 12.5Z"
          fill="none"
          stroke="currentColor"
          strokeWidth="1.4"
        />
      </svg>
      <span>{v.label}</span>
    </button>
  );
}
