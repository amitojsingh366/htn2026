import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import { ErrorBoundary } from '@sentry/react'
import { initializeTelemetry } from './telemetry'

initializeTelemetry()

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ErrorBoundary fallback={<main role="alert"><h1>The dashboard could not render.</h1><p>Reload to reconnect to the game.</p><button onClick={() => location.reload()}>Reload dashboard</button></main>}>
      <App />
    </ErrorBoundary>
  </StrictMode>,
)
