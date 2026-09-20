# LIVEG012 integrated release

This release combines `codex/firmware-sync-recovery`, `codex/outbreak-director`,
and `codex/sentry-observability` on `main`. The user authorized the integration,
validation, production Cloudflare deployment, and a firmware build on 2026-09-20.
Older handoff restrictions about pending deployment and skipping tests are historical.

## Integration choices

- Firmware retains bounded retry bursts, actual badge receipt replay, nonblocking
  gateway command scans, late reset catch-up, registration-before-start, explicit
  control errors, and the shared five-second countdown.
- Outbreak retains durable agent memory/scheduling, guarded announcement tools,
  grounded recaps and the dashboard panel. Terminal/control commands take priority
  over cosmetic announcements; absent badges cannot pin gateway traffic.
- Sentry retains privacy filtering, bounded host diagnostics, trace correlation,
  and disabled Session Replay. Diagnostic serialization accepts the new firmware
  control errors through `ZT_ERR_MAX`. Both applications use `zombie-tag@LIVEG012`.
- The backend uses the newer Cloudflare Vitest plugin from the Sentry branch with
  the full director and telemetry suites. Tests use synthetic secrets and no
  production Sentry export. CI covers both native firmware wire-format suites.

## Production configuration

Target: https://htn2026-backend.amitoj.workers.dev/

The Worker already has `OPENAI_API_KEY`, `ZT_GATEWAY_TOKEN`, and `ZT_HOST_MAC`.
The director is enabled for game `005a544d454d4f01`, using `gpt-4.1-mini`, at most
60 requests per UTC day, 18 per round, and 600 output tokens per request. These
are request/token limits, not a dollar cap. Provider credentials are never part
of the dashboard or firmware. Sentry uses the existing public project DSNs;
no Sentry auth token is needed unless optional source-map upload is wanted.

The existing GameRoom SQLite migration is retained. The additive
`v2-outbreak-director` migration creates the director storage. Deployment does
not require resetting the game. Build frontend assets before deploying the Worker.

## Firmware and flashing

Build ESP32-C3 firmware with ESP-IDF 5.5.3 and the existing ignored private
headers. The displayed build ID is `LIVEG012`. Package from the clean merged
checkout using `tools/badge_dev_release.py --allow-dirty`; the manifest records
whether the checkout is actually clean and binds the application hash.

From the repository root, open the existing guarded operator menu:

```sh
.venv-badge/bin/python tools/badge_ops.py
```

Select each badge, then `2. flash`; the menu discovers the newest packaged
release. Flash **every badge**, including the host, for announcement support.
Use the existing guarded application-only workflow. Do not use ESP-IDF's
full-flash commands: the stock bootloader, partition table and recovery data
must remain intact. The build/release operation itself does not access hardware.

After flashing, boot the host with AUX1 on. Press A on each badge to register,
then B on the host or Start Game in the dashboard once at least two have joined.
Keep the fleet in range through preparation and the five-second countdown.
Verify infection syncing, late rejoin/reset catch-up, announcements, and host
health on physical badges. The automated/build checks do not establish RF,
heap, display, or timing behavior on the hardware. Pending unuploaded events
are intentionally lost on reboot under the retained fresh-boot policy.
