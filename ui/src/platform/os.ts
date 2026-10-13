// Desktop OS of this browser: picks the engine download and the OS-specific instructions (EngineGate). Only
// desktop OSes the engine ships for; phones / tablets (iPad Safari reports "MacIntel") are 'other'.

export type DesktopOs = 'windows' | 'mac' | 'other';

interface NavigatorLike {
  platform?: string;
  userAgent?: string;
  maxTouchPoints?: number;
  userAgentData?: { platform?: string };
}

export function detectOs(nav: NavigatorLike | undefined = typeof navigator !== 'undefined' ? navigator : undefined): DesktopOs {
  if (!nav) return 'other';
  const p = (nav.userAgentData?.platform || nav.platform || nav.userAgent || '').toLowerCase();
  if (p.includes('win')) return 'windows';
  if (p.includes('mac')) return (nav.maxTouchPoints ?? 0) > 1 ? 'other' : 'mac'; // iPadOS desktop-mode Safari
  return 'other';
}

export const OS = detectOs();
