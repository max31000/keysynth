// keysynth REST API (docs/specs/2026-10-12-cloud-api.md §3.1) over the platform client (cookies, 401 → refresh).

import { apiJson } from '@mvv42/kit/platform';
import { API_BASE } from './mode';
import type { DeviceRow, Minted } from './link';

export interface DeviceMinted extends Minted {
  deviceId: string;
}

export function mintDevice(deviceId: string, name: string): Promise<DeviceMinted> {
  return apiJson<DeviceMinted>(`${API_BASE}/devices`, {
    method: 'POST',
    body: JSON.stringify({ deviceId, name: name.trim().slice(0, 100) }),
  });
}

export interface DeviceItem extends DeviceRow {
  createdAt: string;
  lastSeenAt: string | null;
}

export async function listDevices(): Promise<DeviceItem[]> {
  const r = await apiJson<{ items: DeviceItem[] }>(`${API_BASE}/devices`);
  return Array.isArray(r?.items) ? r.items : [];
}
