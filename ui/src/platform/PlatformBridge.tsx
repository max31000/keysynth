// Platform glue for React: shell bridge (READY; THEME is ignored — keysynth is always dark; AUTH_CHANGED re-reads
// /me inside the kit's useMe) and the current user → cloud store (drives the automatic account link).
// Rendered only in platform mode (the local UI at :7340 has no platform auth).
//
// Why not the kit's Mvv42Provider/AccountButton: they render MantineProvider and need Mantine's + the kit's global
// CSS (body font/background, scrollbars, focus ring), which restyles the synth UI. keysynth uses only the
// framework-free `@mvv42/kit/platform` entry and draws its own account button in the synth style.

import { useEffect } from 'react';
import { isInsideShell, postToShell, useMe } from '@mvv42/kit/platform';
import { SERVICE_ID } from './mode';
import { cloudActions } from '../store';

export const INSIDE_SHELL = typeof window !== 'undefined' && isInsideShell();

export function PlatformBridge() {
  useEffect(() => {
    if (INSIDE_SHELL) postToShell({ type: 'READY', serviceId: SERVICE_ID });
  }, []);

  const { status, user, app } = useMe();
  const granted = !!user && (user.isAdmin || !!app(SERVICE_ID)?.granted);
  const name = user?.name ?? null;
  useEffect(() => {
    cloudActions().setMe({ status, granted, name });
  }, [status, granted, name]);
  return null;
}
