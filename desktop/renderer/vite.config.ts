import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Built into renderer/dist and served by the backend (same origin as /api).
// Dev: `npm run dev` proxies /api to a backend started on port 8765.
export default defineConfig({
  plugins: [react()],
  base: './',
  server: { proxy: { '/api': 'http://127.0.0.1:8765' } },
})
