import react from '@vitejs/plugin-react'
import { sentryVitePlugin } from '@sentry/vite-plugin'
import { defineConfig, loadEnv } from 'vite'

export default defineConfig(({ mode }) => {
  const env = { ...loadEnv(mode, process.cwd(), ''), ...process.env }
  const uploadMaps = env.SENTRY_UPLOAD_SOURCEMAPS === 'true'
  if (uploadMaps && (!env.SENTRY_AUTH_TOKEN || !env.SENTRY_ORG || !env.SENTRY_PROJECT || !env.VITE_SENTRY_RELEASE)) {
    throw new Error('Source-map upload requires SENTRY_AUTH_TOKEN, SENTRY_ORG, SENTRY_PROJECT and VITE_SENTRY_RELEASE.')
  }
  return {
    plugins: [react(), ...(uploadMaps ? [sentryVitePlugin({
      org: env.SENTRY_ORG,
      project: env.SENTRY_PROJECT,
      authToken: env.SENTRY_AUTH_TOKEN,
      telemetry: false,
      release: { name: env.VITE_SENTRY_RELEASE },
      sourcemaps: { filesToDeleteAfterUpload: ['./dist/**/*.map'] },
    })] : [])],
    build: { sourcemap: uploadMaps ? 'hidden' : false },
  }
})
