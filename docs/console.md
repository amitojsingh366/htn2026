# USB console and operator commands

C01 implements the native USB Serial/JTAG parser and transport and the four CLI
commands below. GPIO18/19 and `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` are preserved.
Integration owns the request sink and all effects: the parser never opens storage,
writes flash, decides admission, changes gameplay state, or reboots the badge.
`reset-game-storage` is absent by instruction and remains separate R01 maintenance.

## Framing and ownership

The wire is UTF-8 JSON Lines, one compact object followed by LF. The object is at
most **2,048 bytes**, excluding LF; an optional CR before LF counts toward that
limit. Each host write, including its portion of the delimiter, is at most 192
bytes. Strings are bounded by decoded UTF-8 bytes, not character count. Malformed
UTF-8, embedded NUL, invalid Unicode escapes, duplicate/escaped/unknown request
keys, out-of-range integers, nested objects/arrays and trailing data are refused.
There are at most 40 object fields and 20 scalar elements per array. Numbers use
integer JSON notation; request integers are unsigned. Booleans are JSON booleans.

Every request begins with the unescaped `id` field, an unsigned 32-bit integer,
then `op`, then the operation's fields. MACs are full 12-digit lowercase hex;
game and round IDs are 16-digit lowercase hex strings. MACs must be nonzero
unicast addresses. UUIDs use lowercase, hyphenated 8-4-4-4-12 notation. Digests and
binary fields use lowercase hex. No secret values are present in these examples:

```json
{"id":1,"op":"info"}
{"id":2,"op":"status"}
{"id":3,"op":"button","button":"A","edge":"press"}
{"id":4,"op":"link_allowlist","macs":[]}
{"id":5,"op":"game_export","cursor":0}
```

Only one request may be outstanding. The sink must copy an accepted
`zt_console_request_t` before returning `ZT_OK`; the parser immediately wipes its
request and input buffers. A refusal returns a `zt_err_t` and must cause no later
reply or effect. Do not call `zt_console_receive()` recursively from the sink.
A sink may reply synchronously, or post bounded work and reply on completion.
It retains that completion while `zt_console_reply()` returns `ZT_ERR_BUSY`.
Request IDs belong to this connection; persistence IDs need a separate integration
mapping, so an operator ID cannot collide with a game persistence request.

Successful responses begin `{` and contain `id`, `ok:true`, and only the payload
fields specified below. Failure responses are **exactly**:

```json
{"id":1,"ok":false,"error":4}
```

`error` is the numeric frozen `zt_err_t`: 1 not implemented, 2 invalid argument,
3 invalid length, 4 invalid state, 5 unsupported version, 6 authentication,
7 not found, 8 busy, 9 no space, 10 timeout, 11 storage, 12 radio, 13 protocol,
14 stale, 15 conflict, 16 overflow, 17 network. Set `zt_console_response_t.id` and
`.result` consistently with JSON and `.len` to the JSON byte count without LF.
The reply API validates the envelope and copies the whole response into one
bounded output slot. It rejects unsolicited/mismatched replies, oversized output,
and extra fields in failures or successful configuration acknowledgments.

Overflow discards input through LF and reports error 16 with the first-field ID.
An invalid/missing first-field ID reports ID 0; this is a framing error, never
permission to perform an effect. Five seconds of inactivity wipes an incomplete
input transaction, reports error 10, and drains through the next LF. Under output
congestion a refusal may be shed; the host times out and closes, never retries a
write. The Python transport matches IDs, bounds every line including diagnostics,
and permanently closes its connection on timeout, malformed responses or I/O
failure. A lost acknowledgment is an uncertain operation, not permission to resend.

`zt_console_service(now_us)` is nonblocking and performs at most one 192-byte TX
and one 192-byte RX operation per call. Integration runs it regularly from the
console task, blocking/yielding between calls (well within the five-second input
timeout). Receive/service have one input owner; do not concurrently feed input
from another task or feed the same USB bytes through two producers. The public
receive entry point has the same 192-byte bound. Service returns busy while output
remains staged, and OK once staging is drained (driver transmission may still be
pending); input errors take precedence. No task is created by the component.
Initialize once, before admitting operator traffic.

Responses and `# ` diagnostics share one serialized USB writer. A partly emitted
line completes before another starts; responses take precedence over logs that
have not started. Logs are printable ASCII, at most 240 bytes of text, with one
pending log slot and at most 10 accepted lines per monotonic second. SDK vprintf
logging is suppressed when the console initializes: arbitrary SDK arguments are
not safe diagnostic input. Integration must route reviewed diagnostics through
`zt_console_log()` and must not write directly through printf, stdout, ROM-print,
or another USB writer while this protocol is active. Boot output before console
initialization is ignored by the host. No packet hexdumps or configuration dumps.

**Provisioning values are never echoed or logged**, including parser errors,
serial exceptions and private operation records. Python cannot guarantee erasure
of immutable strings from process memory; use organizer-controlled private inputs.

## Operations and integration responses

### `info`

No additional request fields. Read-only; available in `WAIT_INSTALL` as well as
configured and error states. The sink returns these flat fields:

| Field | Type / meaning |
|---|---|
| `firmware_version`, `build_id` | Version and build strings |
| `protocol_version` | Integer 1 |
| `mac` | Full physical factory STA MAC |
| `chip`, `chip_revision`, `flash_bytes` | `ESP32-C3`, major × 100 + minor, 4194304 |
| `layout` | `stock-v1` only after validating the actual four-entry stock table |
| `factory_offset`, `factory_bytes` | 65536, 2752512 |
| `install_offset`, `nvs_offset`, `nvs_bytes` | 4128768, 4132864, 61440 |
| `factory_sha256` | SHA-256 of **all 2752512 bytes** of the factory allocation |
| `install_state` | `WAIT_INSTALL`, `VALID`, or `ERROR` |
| `tail_blank` | True only after verifying the entire 64 KiB tail is FF |
| `header_hex` | Raw, read-back 80-byte installation header as 160 hex digits; null without a valid header |
| `config_status` | `absent`, `present`, `error`, or `unopened` |
| `active_round` | Round ID; 16 zero digits means lobby/no active round |
| `admission`, `pending_round` | Admission enum name without prefix; boolean pending-round flag |
| `role`, `last_error` | Numeric frozen role and error enums |

Use bounded raw reads and incremental SHA-256 over the complete padded factory
partition. An ELF digest, app-image digest, or SDK helper that hashes only the app
image is not this digest. Return a failure if the digest/identity/layout cannot be
established; do not fill unknown facts with successful-looking defaults. Report
storage errors explicitly without initializing unknown storage. No keys or config
values are returned. Reading the factory allocation and installation header for
this operation never authorizes reading participant stock storage.

### `install_init`

Required fields: `expected_mac`, `schema` (exactly 1), `installation_uuid`,
`baseline_sha256` (32 bytes). All-zero/all-FF UUIDs and baseline digests refuse.
The parser fills the fixed header magic, length, range, flags and zero reserved
fields. The storage encoder computes the CRC; do not copy C structure padding.

The sink compares the physical MAC, requires `WAIT_INSTALL` and verified blank
tail, and calls **`zt_store_install_init()`**. G01 initializes only the named
partition and writes the raw installation header last. Do not implement another
initializer or a format/retry fallback. On durable success, raw-read and verify
the header and establish absent configuration, then reply with `install_state`
`VALID`, `header_hex`, and `config_status` `absent`. No reboot before this response
and the tool's subsequent `info` readback. Integration must make configuration
available after installation, or document an ordinary operator reboot before
provisioning; the transport itself performs no reboot.

Wrong MAC/schema/UUID, an occupied tail, prior initialization, a storage error or
failed readback produces a fixed failure. An interrupted attempt is not retried
or erased automatically. The baseline hash links the immutable original archive,
not the padded factory image.

### `configure`

Required player fields: `expected_mac`, `name` (1–12 printable ASCII bytes),
`game_id` (nonzero), `group_key` (exactly 32 bytes, encoded as 64 hex digits;
all-zero refused), `host_mac`, `last_channel` (1–11),
`host_credentials_present` (boolean), and `provisioned_time_ms` (integer
0–9007199254740991). All fields fit the frozen `zt_config_t`.

A host additionally requires `https_base` (9–192 printable ASCII bytes, HTTPS
scheme and nonempty authority; whitespace, userinfo, fragments and backslashes
refused), `ssid` (1–32 UTF-8 bytes), `password` (0–63 UTF-8 bytes), and `token`
(1–256 printable ASCII bytes). The host's expected MAC must equal `host_mac`.
Players must have a different MAC and must omit all four host fields. A switch
position cannot confer credentials. Escaping may make an otherwise individually
valid set exceed 2048 wire bytes; that transaction refuses without truncation.

The integration sink rechecks physical MAC and admission **at effect time**:
initial `NEEDS_CONFIG`, or lobby admission with no active, retained/pending round
or outstanding gameplay transition. The tool permits the lobby substates
`LOBBY`, `REGISTERING`, `WAITING_FOR_ROUND`; the sink remains authoritative.
An active-game configuration is frozen. Submit the entire validated configuration
atomically, wait for its definitive durable completion, and then return **only**:

```text
{"id":<u32>,"ok":true,"config_sha256":"<64 lowercase hex digits>"}
```

The hash is SHA-256 over the following **622 bytes**, in order, matching the
schema-1 configuration field encoding, excluding blob magic/schema/length/CRC:

| Field | Bytes / encoding |
|---|---|
| expected MAC; name | 6 raw bytes; 13-byte zero-padded UTF-8 field |
| game ID; group key; host MAC | u64 little endian; 32 raw bytes; 6 raw bytes |
| last channel; host flag | u8; u8 (0 or 1) |
| HTTPS base; SSID; password; token | Zero-padded fields of 193, 33, 64, 257 bytes |
| provisioned time | u64 little endian |

All unused bytes are zero. Missing host fields are entirely zero. Never hash a
native C struct with padding. After the acknowledgment has been accepted by
`zt_console_reply()`, integration must drain the console output through
`zt_console_service()` and wait for USB transmission to finish before a normal
reboot: service until OK, then use the driver's bounded
`usb_serial_jtag_wait_tx_done()` and check success. Reply acceptance means queued,
not transmitted; the transport never
reboots. Return a failure on storage refusal and retain existing configuration.

### `status`

No extra request fields; read-only sanitized diagnostics, never stock storage.
Return the following u32 counters (unavailable owners must produce an explicit
failure, not invented samples): `rx_drops`, `tx_drops`, `invalid_frames`,
`auth_failures`, `dedupe_hits`, `rx_high_water`, `tx_high_water`,
`event_high_water`, `input_drops`, `tx_watchdogs`, `radio_restarts`,
`gateway_drops`, `gateway_reconnects`, `replay_backlog`, `free_heap`,
`minimum_heap`, `largest_free_block`, `clock_uncertainty_ms`, `gateway_age_ms`,
`channel`, `pending_event_count`. Also return `diagnostic_mode` (boolean),
`peer_ages_ms` (up to 20 u32 values), `pending_event_ids` (up to 20 event-ID strings
in round16/slot02/seq04 format), and `pending_ids_complete` (boolean).

Read radio-owned values from **`zt_radio_get_diagnostics()`**. Other values come
from their existing owners/heap APIs. Radio fields are a best-effort sample, not
an atomic instant. Fit the whole JSON response under 2048 bytes by shortening the
pending-ID page if necessary and setting `pending_ids_complete:false`; never
truncate JSON or imply that an omitted ID does not exist. Full round evidence is
available through the explicitly scoped export path.

### `button`

Required `button`: `A`, `B`, `HOME`, `DOWN`, `LEFT`, `RIGHT`, `UP`, `AUX1`, or
`START`; `edge`: `press` or `release`. The parser stamps monotonic time. No repeat
or role assignment is accepted. Integration fans the edge through ordinary
`zt_game_post_button()` and `zt_ui_post_button()` rules, handling bounded queue
backpressure without duplicating a partially delivered edge. Aux1 remains a
maintained switch governed by ordinary input rules; USB cannot select a host.
Reply `{"id":<u32>,"ok":true}` after acceptance; return busy/invalid state on
refusal. A successful debug press does not assert that a tag or registration won.

### `link_allowlist`

Required `macs`: zero to 20 distinct full MAC strings. Integration invokes
`zt_radio_link_allowlist()` and verifies its diagnostic readback. Reply with
`diagnostic_mode` (boolean) and `count` (integer). The radio drops disallowed
received frames **before protocol processing**, and the UI must visibly show
`DIAGNOSTIC` whenever the mode is active. Empty input restores ordinary reception.
No persistence, fabricated RSSI, or protocol-generated fake peers. Refuse if the
radio cannot apply the setting. This emulates link loss; **it is not a range
measurement**.

### `demo_round`

This is an explicitly marked local-round operation for demonstration, with a
persistent `DEMO MODE - LOCAL ROUND, NO SERVER` banner; firmware refuses it when
the badge is not the designated host with its host switch latched or when a real
round exists.

All six fields below are required in addition to `id` and `op`:

| Field | Validation |
|---|---|
| `round_id` | Exactly 16 lowercase hex digits, nonzero; use an unused round ID |
| `duration_ms` | Unsigned integer, exactly 0 or 600000 |
| `players` | Array of 2–20 distinct full 12-digit lowercase hex, nonzero unicast MACs; array index is roster slot |
| `patient_zero_slot` | Unsigned integer strictly less than the number of players |
| `channel` | Unsigned integer 1–11 |
| `start_delay_ms` | Unsigned integer 3000–30000, inclusive |

For example, replace the illustrative MACs below with the actual participating
badges' MACs (including the designated host), select an unused round ID, and send
this single object followed by LF, using host writes of at most 192 bytes:

```json
{"id":6,"op":"demo_round","round_id":"0000000000000001","duration_ms":600000,"players":["020000000001","020000000002","020000000003"],"patient_zero_slot":0,"channel":1,"start_delay_ms":10000}
```

The parser copies the complete validated roster and fields into `args.demo` and
posts `ZT_CONSOLE_DEMO_ROUND` through the existing sink. It never starts a round,
checks admission, assigns roles, clamps a value, supplies a missing field, or
accepts a partial roster. Zero duration is forwarded unchanged as the contract's
allowed default-duration sentinel. The sink owner invokes `zt_demo_start()` in the
game task's context; D01 stages the ordinary snapshot/command flow. No real server
authority is created, and the acknowledgment does not assert a completed START.

The response uses the existing envelope, with no additional success fields:

```json
{"id":6,"ok":true}
```

Invalid or missing fields, duplicate players, or any out-of-range value reject
the whole request before posting: `{"id":6,"ok":false,"error":2}`. Sink refusals
use the same shape with their frozen error code, including 6 for a badge that is
not the designated host, 4 for an ineligible admission state or retained
unfinalized round, and 7 when the host's MAC is absent from the roster. The normal
2048-byte input/response bounds and overflow handling remain unchanged. No tool
module or command is added by this parser change.

### `game_export`

Required `cursor` (0–128); optional nonzero `round_id`. Omission maps to round zero
in the typed request, meaning the sink must resolve the active round. If there is
no active round, return not found; never silently select an unknown previous one.
The sink calls `zt_store_export()` for the resolved retained round and requested
cursor with a 512-byte output capacity (at most eight canonical 64-byte durable
records), and reads produced/decided frontiers from its checkpoint. An explicitly
named unretained round refuses. No stock reads.

Each successful response contains `round_id`, `cursor`, `next_cursor`,
`records_hex` (0–1024 hex digits, multiple of 128), `produced` and `decided`
(exactly 20 u16 entries each), and these mandatory scope fields:

```json
{"scope":"single_round","retained_set_complete":false,"limitation":"PREVIOUS_ROUND_ENUMERATION_UNAVAILABLE"}
```

Those fields are part of the response object, not a nested object. At cursor 128
the round page sequence is complete. Other successful pages must advance the
cursor. The operator concatenates records in storage cursor order, verifies their
framing/round/CRC and unique event IDs, enforces the 128-record bound, requires
stable frontiers across pages, and hashes the binary concatenation. A changing
export refuses; it never creates a complete-retained-set receipt. Pages must be
sampled consistently by the sink; retained evidence/round clearance must not race
this export. The response is bounded even when discovery is unavailable.

### `archive_clear`

Required `round_id` (nonzero), `produced` (20 u16 values), and `export_sha256`
(32 bytes). The sink verifies this is an eligible **prior** round, that the receipt
matches the exact exported record bytes, and that exported and decided frontiers
cover all locally produced evidence. Do not rely on a hash alone to establish
final decisions. Serialize the check with game/persistence work so newly received
evidence cannot race clearance. Submit `ZT_PERSIST_ARCHIVE_CLEAR`, wait for durable
completion, then reply with `cleared_round` equal to the requested round.

Uncovered or stale frontiers, unresolved decisions, active-round clearance, hash
mismatch, missing rounds and storage failures refuse. Never clear configuration,
the installation header or stock partitions. There is no CLI clearance/reset
shortcut in this packet.

## Operator commands

Help requires no serial dependency, device, archive, or private configuration.
The entry point imports only `cli_console.register(subparsers)` at help time;
handlers import `console.py` lazily. No ports are opened merely by importing either
module. Commands hold R01's archive and hardware locks, check port holders, and
request exclusive serial access with DTR/RTS inactive. They use application USB,
not the ROM capture session. Agreement is confirmed locally with START released.

```text
install-init --recovery-id ID --port PORT --operation VERIFIED_FLASH_OPERATION_UUID [--release ABSOLUTE_MANIFEST_PATH]
provision --recovery-id ID --port PORT --config PRIVATE_FILE --name NAME [--host]
diagnose --recovery-id ID --port PORT
game-export --recovery-id ID --port PORT --output PRIVATE_LOCAL_FILE [--round ROUND_ID]
```

`install-init` rehashes original/current dual-read archives and both copies,
mirrored flash intent/result/expected/readback files, identity/security evidence,
original/current blank tails and supported layout. It requires the latest
`FLASH_VERIFIED`, `initialization_required:true`, an unused UUID and the current
tool revision/rehearsal. The optional `--release` supplies the absolute local
release-manifest path without prompting for it. When omitted, the same local
terminal prompt asks for that path, with no default. Both paths run identical
validation: relative paths are refused, and the release verifier rechecks artifacts
and matches the recorded release-manifest **hash**. A mismatch refuses with
`FLASH_RELEASE_MANIFEST_MISMATCH`, without prompting as a fallback or retrying.
The argument does not suppress any operator confirmation. The tool then compares
the app's full padded digest. The same uninterrupted USB connection supplies
physical MAC, layout and blank `WAIT_INSTALL` evidence, performs installation, and
reads back the exact header and absent config through another `info`. It records
and mirrors `INSTALL_INITIALIZED`. A durable intent consumes the UUID **before**
the send: lost acknowledgment, disconnect or interruption requires operator
recovery, never automatic reuse. Missing flash evidence cannot be improvised.

`provision` reads at most 16 KiB from a private regular file outside repositories
and shared/cloud paths (file mode 0600 or stricter, parent 0700 or stricter, no
symlinks/hard links). The file has exactly these fields: `game_id`, `group_key`,
`host_mac`, `last_channel`, `provisioned_time_ms`, `badges`, `host`. `badges` is a
1–20 entry array of objects with `mac` and boolean `host`, exactly one true and
matching `host_mac`; every badge must have a verified enrolled original on both
media. The selected badge must be listed and its designation must agree with
`--host`. `host` has exactly the four host provisioning fields listed above.
Do not put this file or real values in the repository, command line, or logs.
The player wire request omits host credentials even when the session file includes
them. The tool compares the hash-only acknowledgment to its canonical hash and
records/prints only that hash. A side switch alone cannot create another host.

`diagnose` checks physical identity/layout, reads `info`/`status`, and records only
whitelisted bounded counters, ages and pending IDs in the private operation's
`diagnostics.json`. Ordinary output is a short counter summary, without the
recovery reference or a config dump.

`game-export` writes a new private binary evidence file and an adjacent
`.receipt.json`, then records/mirrors the single-round receipt. Existing files
are never overwritten. The receipt identifies the device, round, produced/decided
frontiers, byte count and binary SHA-256; it explicitly says the retained set is
incomplete. Exporting does not send `archive_clear` or delete evidence. It is not
a receipt for resetting all game storage.

All new operation records live in R01's private archive. Errors use fixed condition
codes and never include private provisioning values. Adding these two modules
changes `zt_badge.revision()` and invalidates an earlier restore rehearsal as
intended; the orchestrator repeats the rehearsal under the final tool revision.

## Known limitations

Previous retained rounds cannot currently be enumerated through the frozen API;
exports cover only a named or active round and always disclose that limitation.
Integration must supply the operation effects and response fields above; the
current smoke-test `app_main.c` does not wire this console yet.

## Resource and review notes

C01 uses fixed input/request/parser storage, 15 bytes of serialized operation-name
scratch, one 2049-byte response slot, one 243-byte log slot and three static mutexes;
there are no per-request heap
allocations or recursive parser calls. USB driver initialization allocates bounded
2048-byte RX/TX buffers plus driver metadata. The reply parser has a fixed
40-field stack object; the configured console task stack remains 4096 bytes.
The orchestrator must include these pools in the global 64 KiB accounting, inspect
actual task stack high-water and USB draining before reboot, and run the complete
ESP-IDF build/link/size verification. No hardware behavior is claimed by syntax or
help checks. A source-only ESP-IDF 5.5.3 RISC-V GCC compilation using the existing
integration build flags measured 10,693 bytes of object code/constants and 6,130
bytes of BSS after adding `demo_round` (+676 and +15 bytes respectively). The
decoder's compiler-reported local stack frame remains 320 bytes, and the largest
local frame remains 912 bytes in the reply validator. These are component-object measurements, not linked firmware
size or whole-call-chain stack high-water measurements.
