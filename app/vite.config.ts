// =============================================================================
// vite.config.ts - 只构建渲染层
//
// root 指向 src/renderer；产物输出到 app/dist/renderer。
// base: './' 让资源使用相对路径，Electron 用 file:// 加载时才能找到。
// =============================================================================

import { resolve } from 'node:path';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

export default defineConfig({
  root: resolve(__dirname, 'src/renderer'),
  base: './',
  plugins: [react()],
  build: {
    outDir: resolve(__dirname, 'dist/renderer'),
    emptyOutDir: true,
    // Electron 45 的 Chromium 版本足够新，不需要兼容更老的浏览器。
    target: 'chrome120',
  },
});
