import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: { port: 5173, strictPort: true, host: '127.0.0.1' },
  build: { outDir: 'dist', target: 'es2022', sourcemap: true },
  test: { environment: 'node', include: ['tests/**/*.test.ts'] },
});
