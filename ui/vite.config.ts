import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { platformStub } from './dev/platformStub';

// Modes (docs/TESTING.md "Web UI"):
//   default        local UI: base '/', served by the engine at :7340 or `npm run dev`
//   mock           `npm run dev:mock`: local UI pointed at the mock engine (ws://127.0.0.1:7441, `npm run mock`)
//   platform       `npm run build:platform`: base '/keysynth/' for https://mvv42.ru/keysynth/ (output dist-platform/)
//   platform-dev   `npm run dev:platform`: base '/keysynth/' + stubbed platform endpoints (dev/platformStub.ts), engine =
//                  the mock (ws://127.0.0.1:7441)
// Dev servers only: KS_ENGINE=ws://127.0.0.1:<port> sets the default engine URL (`?engine=` still wins).
export default defineConfig(({ mode, command }) => {
  const platform = mode === 'platform' || mode === 'platform-dev';
  const stub = mode === 'platform-dev';
  const engine = command === 'serve' ? (process.env.KS_ENGINE ?? (mode === 'mock' || stub ? 'ws://127.0.0.1:7441' : '')) : '';
  return {
    base: platform ? '/keysynth/' : '/',
    plugins: [react(), ...(stub ? [platformStub({ latest: process.env.KS_STUB_LATEST ?? null })] : [])],
    define: {
      ...(stub ? { 'import.meta.env.VITE_KS_RELEASES_API': JSON.stringify('/__stub/github-latest') } : {}),
      ...(engine ? { 'import.meta.env.VITE_KS_ENGINE': JSON.stringify(engine) } : {}),
    },
    server: { port: 5173, strictPort: true, host: '127.0.0.1' },
    // sourcemaps are published on purpose (public AGPL repo; readable stack traces from users)
    build: { outDir: platform ? 'dist-platform' : 'dist', target: 'es2022', sourcemap: true },
    test: { environment: 'node', include: ['tests/**/*.test.ts'] },
  };
});
