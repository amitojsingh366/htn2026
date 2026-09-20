# Outbreak Director: configuration and demo

The outbreak director is one feature for the Cloudflare and OpenAI sponsor
tracks. Cloudflare Workers execute the application and tools; the Cloudflare
Agents SDK owns durable memory and follow-up scheduling. The OpenAI Responses
API supplies the model responses and function-call choices. Existing game code
continues to own infection adjudication, roles, timing, scoring, and winners.

**Release status:** the director is integrated with firmware recovery and Sentry in
LIVEG012. See [the integrated release guide](integrated-release.md) for deployment,
validation, and flashing instructions. Automated tests use controlled API replies;
physical badge delivery still requires the operator to flash and verify the fleet.

## Architecture and boundaries

`OutbreakDirector` extends `Agent<Env, DirectorState>` and has the
`OUTBREAK_DIRECTOR` Durable Object binding. Its instance name is the existing
game room's Durable Object ID. The new `v2-outbreak-director` SQLite migration
adds this class without replacing the `GameRoom` migration.

The gateway reads canonical round state, role counts, result metadata, and the
latest 40 infection events. Badge names, credentials, and MAC addresses are
excluded from the model observation. Game changes notify the director through
internal RPC outside the gameplay response path. Preparing/starting a round,
accepted infection changes, a round result, and finalization can enqueue a
coalesced agent run. Ordinary unchanged ACKs do not call the model.

Memory is written with `setState`: up to 32 observations, 80 action records,
30 runs, and 10 recaps, plus persistent request counters and pending schedules.
This memory survives Worker restarts and browser closure through the SDK's
[SQLite-backed state](https://developers.cloudflare.com/agents/runtime/lifecycle/state/).
Follow-ups use the SDK's [scheduled callbacks](https://developers.cloudflare.com/agents/runtime/execution/schedule-tasks/),
not a browser timer. A new round cancels obsolete follow-ups.

The model can choose four tools:

| Tool | Server-side behavior |
| --- | --- |
| `read_game_state` | Reads fresh authoritative state and bounded infection evidence. |
| `send_announcement` | Queues a short `ANNOUNCE` in the existing authenticated host gateway, subject to current round, revision, host freshness, expiry, and rate checks. |
| `schedule_follow_up` | Schedules a new read in 15–120 seconds; at most two pending follow-ups. |
| `publish_recap` | Selects a headline and up to five accepted event IDs. Server code renders the factual recap from canonical counts, winner, time, and cited events. |

Tool intents are saved before execution. Announcement action IDs and gateway
commands commit in one transaction, so retries reuse the same command. Tools
recheck game freshness; stale model work cannot change gameplay. Interrupted
model requests are not automatically retried, and their reserved request budget
remains consumed. Provisional recaps are labelled and can be replaced by a final
recap when final evidence arrives.

The app exposes `GET /api/v1/games/{game_id}/director` for status, observations,
tools/results, follow-ups, recaps, response/request IDs, and token counts. This
endpoint cannot trigger a model run or execute a tool. The dashboard polls it
independently every five seconds and retains the last values with a stale-feed
notice during errors. There is no public agent connection, state mutation,
prompt, or tool-execution route.

## Configuration

`backend/wrangler.jsonc` contains only non-secret settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| `DIRECTOR_ENABLED` | `"true"` | Enabled for the integrated production release; set `"false"` to disable model calls. |
| `DIRECTOR_GAME_ID` | `"005a544d454d4f01"` | Only this game can trigger model calls. Match the provisioned badge game and dashboard API URL. |
| `OPENAI_MODEL` | `"gpt-4.1-mini"` | Configurable Responses API model; must support function calling. |
| `DIRECTOR_DAILY_REQUEST_LIMIT` | `"60"` | Requests per UTC day for this director; clamped to 1–200. |
| `DIRECTOR_ROUND_REQUEST_LIMIT` | `"18"` | Requests per round; clamped to 1–40. |
| `DIRECTOR_MAX_OUTPUT_TOKENS` | `"600"` | Output-token ceiling per API request; clamped to 128–1200. |

The default model supports the Responses API and function calling according to
the [OpenAI model documentation](https://developers.openai.com/api/docs/models/gpt-4.1-mini).
The app uses strict [function tools](https://developers.openai.com/api/docs/guides/function-calling)
and runs the selected functions itself. No Cloudflare Workers AI model binding
is needed.

Add `OPENAI_API_KEY` as a **Worker secret**, alongside the existing
`ZT_GATEWAY_TOKEN` and `ZT_HOST_MAC`. The operator can use Cloudflare's secrets
UI or, from `backend`, the interactive command below when preparing deployment:

```sh
npx wrangler secret put OPENAI_API_KEY
```

For local development with OpenAI, include `OPENAI_API_KEY` in the
`secrets.required` list of your private local Wrangler config, then place the key
only in ignored `backend/.dev.vars`. Wrangler filters local secret files to the
declared secret names; the production config keeps this key optional so gameplay
can deploy without AI. Missing keys leave the director disabled;
they do not prevent gameplay. Follow Cloudflare's [secret configuration guide](https://developers.cloudflare.com/workers/configuration/secrets/).
Never put the key in `wrangler.jsonc`, a `VITE_*` variable, dashboard storage,
badge provisioning, command text, or a screenshot. The browser receives only a
`configured` boolean. Provider error bodies and headers are not saved.

For another origin, build the dashboard with `VITE_API_BASE` pointing to its
`/api/v1/games/{game_id}` path. This URL is public configuration. The checked-in
dashboard default points to the existing production Worker; use an explicit
local/staging URL for local/staging verification.

## Limits and failure behavior

Each run uses at most four API requests and at most three tool calls, one per
response. The last request disables tools. API requests have a 12-second client
timeout and SDK retries disabled. Runs are separated by at least 15 seconds and
have a 90-second action-validity window. Request slots are persisted before
sending, including failed attempts. The initial context is capped at 24,000
characters; tool arguments at 2,048 characters. Token totals record usage from
received API replies, so they are not a provider billing ledger or dollar cap.

The director asks for a 30-second announcement TTL; the gateway independently
allows at most 60 seconds and never extends past the round deadline. It requires
a connected host heard within 25 seconds, at least 15 seconds between accepted
announcements, and no more than three outstanding broadcasts. Repeating the same
normalized text in one round is rejected, even under a new action ID. Text is normalized
to at most 96 printable ASCII characters without markup. Expired and ended-round
announcements stop replaying; critical gameplay commands remain eligible.

Badges enforce their own three-slot announcement bound, current round/clock
checks, 15-second display spacing, and the earlier of ten seconds or command
expiry. Mesh transmission uses cosmetic priority. Infection and tag feedback
take screen priority. `queued` means server custody; per-badge `applied` receipts
mean admission to each badge's display snapshot, not proof a player read it.
Delivery audit results settle to `applied` or `expired` once terminal; interrupted
announcement intents are reconciled against the gateway without resending.
Badge announcement deduplication is RAM-only for the current round/boot, so an
unacknowledged message can replay after reboot within its remaining TTL. It adds
no flash writes. See [the badge protocol](protocol.md) for the wire contract.

An API error marks the director degraded and waits for a new meaningful event.
An exhausted budget stops further calls. Host loss rejects new announcements;
existing messages expire instead of arriving late after reconnect. None of these
paths resets roles, cancels a tag, changes radio channel policy, or blocks local
ESP-NOW gameplay. Existing firmware reboot behavior is described separately in
[the live backend notes](live-backend.md).

## Verification before deployment

Run these checks from the repository root; they do not deploy or flash:

```sh
npm --prefix backend run typecheck
npm --prefix backend test
npm --prefix frontend run build
npm --prefix frontend run lint
sh firmware/tests/run-announcement-tests.sh
(cd backend && npx wrangler deploy --dry-run)
```

The Worker test pool is pinned by the lockfile and currently uses a workerd
runtime supporting compatibility dates through `2026-08-22`; its test-only date
override is explicit in `vitest.config.ts`. Production retains `2026-09-19` and
is separately checked with the installed Wrangler dry run and local boot. Tests
use a clearly synthetic API key and controlled responses, including persisted
memory across actual Durable Object eviction, budgeting, SDK schedules, API
failure, late results, and reset races.

The native C test exercises the real gateway decoder: accepted 96-character
messages, escaped printable text, invalid targets, infinite expiry, empty and
oversized strings, Unicode, and control characters. The full firmware built
successfully with ESP-IDF 5.5.3 using the installed Python 3.12 environment and
existing private build headers. The application was 1,319,024 bytes, leaving
52% of its application partition free. The only build warning was the existing
unused `self_install` function in `app_main.c`.

The new firmware must be installed on the host and receiving badges for the
completed ANNOUNCE path. Installation, merging, and deployment remain operator
tasks. Use the existing guarded badge workflow in [live-backend.md](live-backend.md),
not the generic full-flash commands printed by ESP-IDF.

### Verified in this branch

- Backend: 42 tests across four suites passed; TypeScript passed. API requests
  were mocked and labelled with synthetic response/request IDs.
- Frontend: production build and lint passed. Browser checks covered a 390px
  viewport, a stalled request, recovery, and retained state after HTTP 503 while
  the gameplay feed/controls remained independent.
- Worker: Wrangler production bundle dry run passed; a local Worker using the
  unchanged production compatibility date returned HTTP 200 for health and both
  configured-game and unrelated-game director status. The missing-key state was
  correctly disabled. No remote deployment ran.
- Firmware: native decoder tests and the full ESP-IDF build passed, as detailed
  above. Physical transmission/display has not been verified.
- Production dependency audit: no advisories reported by `npm audit --omit=dev`
  at implementation time.

## Live demo and evidence to capture

1. After reviewing and deploying the Worker, new Durable Object migration, and
   dashboard, confirm the game ID matches, the director is enabled, and the
   server key is configured. Install the reviewed firmware through the existing
   guarded workflow. Register at least two real badges, including the host, and
   start using the normal dashboard or host control.
2. Open **Outbreak Director**. Show its authoritative round ID, revision, counts,
   infection evidence, and retained observations. A prepared round may first
   schedule a follow-up because its start time is in the future. Model choices
   vary; silence is a valid choice and is not evidence of a completed tool demo.
3. Perform a real valid tag. In **OpenAI API activity**, capture a completed run,
   its configured model, nonempty OpenAI response ID, request ID when supplied,
   and actual returned input/output token counts. Correlate the time/model with
   the OpenAI project's usage view. A mock reply, queued schedule, or failed run
   alone does not prove actual OpenAI API usage.
4. When the model chooses `send_announcement`, expand its arguments/result and
   capture the action ID and gateway command sequence. Show the physical badge
   message and the subsequent per-badge acknowledgments. Demonstrate that tag
   feedback keeps priority. Record missing acknowledgments honestly.
5. When `schedule_follow_up` succeeds, capture its Cloudflare schedule ID and
   due time. Close the dashboard through that due time, then reopen it and show
   the follow-up run and saved observations/actions. Check the deployed
   `OUTBREAK_DIRECTOR` binding and Durable Object activity in Cloudflare to show
   that execution and persistence are hosted there. Browser closure must not be
   the trigger for the follow-up.
6. End the round through ordinary gameplay. When `publish_recap` runs, show the
   winner/counts, final or provisional label, and accepted evidence IDs. Compare
   those IDs against the recorded infection history. If synchronization changes
   the result from provisional to final, show the updated recap.
7. In a controlled test round, disconnect the host's Internet connection and
   confirm badges still perform local tags. Reconnect after the announcement
   TTL; verify expired messages do not appear as fresh actions. Use a staging
   configuration with no API key or a low request budget to verify the disabled
   and budget-exhausted states without changing gameplay rules.

For a portable evidence bundle, save the read-only `/director` JSON response
with the recording. Include timestamps and deployment identity. The Cloudflare
and OpenAI parts should be shown from the same real run, with secrets excluded.

## A concrete example of Codex assistance

Codex traced `ANNOUNCE` from the documented WSS command to the badge and found
that the protocol and display code already existed, but the gateway JSON decoder
and gateway-to-wire bridge had no ANNOUNCE cases. It implemented those missing
cases and added native boundary tests. During the same trace, it found that the
game and UI tasks independently enforce 15 seconds: a message arriving just
before the UI's gate opened was marked seen and permanently dropped. The UI now
retries that coalesced snapshot after a busy result. These changes were compiled
in the full ESP-IDF build; they have not yet been demonstrated on physical badges.
