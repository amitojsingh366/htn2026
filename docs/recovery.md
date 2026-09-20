# Private badge recovery

Participant agreement is obtained **before any capture or write**. Only the organizer
operates this tool or handles private archives. Workers never receive badge images,
credentials, receipts, or real archive access. See `docs/operations.md` for the wider
build and event workflow; that document is maintained separately.

The supported write adapter is the HTN 2026 ESP32-C3 with 4 MiB flash, unsecured ROM
download, the exact four stock partitions, and the required security state. Each
operation rechecks the full factory MAC, package/revision, JEDEC ID, capacity,
readable eFuses and security information. USB paths, USB serial numbers, VID and PID
are observations, never identity. An unsupported but readable partition layout or
corrupt application can be captured. It cannot pass the custom application gate.
This adapter does not guess acquisition sizes or registers for other chips.

## What the archive promises

`original/` is the immutable, complete 4,194,304-byte state at enrollment. It includes
boot infrastructure, installed applications, identity and wallet material, saves,
stock NVS, PHY data, filesystem and unallocated flash. Two reads are independently
acquired while the application remains stopped; both files are retained. Another
full capture immediately before ordinary writes preserves later legitimate changes.
`flash-game` alone can automatically skip that capture for an eligible initialized
custom installation, under the gates and comparison rule described below.
Original is never replaced, including on repeated enrollment or owner-bundle import.
A recovery ID is a private lookup reference, not proof of ownership or permission to
disclose an image. Recover a lost code by enrolling the same physical badge again.

Recovery requires the same functioning physical badge, unchanged irreversible
configuration, readable flash, working ROM download, and retained verified copies.
It cannot restore eFuse bits or read-protected eFuse values, flash status registers
or OTP, RAM, external NFC tags, remote accounts/websites/blockchains, physical
damage, lost archive media, or a state that was never captured. It does not repair
corrupt captured bytes. A verified restoration to a damaged checkpoint restores
those exact damaged bytes. Firmware preservation still requires commissioning.

There is no upload, retrieval service, deletion, retention pruning, overwrite
switch, force option, arbitrary offset, chip erase, security activation, eFuse
write, status-register write, OTP write, or default `idf.py flash` path. Recovery
IDs and flash/eFuse contents never appear in ordinary status output. Private logs
must remain private; do not attach them to an agent or shared issue.

## Setup and enrollment

Use Python 3.12 and install the exact `tools/requirements.txt` dependency in an
organizer-controlled environment. It pins `esptool==5.4.0`; application code uses
its public Python API. Help and syntax checks require no dependency or archive.
The current medium inspector supports macOS (`diskutil`) and Linux (sysfs). Port
holder inspection requires `lsof`; an unavailable inspection blocks serial access.

Prepare two existing private directories, mode `0700`, on encrypted independent
durable devices. Both must be outside repositories, worktrees, shared projects,
cloud synchronization and worker-accessible storage. Files are created `0600`.
These are organizer operating requirements, not claims that path inspection can
establish encryption, ownership, or every possible synchronization service.

```text
python3 tools/badge_tool.py archive-init --archive ABS --mirror ABS --mirror-medium-label LABEL
python3 tools/badge_tool.py ports
python3 tools/badge_tool.py enroll --port PORT --owner-label LABEL
```

`archive-init` requires a local-terminal attestation about storage and records the
medium label. Resolved paths, filesystem device IDs and physical backing disks are
checked. A second directory, symlink, same-disk APFS volume/clone/snapshot, loop
volume or unknown backing does not count. APFS physical stores and Linux backing
block devices are compared. **Filesystem IDs alone cannot prove independent
hardware; the organizer's recorded medium label is the evidence of actual medium
selection.** A missing mount is never silently created as a local directory.
Unknown storage stacks fail closed.

Configuration is organizer-local: on macOS,
`~/Library/Application Support/ZombieTag/recovery-config.json`; on Linux,
`~/.local/share/zombie-tag/recovery-config.json`. Configuration is created
exclusively; there is no command-line root override or overwrite switch. Retain
and protect it. A replacement organizer machine uses its own new configuration.

Close serial monitors/browser IDEs. Obtain participant agreement and select one
badge. Hold START while connecting USB to enter ROM download. Keep USB power stable.
The tool takes archive and port locks, checks for existing holders without killing
them, acquires exclusive serial access, and never treats the selected path as the
badge's identity. Enter confirmations in the organizer's local terminal. Batch
runs cannot supply an observation flag in place of an actual observation.

A read-only `espefuse` subprocess acquires summary, protection masks and the joint
readable dump. The ensuing loader session re-reads the full eFuse register image
and compares its hash/mask to that evidence. It connects without reset, attaches
flash, retains the returned RAM stub, attaches flash again, identifies the badge,
and performs both full reads when a capture is required. The same connection remains
held through any gates, write and required readback. Reads explicitly set 4 MB; all write header parameters
are `keep`. The pinned loader's automatic write reconnect/retry is disabled.
A disconnect invalidates the operation; start a new command and fresh capture.
An already-running unknown stub is rejected; enter ROM afresh with START instead
of adopting RAM code whose origin this operation did not establish.
At a successful session end, `--after no-reset` semantics exit the stub to ROM.
Full-capture/write failures stop the session. An optional installation-header or
tail-read failure selects the ordinary capture path without reconnecting or retrying
the probe; that capture must succeed before any write. Neither path starts the
application automatically.

Each capture is staged in a unique `*.partial` directory. Files and directory
metadata are fsynced, reopened for hashing, then the directory is atomically
published. The mirror uses a separate partial directory, fsync, reopened hashes,
and atomic publication. A missing/full/mismatching second medium leaves
`AWAITING_SECOND_COPY` and blocks writing. Use `verify` when the configured medium
is available again. Interrupted/failed partial directories are retained as evidence;
they never become eligible snapshots simply because files exist.

## Private archive contract

```text
ARCHIVE/
  index.sqlite
  RECOVERY-ID/
    receipt.txt
    original/
      flash.bin
      flash-second-read.bin
      manifest.json
      efuses.json
      efuses-readable.bin
      security-info.txt
      flash-info.txt
      partition-table.bin
      partitions.json
      acquisition.log
      recovery-instructions.txt
    snapshots/UTC-UUID/             # same capture files
    operations/UUID/
      intent.json
      result.json
      post-write.bin
      commissioning.json
      ...append-only events and private evidence as applicable...
    tool/SOURCE-SHA256/
      badge_tool.py
      requirements.txt
      zt_badge/*.py
```

The plan's original directory contains **eleven** files; the packet's phrase “ten
files” is a counting discrepancy. The implementation follows the plan's names.
Not every operation creates every optional operation file. Incomplete operations
remain visible: `intent-prepared.json`, an atomically replaced `intent.json` with
`WRITING`, and numbered durable events distinguish a prepared or interrupted write
from a completed one. No success is inferred from a process exit or missing result.
Failure records are separate UUID operations. Failures before device identification
are recorded under root operations; invocation logs/failures live in the private
organizer-local `invocations/UUID` directory.

Manifests contain complete device identity, read provenance, UTC times, observations,
versions, parsed image/partition results and hashes, evidence hashes and masks,
original linkage, return codes, operator label and intended mirror location/label.
They contain no decoded wallet or filesystem data. A manifest never hashes itself.
A separate `BACKUP_VERIFIED` operation receipt signs off its manifest hash and file
hashes. This is a local recorded sign-off, not a cryptographic digital signature or
a claim of protection against an administrator maliciously rewriting all evidence.

The SQLite index is only a lookup cache. Directory scanning of signed-off original
manifests rebuilds it (`zt_badge.index.rebuild`); successful captures, imports and
`verify` invoke rebuilding. Gates re-open and re-hash actual files on both media,
check both independent read records, and compare bytes. They never trust an index
status. Snapshot file creation is exclusive; published snapshots have no mutation
path. Their private permissions are not filesystem cryptographic immutability.

Recovery references use 80 random bits encoded in 16 Crockford Base32 characters,
in four groups after `ZT-`. A two-character Base32 checksum suffix encodes the
80-bit integer modulo 37. Collision checks include existing directories as well as
the rebuilt lookup. The private receipt provides the reference, enrollment time,
masked MAC, original hash and same-badge return instructions.

## Results are separate claims

| Outcome | What it establishes | What it does not establish |
|---|---|---|
| `READ_VERIFIED` | Equal full independent reads and verified durable copies | Valid firmware, supported layout or successful boot |
| `STOCK_IMAGE_VALID` | Boot/app image checksums/digests and partition structure parsed successfully | Device identity, backup durability, provenance as an official stock release, or working saved data |
| `LAYOUT_SUPPORTED` | Exact expected four entries, flags, bounds and non-overlap | Blank tail, backup durability or app validity |
| `BACKUP_VERIFIED` | Both reads and both durable copies passed, with a separate sign-off | Permission for a future write without fresh checks |
| `FLASH_VERIFIED` | Ordinary path: complete 4 MiB preboot equality. Fast path: application and tail each match their recorded expectation by bytes and SHA-256 | Fast-path stock-region verification; successful boot, initialization or preservation during firmware use |
| `RESTORE_VERIFIED` | Entire selected 4 MiB image and SHA-256 matched before any boot | Successful stock boot or a postboot unchanging flash hash |
| `STOCK_BOOT_CONFIRMED` | Organizer/owner separately observed stock functions | A byte comparison; stock may legitimately change flash after boot |
| `COMMISSIONED` | Protected stock bytes and padded app matched the recorded baseline after an observed initialized role; readable security/eFuses matched | An unobserved role, venue performance, or permanent permission to flash |

Historical `status` output does not rerun gates. A current image may be
`READ_VERIFIED` with `STOCK_IMAGE_VALID=false` and `LAYOUT_SUPPORTED=false` and still
be restorable. The restore gate requires same-device identity/security and verified
selected/current archives, not current bootability or current layout.

## Rehearsal, release and installation

Before any custom deployment, run `rehearse-restore` on the first badge. It captures
fresh state, verifies both copies, writes that badge's exact original image at zero
without a separate erase, then compares every restored byte and SHA-256 before boot.
Only after `RESTORE_VERIFIED` does the local-terminal prompt permit an ordinary
power cycle with START released. Observe the stock launcher/display, buttons,
recognizable saved state and installed app availability with the owner. Do not dump
wallet material or invoke the known crashing stock `radio` command. Answer `y` to
the stock observation to record `STOCK_BOOT_CONFIRMED`, then separately answer `y`
to accept the rehearsal for this utility/workflow.
An interrupted observation does not invent acceptance; rerun the guarded rehearsal.
Tool source or dependency changes invalidate the prior rehearsal acceptance.
R03 changes `zt_badge/*.py`, which `zt_badge.revision()` hashes: the operator must
rehearse once more before the next flash. Historical results are never edited to
preserve acceptance.
Restoring a named non-original checkpoint reports preboot verification and permits
normal boot, but does not claim or prompt for stock boot. Return through a verified
original restoration and stock observation before a new stock-to-custom install.

Produce a release from a clean committed ESP-IDF v5.5.3 build:

```text
python3 tools/badge_tool.py release-manifest --build ABS_BUILD_DIR --output ABS_RELEASE_DIR
```

The output directory must be new and its parent must exist. The tool reads the
build's project description and generated sdkconfig JSON, checks source cleanliness
and artifact age, and requires an operator source/configuration review observation.
It checks the generated partition-table checksum and exact stock entries. It
copies app, ELF and map, writes a sanitized public sdkconfig subset and build
manifest, records source commit, tool/compiler/SDK and protocol/API/storage schema
versions, hashes every artifact, and creates the exactly `0x2A0000`-byte FF-padded
app. Neither bootloader nor partition-table binaries are deployment artifacts.
Security activation, anti-rollback and download restrictions are rejected; SDK
hardware-capability/default-selector symbols are distinguished from activation.
The app must be ESP32-C3, DIO/80 MHz/4 MiB and fit the stock factory allocation.
Its ESP-IDF descriptor must identify the pinned SDK, a zero security version, and
the same ELF SHA-256 as the released ELF.
Actual runtime write paths still require code review and commissioning; sdkconfig
alone is not proof of firmware behavior.

`flash-game` ordinarily captures fresh state and verifies original/current copies
under the retained locks/session. First installation requires a blank original and
current tail; later installations require a valid tool-installed or self-installed
header as described below. On the ordinary path, the current stock layout must match. Following
tool-installed custom use, protected bytes must match the accepted preflash baseline
and the previous installation must have been commissioned. A verified stock
restoration followed by observed stock boot records a new preservation baseline
transition on the next flash; original never changes. For self-installed badges,
the ordinary path compares protected stock bytes against original. Unexplained
changes stop the command.

Flashing alone accepts a self-installed development-fleet header when **all** of
these hold: an exact 4 KiB sector with `ZTIN` magic, schema 1, length 80, full
factory MAC matching the selected badge, zero reserved field, NVS offset
`0x3f1000` and length `0xf000`, flags exactly `0x00000003`, a correct CRC32 over
the first 76 bytes, and an erased remainder after the 80-byte header. Its UUID
must be all 16 zero bytes and its baseline hash all 32 zero bytes. Mixed markers,
unknown flag bits, corrupt or foreign headers are not accepted. Tool-installed
headers still require flags exactly `0x00000001`, the archive's original image
hash and a recorded tool-issued installation UUID, through the existing parser.

A self-installed badge carries **no proof that its storage was initialized against
a verified backup**. The remaining recovery protections are its own verified
`original/` on both media, full live identity/security checks, current-revision
rehearsal, the bounded application write and mandatory preboot verification of the
recorded ranges. Its operation
records `backup_binding_verified=false`, its zero header fields and a null
tool-issued `installation_uuid`; no tool-issued installation is invented.

Only a badge carrying that valid self-installed header is exempt from the previous
custom-install commissioning requirement, recorded with the reason
`VALID_SELF_INSTALLED_DEVELOPMENT_FLEET_HEADER`. These badges need neither a
tool-issued installation history nor a separate install step to reflash.
A development fleet whose first device has a `FLASH_VERIFIED` result recording
that self-installed-header exemption does not require first-badge player/host
commissioning before the remaining badges are flashed, including their first stock
flash, because commissioning is impossible for that first device. Every such
exemption records the first device, supporting operation UUID, result path and
SHA-256, and the recorded exemption; without that evidence, both roles remain
required for the current tool revision and same release hash, regardless of the
current badge's header.
**Commissioning itself still refuses self-installed headers**: it
uses the unchanged strict tool-issued parser and cannot grant them `COMMISSIONED`.

For custom-to-custom iteration, `flash-game` automatically considers skipping
the fresh dual full-flash capture and limiting readback to the application and tail.
There is no new command, flag or confirmation.
The decision is made inside the write gate on the retained exclusive connection.
All of the following must pass:

- The existing full identity comparison: factory MAC, chip, package/revision,
  JEDEC ID, capacity, readable security, eFuse hash and read-protection masks;
  the existing supported-security gate also passes.
- `original/` passes the existing backup gate, including both independent reads,
  sign-off and reopened verification on both independent durable media.
- The existing restore rehearsal gate passes for the current tool revision.
- One 4 KiB read at `ZT_INSTALL_OFFSET` (`0x3f0000`) passes the flashing-only
  header validation above, as either tool-installed or self-installed.
- Release verification and rollout checks pass. Tool-installed badges also pass
  the existing custom installation history, previous commissioning, verified
  baseline and first-badge role gates, with the recorded first-device development
  fleet exemption above where applicable. Self-installed exemptions are recorded.
- A complete 64 KiB tail read on the same stopped device succeeds immediately
  before the write, and its header sector exactly matches the eligibility read.
  There is no intervening device I/O, reset or boot before writing.

A missing, blank, corrupt, foreign or unreadable header, or any other failed fast
eligibility check, silently selects the ordinary capture path. It adds no refusal,
prompt or retry; the ordinary mandatory gates still apply and may stop an unsafe
flash. A first flash onto stock always captures the current state.

Before writing, `prewrite-capture.json` durably records the decision, session,
each passed condition, current rehearsal revision, observed header sector bytes
as hex, the original snapshot/image/manifest hashes establishing the recovery
archive, and the tail read's offset, length, hash and evidence path. The exact
tail bytes are retained as `prewrite-tail.bin`; this single bounded read is not a
dual full-flash backup. The intent and result also carry `prewrite_capture`:
on the fast path `skipped` is true and its `snapshot_id` is null. The legacy
`current_snapshot`/`current_hash` fields retain original for downstream baseline
consumers, explicitly labeled `current_snapshot_role=recovery_archive_reference_only`;
they claim neither a capture nor a live stock-region comparison. A failed probe
records its fallback condition; an ordinary write's intent/result identifies the
actual fresh capture instead.
`expectation_sources` identifies each range's source. `installation_header` and
`commissioning_exemptions` record the installation classification and any exemptions
on both the fast and ordinary flashing paths.

Only `0x10000..0x2affff` is written, with sector-aligned bounds. The ordinary path
is unchanged: it takes a fresh dual capture and compares the complete 4 MiB
(4,194,304-byte) preboot readback against that fresh snapshot with only the
application allocation replaced by the padded release, both byte-for-byte and
by SHA-256.

The fast path constructs no whole-image expectation and reads back only these
two ranges, comparing each independently by bytes and SHA-256 (inclusive addresses):

| Range | Expectation source |
|---|---|
| `0x010000..0x2affff` | Release's verified padded application |
| `0x3f0000..0x3fffff` | Same-session 64 KiB device read immediately before writing |

The stock regions `0x000000..0x00ffff` and `0x2b0000..0x3effff` are **neither written
nor re-read or re-verified on the fast path**. Stock firmware legitimately updates
its own filesystem after enrollment, including after a restore and stock boot;
differences from original there are not evidence of a failed application write.
The badge's own verified `original/` archive makes it recoverable, not a per-flash
re-read of regions the tool never writes. Verification of that archive on both
media remains mandatory. The tail contains the installation header and `zt_nvs`;
comparing it to the live prewrite bytes still catches changes during the write.

Fast-path intent/results explicitly record `verification_scope` with
`mode=application_and_tail`, `full_flash_verified=false`, and the stock ranges
marked `written=false`, `read_back=false`, `verified=false`.
`verification_ranges` names each checked range, expected hash and evidence file;
`range_verification` records both actual hashes and byte/hash equality. The same
comparisons are retained in `readback-verification.json`, including on mismatch.
`expected-application.bin`, `prewrite-tail.bin`, `post-write-application.bin` and
`post-write-tail.bin` hold the bounded evidence; fast operations create neither
`expected.bin` nor `post-write.bin`. The legacy whole-image `expected_hash` and
`preboot_sha256` fields are null, and `original_archive_preserved=true` refers only
to the immutable archive, not a comparison of stock bytes on the badge.

Esptool's write MD5 is supplementary. Any byte or hash mismatch in the required
readback ranges fails closed, retaining private expected/readback evidence,
absolute differing ranges and hashes, and leaving the badge in the loader.
No boot, widened write, erase or retry is attempted. Success records and fsyncs
`FLASH_VERIFIED`; the organizer may then perform a normal power cycle.

C01 supplies `install-init` and provisioning. The installation UUID is recorded in
the successful first stock-to-custom flash intent/result for that guarded
tool-issued continuation. Self-installed reflashes do not require this step. After
tool-installed firmware initializes in player mode, and separately after host radio initialization on the
first badge, return to the loader and run `commission`. It makes another complete
mirrored capture, compares `0..0xffff` and `0x2b0000..0x3effff` to the recorded
baseline (fresh preflash snapshot ordinarily, original for the fast path), compares
the padded app, and rechecks eFuses/security. Only the
64 KiB tail may change. The operator separately records the exercised role.
When the first device has no recorded self-installed-header exemption, other badges
cannot receive a release until it has both roles commissioned for that same release
and current tool revision. Each participant
still needs its own capture and commissioning. A protected-region commissioning failure blocks rollout of that
release; investigate and rebuild/review before another attempt. Restore remains
available through its own gates.

## Operator confirmations: y/n

Several commands stop and wait for a separate local-terminal answer to each
physical observation or attestation. Answer `y` to agree or `n` to refuse, then
press Enter. Only `y` and `yes`, case insensitive and with surrounding whitespace
stripped, agree. An empty answer (bare Enter), `Y E S`, an old sentinel, or any
other answer refuses with `OPERATOR_OBSERVATION_NOT_CONFIRMED`. The capital `N`
in ` [y/N]: ` makes the safe answer visible; empty input never means yes.

The tool reads `/dev/tty` first. If unavailable, it uses the preserved operator
output fd and requires interactive stdin. A piped, redirected or heredoc answer
is still refused as a source of confirmation; it cannot substitute for a human
answer at the terminal. Missing/unreadable terminal input or EOF refuses with
`LOCAL_OPERATOR_OBSERVATION_REQUIRED` or `OPERATOR_OBSERVATION_NOT_CONFIRMED`.
No flag, argument or environment variable supplies an answer. Every prompt reads
a fresh answer; stock observation and rehearsal acceptance remain separate.

The confirmation wording below is verbatim; each prompt ends with ` [y/N]: `,
including the trailing space:

| Command | Prompt |
|---|---|
| `archive-init` | `Confirm both existing private roots are outside repositories, shared/cloud folders and worker access, on encrypted independent durable hardware. The medium label records organizer evidence; filesystem IDs alone cannot prove independent hardware. Do you confirm? [y/N]: ` |
| `enroll`, `snapshot`, `rehearse-restore`, `flash-game`, `commission`, `restore` | `Do you confirm the participant agreed to this capture and any requested write, and the selected badge is in ROM download mode? [y/N]: ` |
| `rehearse-restore`, `restore --snapshot original` | `RESTORE_VERIFIED: normal power cycle is now permitted. Release START, then verify stock launcher/display, buttons, saved state and app availability with the owner. Do not dump wallet secrets. Have you observed all these stock functions with the owner after the normal power cycle? [y/N]: ` |
| `rehearse-restore` (separate acceptance) | `Do you confirm this first-badge restoration rehearsal passed for this tool and recovery workflow? [y/N]: ` |
| `commission` | `Do you confirm the badge completed provisioning, radio initialization and reached a working game screen before this capture? [y/N]: ` |
| `export` | `Confirm participant agreement and ownership of this local destination. The recovery code alone is insufficient. Do you confirm? [y/N]: ` |
| `release-manifest` | `Do you confirm this build was produced from the recorded clean source commit, and security/storage configuration and firmware write paths were reviewed? [y/N]: ` |
| `install-init`, `provision`, `diagnose`, `game-export` | `Do you confirm participant agreement and the selected badge is running the custom application with START released? [y/N]: ` |

Commissioning also asks `Record the exercised radio role: type p/player or h/host:`.
This is a separate choice: enter `p` or `player`, or `h` or `host`. Anything else
refuses. The recorded role remains `player` or `host`; run `commission` once per
role on the first badge. Confirmation answers do not rename any recorded outcome
or evidence field, including `STOCK_BOOT_CONFIRMED` and `rehearsal_accepted`.

Two consequences worth knowing before you are standing at a table with someone's
badge plugged in:

- **`flash-game` will refuse to run until `rehearse-restore` has been accepted**, and
  the acceptance is bound to the tool's source hash. If any tool source file changes
  afterwards, the rehearsal no longer counts and must be redone. This is intentional:
  a recovery path that was never exercised with *this* code is not a recovery path.
- **`restore --snapshot <checkpoint>` does not collect a stock-boot observation.** Only
  `--snapshot original` and `rehearse-restore` do, because only those restore a state
  whose correct stock behaviour you can actually recognise. The tool says so in its
  result line rather than silently recording less than you think.


## Command reference

All `--recovery-id` values come from the private receipt. Paths marked ABS or LOCAL
must be local filesystem paths; nothing is transmitted over a network. Output files
are created exclusively. Do not redirect private receipts or archives into shared logs.

| Command | Purpose |
|---|---|
| `archive-init --archive ABS --mirror ABS --mirror-medium-label LABEL` | Record independent organizer media; existing private roots required |
| `ports` | Enumerate observations without opening serial ports |
| `enroll --port PORT --owner-label LABEL` | Original capture, or a new snapshot for an already-known MAC |
| `snapshot --recovery-id ID --port PORT --reason TEXT` | Fresh complete independent reads and mirrored publication |
| `verify --recovery-id ID --snapshot SNAPSHOT` | Rehash both copies; complete pending second copy and rebuild lookup |
| `receipt --recovery-id ID --output LOCAL_PATH` | Copy the private recovery receipt without displaying its ID |
| `rehearse-restore --recovery-id ID --port PORT` | Required first-device original restoration and separate observations |
| `flash-game --recovery-id ID --port PORT --release RELEASE_MANIFEST` | Automatic capture selection, complete install gates, padded app write; fast readback covers app/tail, ordinary readback covers all 4 MiB |
| `commission --recovery-id ID --port PORT --release RELEASE_MANIFEST` | Protected-region/app/security verification after an observed initialized role |
| `restore --recovery-id ID --snapshot original --port PORT` | Capture current state, then exactly restore selected same-device checkpoint |
| `export --recovery-id ID --snapshot original --destination OWNER_LOCAL_DIRECTORY` | Verify and create a new private bundle under an existing local directory |
| `status --recovery-id ID` | Sanitized historical outcomes, not authorization |
| `release-manifest --build ABS_BUILD_DIR --output ABS_RELEASE_DIR` | Produce the bound, reviewed app release artifacts |
| `import-bundle --source OWNER_BUNDLE_DIRECTORY` | Validate historical evidence and recreate two durable copies |
| `install-init --recovery-id ID --port PORT --operation VERIFIED_FLASH_OPERATION_UUID` | **Provided by a later packet (C01)**; guarded WAIT_INSTALL continuation |
| `provision --recovery-id ID --port PORT --config PRIVATE_FILE --name NAME [--host]` | **Provided by C01**; bounded private USB configuration |
| `diagnose --recovery-id ID --port PORT` | **Provided by C01**; read-only status and explicit diagnostics |
| `game-export --recovery-id ID --port PORT --output PRIVATE_LOCAL_FILE` | **Provided by C01**; local game evidence export |
| `reset-game-storage --recovery-id ID --port PORT --export-receipt RECEIPT` | **Provided by C01**; fresh capture and guarded reset of only `0x3f0000..0x3fffff` |

R01 does not implement those five console commands. The entry point always imports
`zt_badge.cli_recovery`, optionally imports `zt_badge.cli_console` inside
`try/except ImportError`, and calls each module's `register(subparsers)`. Console
parsers set `func` to a handler accepting an argparse Namespace and returning a
sanitized status. C01 must use the archive/hardware lock and capture interfaces,
verify durable evidence afresh, and maintain its own bounded console-write guards;
adding a module requires no edit to R01's command table.

## Failure playbook

| Failed condition | Action |
|---|---|
| Participant agreement/ownership/observation missing | Stop; obtain and record the real observation in the local terminal. No unattended substitute. |
| Port occupied, lock held, lsof unavailable | Close the known monitor normally or resolve the holder with its owner. Never kill an unknown holder. |
| Unsupported chip/size/security, changed MAC/package/revision/JEDEC/eFuse/masks | Stop; check the physical badge and original evidence. Never rewrite identity or activate security to make a comparison pass. |
| Independent reads differ, short read, disconnect | Retain evidence; stabilize power/connection and start a wholly fresh session. No choice of a preferred read and no write resume. |
| Mirror missing/full, same physical disk, unknown backing, hash mismatch | Keep primary/partials and `AWAITING_SECOND_COPY`; restore the configured independent medium and use `verify`. Never count a same-disk copy. |
| Capture/sign-off/manifest/file hash invalid | Stop and preserve both versions; inspect private operation records. Do not fabricate or edit a sign-off. |
| Unsupported table or invalid current app | Archive if readable. Custom flashing stays blocked by its layout gate; a separately verified full restore can still proceed. |
| Original/current first-install tail occupied | Preserve it. Do not erase it to make installation fit. Ask the orchestrator to review a new adapter or use stock. |
| Invalid later header/unknown tool-issued UUID/non-FF reserved bytes | Keep storage untouched. Only the precisely marked zero-UUID/zero-baseline self-installed header is an exception for flashing; commissioning still refuses it. Use a deliberate guarded restore when appropriate. |
| Rehearsal absent/obsolete or first-device roles uncommissioned | Perform the required guarded rehearsal/observations or first-device role commissioning for this tool/release. |
| Release oversized, wrong chip/revision/mode/frequency/size, prohibited config, dirty/stale source | Rebuild from the reviewed pinned configuration and clean source. Never patch image headers or widen offsets. |
| Custom-use stock bytes changed or commissioning fails | Stop rollout; preserve exact differing ranges/hashes, inspect storage/PHY/NVS paths, and use guarded original restore when appropriate. |
| Preboot readback mismatch/write exception | Leave loader state, retain expected/readback and failure record. Do not reboot, retry in place, erase, or claim restoration. A new guarded restore needs fresh capture. |
| Bundle traversal/symlink/duplicate/ID-MAC collision/version/hash error | Import nothing into a device entry; retain the original bundle and investigate. Missing explicitly optional private evidence is allowed. |
| Private record storage itself cannot be written | Stop before serial work; repair private storage. No stdout fallback containing private evidence. |

## Owner handover and import

Establish ownership independently of the recovery reference, then export to the
owner's local directory. The bundle includes both original independent flash reads,
source manifest, backup sign-off, receipt, restore instructions, exact tool sources,
revision and dependency lock. Readable eFuse key bytes, full eFuse summary and raw
acquisition log are omitted; their recorded hashes/masks and device-match facts
remain. Flash itself is sensitive even with eFuse bytes omitted. Keep the bundle
private and encrypted. This implementation offers no eFuse-key export option.

The self-contained export manifest enumerates required files and hashes. Import
rejects path traversal, symlinks, hard links, special files, duplicate JSON/file
names, unlisted files, incompatible tools, malformed identity, wrong image size,
hash failures and recovery-ID/MAC collisions before creating a device entry.
Missing optional private eFuse evidence does not invalidate it. Full same-version
live readable-eFuse hashing is still required during subsequent restoration.

A new device entry requires the owner's original bundle; import that before later
checkpoints. A known MAC receives a new verified snapshot without replacing original,
even if its imported recovery reference differs. Imported source evidence remains
linked under unique names. Publishing/importing historical reads never makes them a
fresh device capture. Both durable copies must verify before ordinary restore can
proceed, and restore still captures current state on that physical badge first.

No hardware, real archive, credentials, firmware build or restoration was exercised
while implementing R01. Syntax/help checks are not evidence of hardware recovery.
The orchestrator must perform the first-badge rehearsal and commissioning before
participant rollout.
