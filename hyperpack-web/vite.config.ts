import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import path from 'path';
import { defineConfig } from 'vite';

export default defineConfig({
  // '/HyperPack/' when deployed to GitHub Pages (https://aciderix.github.io/HyperPack/)
  // '/' for local dev and Tauri (TAURI_ENV is set during tauri build)
  base: process.env.GITHUB_PAGES === '1' ? '/HyperPack/' : '/',
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, '.'),
    },
  },
});
