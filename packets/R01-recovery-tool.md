# Packet R01 — guarded recovery, archive, and release tooling

## Goal

Implement `tools/badge_tool.py` and its supporting package: the **only** permitted path
by which any byte is ever written to a badge. It owns the private per-device archive,
the two-independent-read capture procedure, the second-durable-copy verification, the
atomic write gate, the full-image readback comparison, the restore and rehearsal paths,
the owner export/import bundle, and the release manifest that binds a built firmware
artifact to its source and configuration.

This tool is a safety device. Every guard in it exists because a participant is lending
a personal badge that holds their identity, wallet material, installed apps, and saves.
A gate that can be bypassed is not a gate. There is **no force flag**, no arbitrary
offset option, no auto-recovery that erases, and no path that writes without a fresh
verified capture.

## Base commit

Your worktree is already checked out at the exact base commit; run
`git rev-parse HEAD` in it to record the SHA. The orchestrator ledger
(`.orchestration/state.json`, outside your worktree) holds the authoritative
record. Branch: `firmware/R01-recovery-tool`.

## Dependencies already merged

None. This packet runs in parallel with F00 because its disk and CLI contract is fully
specified in `plan.md` §11 and §13 and does not depend on any firmware header.

## Branch and worktree

- Branch: `firmware/R01-recovery-tool`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/R01`

## Source of truth

Read, in the worktree root, **before writing any code**:

- `plan.md` — §11 in full (recovery promise and boundary, supported participants and
  storage, archive structure and manifest, capture procedure, stock-preserving layout,
  fresh capture before every write, atomic write gate, commissioning, restore and
  rehearsal, utility interface), §13 (the additional CLI surface and console
  operations), §3.1 and §3.2 (flash layout and the installation header), §2 (hardware
  boundary), §8 (release artifacts), §10 (known limits).
- `orchestrator.md` — §§1, 5, 9.

`plan.md` is authoritative. Where this packet restates it, the plan wins; report any
discrepancy you find rather than choosing one.

## Owned file allowlist

```
tools/badge_tool.py
tools/requirements.txt
tools/zt_badge/__init__.py
tools/zt_badge/errors.py
tools/zt_badge/config.py
tools/zt_badge/archive.py
tools/zt_badge/manifest.py
tools/zt_badge/index.py
tools/zt_badge/device.py
tools/zt_badge/layout.py
tools/zt_badge/gates.py
tools/zt_badge/release.py
tools/zt_badge/bundle.py
tools/zt_badge/locking.py
tools/zt_badge/cli_recovery.py
docs/recovery.md
```

**Explicitly forbidden paths:** `firmware/`, `backend/`, `frontend/`, `docs/protocol.md`,
`docs/backend-contract.md`, `docs/operations.md`, `docs/hardware.md`, `plan.md`,
`orchestrator.md`, `.gitignore`, `packets/`, `.orchestration/`, `.worktrees/`.
`docs/operations.md` belongs to packet F00, which is running at the same time as you.
Do not create or edit it; reference it by name.

You must **not** create `tools/zt_badge/console.py` or `tools/zt_badge/cli_console.py`.
Packet C01 owns those. See "Extension point" below.

## Architecture you must follow

`tools/badge_tool.py` is a thin entry point: argument parsing, subcommand dispatch,
exit codes, and sanitized status output. All real logic lives in `tools/zt_badge/`.

**Extension point (mandatory).** `badge_tool.py` builds its subcommand table by
importing `zt_badge.cli_recovery` (always) and `zt_badge.cli_console` (**optional —
import inside a `try`/`except ImportError` and continue silently when absent**). Each
module exposes `register(subparsers)`. A later packet adds `cli_console.py` without
editing any file you own. Write the extension point so that works, and document it in a
comment.

## Required behavior

### Environment and dependencies

`tools/requirements.txt` pins exactly, not as ranges: `esptool==5.4.0`. Add any other
dependency only if genuinely required, pinned to an exact version, with a one-line
justification comment. Target Python 3.12. Standard library preferred: use `sqlite3`,
`hashlib`, `json`, `uuid`, `os`, `pathlib`, `argparse`, `fcntl`.

Use esptool's **public Python API** (`import esptool`), not shell strings, not
`subprocess` against the `esptool` CLI, for the single-session capture/write/readback
path. Connect once, run the stub, call `attach_flash()`, retain the returned loader and
stub object, and perform the identity check, both reads, the write, and the readback on
that one connection without releasing the port. The read-only eFuse step may use the
`espefuse` CLI before the single session begins. Any disconnect invalidates the whole
operation and requires a fresh identity check and state capture; never auto-resume a
write. `plan.md` §11 "Exact esptool 5.4.0 command equivalents" lists the reviewable
equivalents of what your API calls must do — match their semantics, including
`--before no-reset --after no-reset`, `--flash-size 4MB` on reads, and
`--flash-mode keep --flash-freq keep --flash-size keep` on writes.

Never use `verify-flash --diff` or any equivalent that can print actual flash bytes into
a log or agent-visible output.

### Archive layout

Implement exactly the directory structure in `plan.md` §11 "Archive structure and
manifest", including `receipt.txt`, `original/` with its ten files,
`snapshots/<utc-time>-<uuid>/`, `operations/<uuid>/`, and `tool/`.

Durability rules, all mandatory:

- Stage every capture in a new `*.partial` directory; `fsync` each file **and** the
  directory metadata; then `rename` atomically to the final snapshot ID.
- Mirror through a separate partial directory on the second medium, `fsync`, then
  **reopen and re-hash the copied files** before publishing that copy atomically.
- Compute every hash from a **reopened** file after flushing, never from the in-memory
  buffer that was just written.
- Mark `BACKUP_VERIFIED` only after both full reads match byte-for-byte and by SHA-256
  **and** both durable copies verify. If the second medium is absent, full, or fails
  comparison, the capture stays `AWAITING_SECOND_COPY` and flashing stays blocked.
- Never open an existing snapshot file for truncation. Implement **no** deletion, no
  retention pruning, and no overwrite switch anywhere in the tool.
- The immutable `original/` snapshot is never modified after publication. Subsequent
  status belongs in separate operation records and index transactions. Never write a
  manifest's own hash into itself.
- Every failure creates its own private operation record with the exact failing
  condition.

`index.sqlite` maps recovery ID to device, but is **not** the only record: directory
scanning of signed-off manifests must be able to rebuild it. Gates re-hash the actual
selected files and never trust SQLite status alone.

Second-copy independence: resolve both paths, compare filesystem/device identity, and
reject a second directory, symlink, APFS clone, or snapshot on the same physical disk.
Record the operator's medium label, and state in the output that a filesystem ID alone
cannot prove independent hardware — the organizer's recorded label is the real evidence.

### Manifest

Record every field enumerated in `plan.md` §11: schema version; recovery ID; snapshot ID
and kind; UTC capture times; full factory MAC; chip/package/revision; JEDEC flash ID and
detected size; port and USB identifiers **as observations, explicitly never identity**;
esptool version; recovery-tool revision; read mode; offsets and lengths; both full-read
SHA-256 values; parsed partition metadata and hashes; eFuse and security evidence hashes
with read-protection masks; original-snapshot linkage; acquisition return codes; operator
label; and intended duplicate-copy location and medium label. Store paths relative to the
snapshot where possible. **No parsed wallet contents, passwords, or personal data.**

### Offline image parsing (`layout.py`)

Parse the captured image without touching the device: boot image header, partition-table
sector checksum and entries, factory app image header, all partition bounds and
non-overlap, and comparison against the supported stock layout of `plan.md` §11:

| Range (end exclusive) | Use |
|---|---|
| `0x000000..0x008000` | bootloader and leading area |
| `0x008000..0x009000` | partition-table sector |
| `0x009000..0x00D000` | stock `nvs`, type 1 / subtype 2 |
| `0x00D000..0x00E000` | stock `phy_init`, type 1 / subtype 1 |
| `0x00E000..0x010000` | gap |
| `0x010000..0x2B0000` | stock `factory`, type 0 / subtype 0, length `0x2A0000` |
| `0x2B0000..0x3F0000` | stock `storage`, type 1 / subtype `0x83`, length `0x140000` |
| `0x3F0000..0x400000` | 64 KiB tail |

Return three **independent** results, never collapsed into one boolean:
`READ_VERIFIED` (equal full reads plus verified copies), `STOCK_IMAGE_VALID`, and
`LAYOUT_SUPPORTED`. A corrupt current application or partition table must still be
capturable and restorable: the restore gate checks live identity and the selected
archived target, **not** current bootability or current layout.

### Gates (`gates.py`)

Each gate is a function returning a structured pass/fail with the exact failing
condition. The gate conditions are `plan.md` §11 "Atomic write gate and minimal
application flash" — implement all of them, checked under the same hardware and archive
lock immediately before writing:

- Connected full factory MAC, chip revision, flash identity and size, and security/eFuse
  state match the selected archive. The USB path is never used as identity.
- Original and fresh current snapshots each have matching independent reads and verified
  second copies, re-hashed from the actual files now.
- Stock partition table matches the supported layout. First installation requires the
  tail to be entirely `0xFF` **in both full snapshots**; a later installation requires a
  valid matching custom storage header. If custom firmware has run since the last
  accepted commissioning, protected stock regions must match that previous
  preflash/commissioning baseline. A legitimate intervening stock boot (including after
  the restore rehearsal) means take a **new preflash baseline** and record the
  transition — never replace `original`. An unexplained change while custom firmware was
  running stops rollout.
- The first-badge stock restore rehearsal has passed for this utility and workflow.
- The application header is valid ESP32-C3, compatible with the chip revision, built
  DIO / 80 MHz / 4 MiB, and fits `0x2A0000`. The release manifest pins source revision,
  SDK version, app hash, size, and reviewed configuration, and contains no unsupported
  security feature.
- The only permitted new-image write is at `0x10000`, and its entire erased sector range
  fits inside the factory partition.

`flash-game` writes an artifact padded with `0xFF` to exactly `0x2A0000`, so the
post-write comparison covers the whole application partition and no stale stock
application bytes survive inside it. Build the expected 4 MiB image by taking the fresh
current snapshot and replacing **only** `0x10000..0x2AFFFF`. Compare the complete
readback against it byte-for-byte and by SHA-256 **before any boot**. esptool's own
write MD5 check is not a substitute.

Write `intent.json` with operation UUID, identity, snapshot IDs and hashes, release
hash, exact permitted range, and status `PREPARED`; `fsync` it; set `WRITING` before the
write. A crash must leave an incomplete operation, never a fabricated success. There is
no reusable "approved badge" bit.

On mismatch: leave the device in the loader, record `WRITE_VERIFICATION_FAILED`, retain
both archives and the private readback, and report differing ranges and hashes **without
exposing contents**. Do not boot, retry indefinitely, widen the write, erase, or alter
security settings. On success record `FLASH_VERIFIED`, `fsync`, then permit a normal
reboot.

### Restore and rehearsal

`restore` resolves the selected snapshot, verifies both stored copies, takes the
hardware lock, identifies the same full MAC, checks security/eFuse state, captures and
duplicates current state, keeps the device in the loader, then writes the full image at
offset 0 with all flash-header options `keep`. No separate chip erase. Compare all
4,194,304 restored bytes and the SHA-256 against the selected image before any boot.
If comparison fails, remain in the loader and report failure — never claim restoration
because the write command exited zero.

`rehearse-restore` is the required first-badge operation: capture, verify both copies,
write that exact same original image back, complete the full preboot readback
comparison, and record the result. The already-fresh original capture may serve as the
current-state snapshot **while that loader session remains uninterrupted**.

Record `STOCK_BOOT_CONFIRMED` as a **separate operator observation**, not as a hash
comparison — flash legitimately changes once stock software resumes. The exact
restoration claim is tied to the verified preboot instant. Model these as distinct
recorded outcomes and never let one imply another: `READ_VERIFIED`, `BACKUP_VERIFIED`,
`FLASH_VERIFIED`, `RESTORE_VERIFIED`, `STOCK_BOOT_CONFIRMED`, `COMMISSIONED`.

### Commissioning

`commission` compares, against that operation's immediate preflash snapshot, the
protected ranges `0x000000..0x00FFFF` and `0x2B0000..0x3EFFFF` byte-for-byte, compares
the factory app against the padded release artifact, re-reads and compares readable
eFuse and security state, and permits change **only** in `0x3F0000..0x3FFFFF`. Record
`COMMISSIONED` only if all of that holds. If stock bytes changed, stop rollout, preserve
the evidence, and report the exact differing ranges.

### Release manifest

`release-manifest --build ABS_BUILD_DIR --output ABS_RELEASE_DIR` reads an ESP-IDF build
directory and emits the release artifacts of `plan.md` §8: app binary, ELF and map
references, build manifest, hashes of both the built app and the `0x2A0000`-padded
image, the exact permitted write range, protocol/API/schema versions, sanitized
sdkconfig, source commit, and tool/SDK versions. It must reject a build whose app does
not fit `0x2A0000` or whose image header is not ESP32-C3 / DIO / 80 MHz / 4 MiB. The
sanitized sdkconfig must have no secret values and must be checked for prohibited
security symbols (secure boot, flash encryption, anti-rollback, download disable).

### Owner bundle

`export` produces a verified **local** bundle — selected flash image, manifest, receipt,
tool revision and dependency lock, restore instructions — and **uploads or sends
nothing**. Omit readable eFuse key material unless explicitly requested; the complete
flash image and device-match information stay included.

`import-bundle` verifies the export manifest, full image size and hash, device identity
metadata, tool version, and per-file hashes, and **rejects path traversal, symlinks, and
duplicate files before importing anything**. It atomically recreates the device/archive
entry and a second verified copy on the newly configured independent medium. A recovery
ID colliding with a different MAC fails. An already-present MAC gains the verified
snapshot **without replacing `original`**. A missing optional private evidence file is
not a corrupt bundle; required files are enumerated in the export manifest.

### Locking and process hygiene

Acquire an archive process lock and a hardware lock before any serial operation.
**Never kill an unknown process holding the port.** Report it and stop. `ports` lists
candidate ports as observations and states plainly that a port name never identifies a
particular badge.

### Prohibited in this tool, without exception

No eFuse writes. No secure boot, flash encryption, anti-rollback, download-restriction,
or JTAG/USB-disable activation. No flash status-register or OTP writes. No chip erase,
`erase-flash`, or arbitrary `erase-region`. No arbitrary offset list. No force flag. No
`idf.py flash`. No merged full-flash release image as a deployment target. No writing
another badge's image to a device. No network upload of any archive, image, manifest, or
receipt. No deletion or pruning of archive content. No printing of flash bytes, eFuse key
material, tokens, passwords, or recovery IDs into shared output — all tool stdout and
stderr is captured privately and sanitized before any status is displayed.

The only region `reset-game-storage` may ever erase is `0x3F0000..0x3FFFFF`, and that
subcommand belongs to packet C01, not to you. Do not implement it.

### Subcommands you implement

```text
archive-init --archive ABS --mirror ABS --mirror-medium-label LABEL
ports
enroll --port PORT --owner-label LABEL
snapshot --recovery-id ID --port PORT --reason TEXT
verify --recovery-id ID --snapshot SNAPSHOT
receipt --recovery-id ID --output LOCAL_PATH
rehearse-restore --recovery-id ID --port PORT
flash-game --recovery-id ID --port PORT --release RELEASE_MANIFEST
commission --recovery-id ID --port PORT --release RELEASE_MANIFEST
restore --recovery-id ID --snapshot original --port PORT
export --recovery-id ID --snapshot original --destination OWNER_LOCAL_DIRECTORY
status --recovery-id ID
release-manifest --build ABS_BUILD_DIR --output ABS_RELEASE_DIR
import-bundle --source OWNER_BUNDLE_DIRECTORY
```

Archive roots live in organizer-local tool configuration, never in the repository.
`flash-game`, `restore`, and `rehearse-restore` automatically make or reuse an eligible
fresh capture; the operator cannot omit it.

**Do not implement** `install-init`, `provision`, `diagnose`, `game-export`, or
`reset-game-storage`. They require the USB console transport and belong to packet C01.

### `docs/recovery.md`

The operator-facing recovery document: what the archive guarantees and — equally
important — what it cannot recover (eFuses, flash status/OTP, RAM, external NFC tags,
account/website/blockchain state, physical damage); the enrollment and capture
procedure; the second-medium requirement and why a same-disk copy does not count; the
full subcommand reference including the C01 subcommands marked *provided by a later
packet*; the distinct result vocabulary above and what each one does and does not claim;
the failure playbook for each gate; and the owner bundle handover. State clearly that
participant agreement is obtained before any capture or write.

This document is R01's alone. `docs/operations.md` is F00's — reference it, never write
it.

## Allowed verification

No hardware. No serial port is opened. No device exists for you.

```sh
python3 -m py_compile tools/badge_tool.py tools/zt_badge/*.py
python3 tools/badge_tool.py --help
python3 tools/badge_tool.py flash-game --help
```

Import-time behavior must be side-effect free so `--help` works with no archive
configured and, ideally, without `esptool` installed — import `esptool` lazily inside
the functions that actually talk to a device. If you create a virtual environment to
check the import, create it **inside your worktree** and do not add it to the
allowlisted files.

Do not write a test suite, a mock device, a fake serial port, or a CI configuration.
`plan.md` D12 excludes them.

## Review criteria

1. No code path can write to a device without: a live identity match, a fresh verified
   dual-read capture, both verified durable copies, the layout gate, and the release
   gate — all checked under one lock immediately before the write.
2. No force flag, no arbitrary offset, no erase outside the one permitted case (which is
   C01's, not yours), no eFuse/security write anywhere in the tree.
3. Post-write and post-restore comparison is a complete 4 MiB byte-and-hash comparison
   against an explicitly constructed expected image, performed before any boot.
4. Capture publication is atomic and fsynced on both media, hashes are computed from
   reopened files, and `BACKUP_VERIFIED` requires both reads and both copies.
5. `READ_VERIFIED`, `STOCK_IMAGE_VALID`, `LAYOUT_SUPPORTED`, `BACKUP_VERIFIED`,
   `FLASH_VERIFIED`, `RESTORE_VERIFIED`, `STOCK_BOOT_CONFIRMED`, and `COMMISSIONED` are
   separate recorded outcomes, and no code treats one as implying another.
6. `import-bundle` rejects traversal, symlinks, duplicates, and ID/MAC collisions before
   any filesystem change, and never replaces an existing `original`.
7. The optional-import extension point for `cli_console` works when that module is
   absent.
8. `--help` succeeds for the top level and for every subcommand.
9. No secret, flash byte, or recovery ID reaches non-private output.
10. No file outside the allowlist changed, and nothing was committed.

## Constraints

- You do not run `git add`, `git commit`, `git merge`, `git push`, `git rebase`, or
  `git reset`. Leave your work uncommitted; the orchestrator reviews and commits.
- No hardware. No credentials. No backup access. No real archive is created or read.
- No network access is required or permitted for this tool's operation.

## Final report

```text
Status: READY_FOR_REVIEW | BLOCKED_CONTRACT | BLOCKED_ENVIRONMENT
Changed files:
Behavior implemented:
Build/syntax command and result:
Memory/size implications:
Known gaps and unfinished work:
Contract amendments requested:
Integration notes:
Hardware operations: NONE
```

If `plan.md` §11 or §13 specifies something that cannot be implemented correctly as
written, return `BLOCKED_CONTRACT` with the precise proposed change. Do not weaken a
gate, skip a verification step, or add a bypass to make a command usable. A gate you
cannot satisfy is a real issue for the orchestrator, not permission to relax it.
