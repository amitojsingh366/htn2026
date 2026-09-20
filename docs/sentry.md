# Sentry operations

The dashboard uses `@sentry/react`; the Worker and `GameRoom` use
`@sentry/cloudflare`. Host firmware sends small diagnostic snapshots to the
existing authenticated gateway WebSocket. Only the backend sends those snapshots
to Sentry. There is no badge Sentry SDK, second Internet connection, mesh
diagnostic packet, or telemetry queue on the badge.

Registration is a badge action in this application. Its trace begins at the
backend's `/registrations` endpoint. Browser Start/Reset and HTTP state reads
carry `sentry-trace`, `baggage`, and an opaque `x-action-id`; pushed dashboard
state carries a trace continuation. Asynchronous host activity is correlated
using the round and boot IDs, rather than keeping a span open for an entire game.

## Configuration

For both the React/browser project and the Cloudflare/backend project, select
Error monitoring, Logging, and Tracing. Leave Session replay and Application
Metrics off. Replay is opt-in in the browser code: both sampling rates default
to zero, and the recorder integration is omitted unless a positive rate is
explicitly configured. Application Metrics is disabled in both SDKs.

Use a browser project and a Cloudflare project in the same Sentry organization.
Select both projects when looking for traces that cross the browser/backend
boundary. You can also use one JavaScript project and distinguish `component`
and `source` attributes. Match the environment and release names across both applications.

The integration is disabled when its DSN is absent. Never put a Sentry API/auth
token in a `VITE_` variable or a firmware header. A DSN is the project's public
ingestion address, not its management API credential.

| Location | Setting | Purpose |
| --- | --- | --- |
| Frontend build | `VITE_SENTRY_DSN` | Browser project's DSN; rebuild to change it. |
| Frontend build | `VITE_SENTRY_ENVIRONMENT` | Environment such as `staging` or `production`. |
| Frontend build | `VITE_SENTRY_RELEASE` | Release identifier, ideally the commit SHA. |
| Frontend build | `VITE_SENTRY_TRACES_SAMPLE_RATE` | Trace sampling, default `0.1`. |
| Frontend build | `VITE_SENTRY_REPLAY_SESSION_SAMPLE_RATE` | Normal session recording, default `0` (off). |
| Frontend build | `VITE_SENTRY_REPLAY_ON_ERROR_SAMPLE_RATE` | Replay buffer on errors, default `0` (off). |
| Frontend build | `VITE_API_BASE` | Game API URL; trace headers are restricted to this API. |
| Worker variable | `SENTRY_DSN` | Backend project's DSN; empty disables delivery. |
| Worker variable | `SENTRY_ENVIRONMENT` | Match the frontend environment. |
| Worker variable | `SENTRY_RELEASE` | Match the frontend release. |
| Worker variable | `SENTRY_TRACES_SAMPLE_RATE` | Trace sampling, default `0.1`. |

### Where to edit this application's values

- Browser production values are saved in [`frontend/.env.production`](../frontend/.env.production).
  Vite loads these for `npm run build`; rebuild the website after editing them.
- Backend production values are in the `vars` object of
  [`backend/wrangler.jsonc`](../backend/wrangler.jsonc). They take effect when
  you deploy the Worker.
- Both now use the supplied project DSNs, `production`, release
  `zombie-tag@9544527`, and trace sampling `0.1` (10%). Both browser Replay rates
  are `0`; metrics are off. Update both release values together for a new release.
- The browser DSN and these public settings are intentionally versioned so a
  merge carries the configuration. Keep private auth/upload credentials out of
  these files. No source-map auth token is required for errors, traces, or logs.
- For local browser development, use ignored `frontend/.env.local`; ordinary
  `npm run dev` does not load `.env.production`. For local Worker development,
  use ignored `backend/.dev.vars` with `SENTRY_DSN=""` to disable sending, or a
  separate development DSN and `SENTRY_ENVIRONMENT="development"`. The Worker
  otherwise inherits its production `vars`. Automated backend tests explicitly
  override the DSN to empty, so tests never send to the production project.

Use the existing private provisioning for `ZT_GATEWAY_TOKEN` and `ZT_HOST_MAC`.
No new badge credential is required. Keep local Worker values in ignored
`backend/.dev.vars` and browser build values in ignored `frontend/.env.local`.
The committed examples contain placeholders only.

For an operator-managed deployment, set `SENTRY_DSN` in the Worker variables and
supply the frontend variables when building the static assets. If you prefer to
store the DSN as a Worker secret, first remove the same-name entry from `vars`
and use Wrangler's interactive secret command. Build the frontend before
packaging the Worker because the Worker serves `frontend/dist`.
Configuration changes do not deploy themselves.

```sh
cd frontend
npm ci
npm run lint
npm test
npm run build
cd ../backend
npm ci
npm run typecheck
npm test
npx wrangler deploy --dry-run --outdir /tmp/zombie-tag-worker
cd ..
sh firmware/tests/run_gateway_diagnostics.sh
```

The dry run validates the bundle and does not deploy. The new application
verification workflow also performs no deployment. Merge, deployment, firmware
packaging, and flashing remain operator actions.

Optional frontend source-map upload is supported at build time. Set
`SENTRY_UPLOAD_SOURCEMAPS=true`, `SENTRY_ORG`, `SENTRY_PROJECT`,
`SENTRY_AUTH_TOKEN`, and `VITE_SENTRY_RELEASE` in the private build environment.
The build fails if an upload setting is missing, uploads hidden maps with the
matching release, and deletes the maps after upload. Ordinary builds produce no
public map files and perform no upload. Never expose the auth token to the
browser. Backend errors retain bundled code locations; a separate backend
source-map upload is optional and is not configured by this change.

## Demonstrating Replay, Tracing, and Logs

Use a disposable staging game. Temporarily set both trace rates and the normal
Replay session rate to `1`, rebuild the dashboard, and use the same staging
environment on both SDKs. Replay demonstration is optional; keep both Replay
rates at `0` if you do not want recording. Restore the normal rates after the demonstration.

1. Open the dashboard at its plain `/` URL, without a query string or fragment.
   Leave it open and interact with the page. In **Replays**, select the browser
   project and staging environment. The layout, timing, scrolling, and actions
   are visible; player content, text, inputs, and media are masked or blocked.
   Replay deliberately omits request/response bodies, credentials, and console
   contents. The normal recording limit is 15 minutes per replay.
2. Press A on two badges, including the host. In **Traces**, find registration
   processing. Click **Start Game** in the browser, wait for readiness and the
   scheduled start, and inspect the `round.start` trace. Follow the browser HTTP
   span into the Worker and Durable Object, then the pushed state update. Search
   the corresponding logs by `action_id`.
3. Infect a human badge. Inspect infection processing and state synchronization
   spans. Filter logs by the `round_id` to connect them to the earlier Start and
   later host health snapshots. Patient Zero and player names/MAC addresses are
   not telemetry attributes.
4. Leave the welcomed host idle for at least a minute. In **Logs**, search for
   `host.diagnostics` and `source:host`. Temporarily interrupt its network and reconnect it. A later
   snapshot shows cumulative connection/failure/drop counters, uptime, heap, and
   stack minima. Counters are retained across network reconnects during that
   boot; compare snapshots with the same `host_boot`. A reboot starts new counts.
5. To exercise browser error capture without changing game state, use the
   staging browser's developer console:

   ```js
   setTimeout(() => { throw new Error('Sentry staging smoke test'); }, 0);
   ```

   Find the redacted browser error in **Issues** and its Replay. The original
   message is intentionally removed; stack locations and component context
   remain. Network/API errors are also captured by the normal dashboard flows.

An offline badge cannot upload a snapshot until its existing connection recovers.
Heavy gameplay may defer diagnostics. A missing sample is not proof of failure,
and a transport drop count is not a count of lost durable infection events.
Backend spans containing only synchronous CPU/SQLite work may have zero measured
duration because the Workers clock advances around I/O.

Useful names to search:

| Feature | Names / attributes |
| --- | --- |
| Browser traces | `round.start`, `round.reset`, `state.sync.http`, `state.sync.websocket` |
| Backend spans | `game.registration`, `game.round.prepare`, `game.round.start`, `game.infection.process`, `game.state.sync` |
| Game logs | `game.registered`, `game.round_prepared`, `game.round_started`, `game.infections_processed`, `game.state_synced` |
| Connection logs | Browser `feed.connected`, `feed.disconnected`, `feed.reconnecting`; backend `connection.opened`, `connection.closed`, `connection.send_failed` |
| Correlation | `action_id` for an HTTP action; Sentry trace ID for its processing; `game_id`, `round_id`, `host_boot` for asynchronous host/game activity |

## Privacy and load controls

Telemetry uses fixed event names and allowlisted scalar attributes. It excludes
authorization/cookies, bodies, query strings, raw WebSocket messages, arbitrary
exception values, player names, badge MACs, Wi-Fi credentials, and mesh keys.
Error stacks retain code locations for debugging. Replay masks text and inputs,
blocks player regions/media, and excludes custom console/network recording
events. An initial URL containing query/hash data disables recording.

Browser logs are capped at 30/minute with a 5-second per-event cooldown; handled
failures have a 60-second cooldown and all browser errors share a 5/minute cap.
Backend game logs share a durable 30/minute/game budget, with 10-second
state/connection and 5-second rejection/failure cooldowns. Host diagnostics have
an independent budget so routine game logs cannot crowd them out. Backend
transport buffering is capped at eight envelopes and serialized traces at 100
child spans. Error capture has a 10/minute/client budget (DO instance lifetime;
the Worker creates its SDK client per invocation). Trace and Replay sampling are
independent. SDK delivery is asynchronous; gameplay never waits for a Sentry
network response. Worker/DO wrappers finish delivery through `waitUntil`.

Diagnostic snapshots are optional, negotiated by the backend, and sent only on
the welcomed host connection. Firmware sends at most once per 60 seconds and
uses at most 1,024 bytes, reusing existing buffers after pending game work. It
checks socket writability without waiting, then uses a 20 ms timeout per SDK
send operation (at most two 512-byte payload fragments). This is a bounded
best-effort write on the gateway task, below game/radio task priority, rather
than an asynchronous SDK send. It never retries a diagnostic or waits for an
application acknowledgment. Congestion can lose a sample; cumulative counters
remain for a later sample. Backend acceptance is limited to 1,536 bytes and one
sample per 30 seconds per game, including across socket reconnects/DO hibernation.
Invalid, excess, or unavailable diagnostics never become mesh traffic or
durable gameplay evidence.

## Verification performed

- Frontend TypeScript/production build, ESLint, and five privacy/rate-limit tests.
- Backend TypeScript and 21 tests in the current Cloudflare Workers test runtime,
  including actual SDK envelope redaction, diagnostic authentication/validation,
  no diagnostic reply/broadcast, persistent rate limiting, and game regression tests.
- Wrangler deployment dry run: bundle validated at 827.98 KiB / 174.38 KiB gzip;
  no deployment performed.
- Native C codec and diagnostic scheduler tests, including truncation canaries,
  negotiated compatibility, priority, congestion, pacing, and counter saturation.
- Full ESP-IDF 5.5.3 ESP32-C3 build with dummy private configuration: image
  `0x142190` bytes, 52% of the app partition free; one existing unused-function
  warning. This was a compile check, not a deployable provisioned release.
- Local browser + Worker + simulated host: registration, browser Start, readiness,
  infection, and game-over state completed. The local collector received Replay,
  errors, traces, and logs, with the same trace/action ID across browser, Worker,
  Durable Object, and pushed state. Compressed Replay segments were decoded;
  synthetic player names, MACs, credentials, error text, and injected query/baggage
  values were absent from the outgoing data.
- Valid, repeated, invalid, and oversized diagnostic frames used the same local
  host socket. Exactly one health log was emitted; rejects stayed silent and the
  next game clock exchange succeeded.

The initial implementation checks used a local collector only. No firmware was
flashed. Physical badge timing and reconnect behavior on a hotspot still require
the staging demonstration above.

## Production deployment — 2026-09-20

After explicit authorization, commit `ef133bf` was deployed to
`https://htn2026-backend.amitoj.workers.dev` with Cloudflare version
`43d0e066-d129-4a0d-804a-e3769ffb9639`. Both supplied DSNs are configured with
environment `production`, release `zombie-tag@9544527`, and 10% tracing. Session
Replay and Application Metrics remain disabled. Existing `ZT_GATEWAY_TOKEN` and
`ZT_HOST_MAC` secrets were preserved. This deployment did not merge the branch
or flash any badges.

Production checks returned HTTP 200 for health and population state. The empty
lobby was unchanged across deployment. The live dashboard loaded without an
unhandled browser error; Sentry accepted its session and structured-log
envelopes with HTTP 200, and no Replay envelopes were sent. Backend trace
delivery was exercised with a read-only sampled request, but ingestion in the
backend Sentry project was not independently inspected. Updated host firmware
must still be built with real provisioning and flashed before host-local
diagnostic samples can arrive.

## References

- [Sentry React setup](https://docs.sentry.io/platforms/javascript/guides/react/)
- [Sentry Cloudflare setup](https://docs.sentry.io/platforms/javascript/guides/cloudflare/)
- [Replay privacy](https://docs.sentry.io/platforms/javascript/guides/react/session-replay/privacy/)
- [Cloudflare Durable Object lifecycle](https://developers.cloudflare.com/durable-objects/api/state/)
