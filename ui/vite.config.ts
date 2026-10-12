import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { platformStub } from './dev/platformStub';

// Modes (docs/TESTING.md "Web UI"):
//   default        local UI: base '/', served by the engine at :7340 or `npm run dev`
//   platform       `npm run build:platform`: base '/keysynth/' for https://mvv42.ru/keysynth/ (output dist-platform/)
//   platform-dev   `npm run dev:platform`: base '/keysynth/' + stubbed platform endpoints (dev/platformStub.ts)
export default defineConfig(({ mode }) => {
  const platform = mode === 'platform' || mode === 'platform-dev';
  const stub = mode === 'platform-dev';
  return {
    base: platform ? '/keysynth/' : '/',
    plugins: [react(), ...(stub ? [platformStub({ latest: process.env.KS_STUB_LATEST ?? null })] : [])],
    define: stub ? { 'import.meta.env.VITE_KS_RELEASES_API': JSON.stringify('/__stub/github-latest') } : {},
    server: { port: 5173, strictPort: true, host: '127.0.0.1' },
    // sourcemaps are published on purpose (public AGPL repo; readable stack traces from users)
    build: { outDir: platform ? 'dist-platform' : 'dist', target: 'es2022', sourcemap: true },
    test: { environment: 'node', include: ['tests/**/*.test.ts'] },
  };
});
