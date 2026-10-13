import { describe, expect, it } from 'vitest';
import { detectOs } from '../src/platform/os';
import { DOWNLOAD_URLS, downloadUrl } from '../src/platform/version';

describe('detectOs', () => {
  it('maps desktop platforms and keeps iPads (MacIntel + touch) out of the Mac download', () => {
    expect(detectOs({ platform: 'Win32' })).toBe('windows');
    expect(detectOs({ platform: 'MacIntel', maxTouchPoints: 0 })).toBe('mac');
    expect(detectOs({ userAgentData: { platform: 'macOS' }, platform: 'MacIntel' })).toBe('mac');
    expect(detectOs({ userAgentData: { platform: 'Windows' } })).toBe('windows');
    expect(detectOs({ platform: 'MacIntel', maxTouchPoints: 5 })).toBe('other');
    expect(detectOs({ platform: 'Linux x86_64' })).toBe('other');
    expect(detectOs({})).toBe('other');
  });

  it('picks the release asset per OS', () => {
    expect(downloadUrl('mac')).toBe(DOWNLOAD_URLS.mac);
    expect(downloadUrl('mac').endsWith('/keysynth-macos.dmg')).toBe(true);
    expect(downloadUrl('windows').endsWith('/keysynth-setup.exe')).toBe(true);
    expect(downloadUrl('other')).toBe(DOWNLOAD_URLS.windows);
  });
});
