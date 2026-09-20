# Badge operator CLI

`tools/badge_ops.py` is a private address book and a plain numbered menu over
`tools/badge_tool.py`. Use Python 3.12. The wrapper uses the standard library plus
configuration/index lookup and the console transport from the existing recovery
package. It installs nothing and makes no network calls. The guarded tool retains its existing
runtime dependencies and organizer setup; see [Private badge recovery](recovery.md).

**This CLI adds no capability and removes no gate.** A nickname is a label, never
authorization. Recovery and provisioning run the existing guarded tool as a separate
process, sequentially, with inherited stdin, stdout and stderr. Local `demo` gameplay
instead calls the existing `zt_badge.console.Console` transport directly, as described
below. The wrapper does not open serial ports itself, capture images, implement gates, flash, restore bytes,
inspect image contents, or supply answers to the guarded tool's observation prompts.
Only the operator makes and records those observations.

## Start here: one interactive session

Run `python3 tools/badge_ops.py` once. Pick a badge by number, then pick an action.
Registered badges appear first with nickname and masked recovery ID, followed by
verified unnamed archives under their own heading. The action menu follows the
working sequence: enrol, flash, install, provision, diagnose, demo, commission, restore,
rehearse, verify, export, export-game and status; listing utilities follow, with
label commands last. The selected badge stays selected after an operation. Use `b`
to choose another badge; use `a` in the badge picker to reach actions such as enrol
without selecting a badge first. `q` or EOF quits, and invalid numbers re-prompt.

Choosing an unnamed archive offers adoption before continuing. Declining returns
to actions. Accepting requires a new nickname **and the exact full recovery ID**;
the typed ID must match the selected archive, not merely another valid archive.
Existing nicknames are selected by number, not retyped for lookup. New labels for
enrol/adopt/rename still need text. If no badge is available, the menu says so and
points to enrol. A selection is never authorization: the nickname and verified
archive lookup are checked again before an operation. A changed mapping is refused.

For restore, choose a snapshot by number: `1. original` is the default and means the
immutable enrolment state. Later published checkpoints are listed from the selected
archive. Restoring a later checkpoint restores exactly its saved bytes, damage
included. Snapshot listing reads directory metadata only; the guarded tool still
checks the chosen snapshot. Verify and owner export use the same snapshot picker.

Menu flash selects the latest release automatically, showing its creation time and
short source commit; it never asks for a release path. A missing/stale release
offers numbered Generate/Cancel choices. Commission lets the operator pick a local
release by number. The direct commands remain available for explicit paths.

For example, with a registered `badge-1` and a current release:

```text
python3 tools/badge_ops.py
  [choose badge-1's number in the badge list]
  [choose 2, flash, in the action list]
  [review the automatically selected release]
  [type the port number, or press Enter for a sole candidate]
  Type the nickname to proceed: badge-1
  [the guarded tool now asks its first confirmation; answer it yourself]
```

The typed nickname is still required before a write, after numbered selection.
It is a selection check, separate from every guarded observation. A successful
flash prints guidance to power-cycle with START released, then choose install and
provision; none of those actions happens automatically.

## Private registry

The file is `zt_badge.config.local_dir() / "badge-names.json"`, beside the existing
`recovery-config.json`:

- macOS: `~/Library/Application Support/ZombieTag/badge-names.json`
- Linux: `~/.local/share/zombie-tag/badge-names.json`

The wrapper never creates or modifies `recovery-config.json`. Help and an
unconfigured `list` create nothing. The registry is created on successful enrolment
or adoption and replaced atomically through a mode-`0600` temporary file in the same directory,
with file and directory fsync. Its organizer-local parent must be private (`0700`)
and owned by the operator. Symlinks, repository/worktree locations and recognizable
cloud-storage paths are refused. Keep this directory outside **all** synced folders;
path checks cannot discover every custom synchronization arrangement. Registry
writes also refuse a location inside either configured archive root.

Its entire schema is:

```json
{
  "schema": 1,
  "badges": {
    "badge-1": {
      "recovery_id": "ZT-AAAA-BBBB-CCCC-DDDD-99",
      "added_utc": "2026-09-19T12:00:00Z"
    }
  },
  "operator_defaults": {
    "provision_config": "/private/operator/session.json"
  }
}
```

That ID and configuration path are fictional placeholders. `operator_defaults` is
optional; existing schema-1 registries without it remain valid. It remembers only
the last configuration path passed to `provision`, at operator scope, never in a
badge entry. The registry contains no MAC, port, image path, participant contact
information, credential value, configuration contents, token or outcome cache. There
is no badge-count limit. Use one operator CLI at a time; atomic replacement protects
file completeness, but is not a multi-process address-book transaction protocol.

Nicknames are case-folded to lowercase, contain 1–24 ASCII characters matching
`[a-z0-9][a-z0-9-]*`, and cannot be `list`, `help`, `quit` or `all`. Each nickname
has exactly one recovery ID, and each ID has at most one nickname. Names such as
`alex`, `badge-1` and `red` work. Enrolment uses the normalized nickname as the
guarded tool's `--owner-label`; there is no separate participant-information field.

Before any named recovery operation, the wrapper reads the registry again and
requires the ID to appear in `index.scan(config.load()["archive"])`. That scan only
returns originals with a signed-off `BACKUP_VERIFIED` receipt. Missing/unverified
archives are refused with an instruction to run `enrol`; lookup never silently
creates a label. The guarded tool still performs all of its own verification and
live identity checks.

## Invocation and commands

```text
python3 tools/badge_ops.py
python3 tools/badge_ops.py --help
python3 tools/badge_ops.py COMMAND --help
```

No arguments opens the badge picker and numbered action menu described above.
`q` at menu/input prompts or
EOF quits; invalid menu choices re-prompt. The nickname confirmation compares the
literal lowercase nickname. Ctrl-C aborts a selection; during a child operation,
the wrapper waits for the child to stop before returning to the menu. Registry
updates are complete or unchanged, never a partially written JSON document.

| Command | Behavior |
|---|---|
| `list [--show-id]` | Show all labels, verified archive visibility, and the guarded tool's recorded outcomes. Also show verified archives without labels. No configuration means a clean exit with visibility/outcomes unavailable. |
| `enrol NICK [--port PORT]` | Scan verified IDs, run `enroll --port PORT --owner-label NICK`, re-scan, and bind exactly one newly appearing ID after child success. Zero or multiple new IDs, or any child failure, bind nothing. `enroll` is an alias. |
| `adopt NICK --recovery-id ID` | Bind an existing verified archive to an unused nickname using its exact full recovery ID. Registry edit only; no guarded-tool subprocess. |
| `flash NICK [--release PATH] [--port PORT]` | Run guarded `flash-game`. An explicit reviewed manifest PATH wins; otherwise select the newest local release, offering generation when missing or stale. |
| `install NICK [--port PORT]` | Run `install-init` with the automatically resolved UUID of this badge's most recent successful flash. No manual operation UUID required. |
| `provision NICK --config PATH --name NAME [--host] [--port PORT]` | Pass through to guarded provisioning; remember only the config path as an operator prompt default. |
| `diagnose NICK [--port PORT]` | Run guarded device diagnostics. |
| `demo NICK [--port PORT] [--round-id ID] [--channel 6] [--start-delay 10000]` | Select registered players and patient zero by number, then send one local `demo_round` request through the existing console transport. NICK selects the designated host. |
| `export-game NICK --output FILE [--round ROUND] [--port PORT]` | Run guarded `game-export`; its required output file and optional round are passed through. The menu exports the active round. |
| `releases` | Read-only listing of local releases: directory name, `created_utc`, short `source_commit`, and the automatic selection. No badge, registry or guarded-tool operation. |
| `restore NICK [--snapshot original] [--port PORT]` | Run `restore --recovery-id ID --snapshot SNAPSHOT --port PORT`. Defaults to the original snapshot; a named checkpoint can also be supplied. |
| `rehearse NICK [--port PORT]` | Run `rehearse-restore --recovery-id ID --port PORT`. This writes the original and requires the tool's physical observations. |
| `verify NICK [--snapshot original]` | Run `verify --recovery-id ID --snapshot SNAPSHOT` without hardware. This is the guarded way to complete a pending second copy when the nickname's original already passes verified lookup. |
| `status NICK [--show-id]` | Run `status --recovery-id ID`; report historical outcomes without granting permission to write. |
| `export NICK --to DIR [--snapshot original]` | Run `export --recovery-id ID --snapshot SNAPSHOT --destination DIR`; the guarded tool checks ownership and creates the local bundle. |
| `commission NICK --release PATH [--port PORT]` | Run `commission --recovery-id ID --port PORT --release PATH`; the operator makes the tool's required observations. |
| `ports` | Run `badge_tool.py ports`, echo its observations and number recognized candidates. |
| `rename OLD NEW` | Change only the label, preserving the recovery ID and original added time. NEW must be unused. |
| `forget NICK` | Remove only the label. The archive, its images and its receipts are untouched, and the badge is still recoverable by recovery ID. |

Use `adopt` to name an archive captured before this CLI existed, or to label an
archive again after `forget`, without re-enrolling the badge:

```text
python3 tools/badge_ops.py adopt badge-1 --recovery-id ZT-AAAA-BBBB-CCCC-DDDD-99
```

The example ID is fictional. Adoption requires the full ID to match
`ZT-(?:[A-Z0-9]{4}-){4}[A-Z0-9]{2}` exactly and appear in
`index.scan(config.load()["archive"])`; there is no case normalization, prefix or
fuzzy matching, masked-ID acceptance, or binding by list index alone. Existing nickname
rules are unchanged. An occupied nickname is refused; an ID already bound to another
nickname is refused with that nickname named. A successful adoption records the same
`recovery_id` and `added_utc` fields as enrolment and prints only a masked confirmation.
**Adopting is a label operation that reads no badge and changes no archive.** It
reads verified archive metadata for lookup and writes only the private registry.

`list` offers adoption beside its unnamed verified archives, and the menu includes
`adopt`. Obtain the full reference privately from the receipt or an explicit
`list --show-id`; the menu still requires you to type the exact full ID. A masked
candidate can be selected by number, but adoption also checks the typed full ID.

Hardware commands display `badge_tool.py ports` observations and number the candidate
paths: type a number, or press Enter to accept the sole candidate when there is only
one. Choose `m` to enter another path; invalid numbers re-prompt, and `q` or EOF quits.
An explicit `--port` wins without a selection prompt. If discovery fails, returns no
candidates or has an unexpected format, the picker falls back to a typed path without
guessing or retrying discovery. These rules apply to both direct commands and the
menu, including `demo`.

Only the fixed, prompt-free `badge_tool.py ports` invocation captures stdout (and
echoes it in full) for numbering, with stdin and stderr inherited; every other
`badge_tool.py` invocation inherits stdin, stdout and stderr so all prompts,
confirmations and diagnostics reach the operator.

A port name never identifies a badge. Identity is the archived full MAC, rechecked
by the guarded tool on each connection. The port is never stored in the registry.
`adopt`, `verify`, `status`, `list`, `releases` and `export` do not request a port.

For `demo`, the port is likewise an observation; the single gameplay request does
not include a separate identity probe. Firmware enforces the designated-host and
host-in-roster requirements. Connect the selected host, with its host switch latched.

Before `flash`, `restore`, `rehearse`, `install` or `provision`, the wrapper prints the nickname, masked ID
and operation (plus the release path for `flash`), and requires interactive stdin
and the typed nickname. This catches a selection typo; it never substitutes for
the guarded tool's own confirmations. The tool may refuse after this check.

## Start a local demo round

A demo round is an explicitly marked local round, is refused unless the badge is
the designated host with its host switch latched, and is **not a server-authoritative
game**. Firmware displays `DEMO MODE - LOCAL ROUND, NO SERVER` and also refuses an
ineligible state such as an existing real round. Selecting a nickname does not
establish or change the badge's host designation.

Choose the host badge in the menu, then `6. demo`. Every registered badge is shown
in a numbered player list with its nickname and masked recovery ID. Enter 2–20
distinct numbers, separated by spaces or commas, **in the desired roster order**.
That order defines zero-based slots; the host need not be first, but must be
included. A missing host is refused before sending. Each selected nickname is
resolved through the verified archive lookup to its full factory MAC, without
asking for a MAC or reading a provisioning file. The mappings and verified lookup
are checked again before sending. An invisible/unverified player is refused.

Next choose patient zero by number from that roster. Enter defaults to the first
non-host player. The wrapper prints the roster's zero-based slots, patient-zero
slot, channel, duration and start delay before sending; MACs are masked to their
last four hex digits. Full MACs exist only in the lookup/request, never in displayed
JSON or the nickname registry.

The menu uses channel 6, duration **600000 ms**, and start delay **10000 ms**. The
direct command accepts `--channel` in 1–11 and `--start-delay` in 3000–30000 ms;
duration is fixed. Eight random bytes produce a nonzero, 16-digit lowercase hex
round ID. `--round-id` accepts an explicit ID in the same format. Round IDs are
reserved in memory and never reused within this CLI process, including after a
refusal, cancellation or uncertain response. No persistent round-ID inventory is
created, so an explicit override must also be unused outside this session.

For three registered badges named `badge-1`, `badge-2` and `badge-3`, with
`badge-1` the designated host and the numbered list in that order, the ordinary
menu session is:

```text
python3 tools/badge_ops.py
Badge number: 1
Action number: 6
Player numbers, separated by spaces or commas (q quits): 2 1 3
Patient zero [1] (q quits): [press Enter]
Port number [1] (m for another path, q quits): [press Enter for the sole host port]
```

This selects `badge-2` as slot 0 and default patient zero, `badge-1` as slot 1, and
`badge-3` as slot 2. No MAC, recovery ID, round ID, release path or wire JSON is typed.
With multiple port candidates, type the host's port number instead. After the
summary, the CLI sends exactly one `demo_round` request, using the transport's
own unsigned request ID, framing, response matching and connection cleanup. It
does not call the guarded tool for the demo request, open a serial port itself,
send an identity probe, mutate archives, change an installation, or implement a
second transport.

The exact successful envelope `{"id":…, "ok":true}` is printed as acknowledgment,
not proof that START has occurred. The host admits itself; **each other listed badge
must press A** to join the lobby. Once every listed player has joined, the host
sends PREPARE and then START after the delay. For the example, press A on `badge-2`
and `badge-3`, then wait for the 10-second start delay.

On a refusal, the CLI prints the badge's numeric error and stops; it never resends
with different values. A timeout, lost acknowledgment, malformed response or I/O
failure after a send attempt is reported as **UNCERTAIN**: the request may already
have taken effect. There is no automatic retry, reconnect, resend or status probe.
Inspect the badges before choosing another action. Transport exception messages,
raw request/response data and arbitrary evidence are never printed or logged by
the wrapper. The transport module is imported without modification, keeping the
recovery revision's hashed source set unchanged.

## Installation and provisioning

These commands require the merged C01 console extension of `badge_tool.py`.
`install` reads only `operations/*/result.json` beneath the freshly resolved,
verified badge archive. It selects the latest `FLASH_VERIFIED` result by its `utc`
timestamp and shows its validated operation UUID. No successful flash, tied latest
timestamps, or a successful installation already referencing that flash stops the
command with an explanation. The selection is checked again before launch. All
continuation eligibility checks, including consumed attempts and intervening writes,
remain the guarded tool's responsibility. The wrapper never reads captured images.

C01 still asks the operator for the original release-manifest path during
`install-init`. That prompt belongs to the guarded tool; this wrapper does not
answer it or change C01 to remove it. Resolving the operation UUID removes the
manual UUID lookup, not that separate guarded release-path prompt.

The provision menu offers the remembered configuration path if it still exists;
Enter accepts that prompt default, and a different path overrides it. If it no
longer exists, the operator must supply a path. Only existence is checked: this
wrapper never reads, parses, validates, copies or prints the file's contents. The
guarded tool alone handles the mesh key and host credentials. The wrapper remembers
the path after a completed invocation even if that invocation refused; remembering
a path is not a statement that its contents are valid. Direct `provision` still
requires `--config`, rather than silently applying a default. The menu also asks
for the provisioned display name (defaulting to the nickname) and a numbered
standard/host role selection, which supplies the explicit command argument and
does not answer any guarded observation prompt.

`diagnose` passes through to the guarded tool. `export-game` asks for a new private
output file because C01 requires `--output`; the menu uses the active round. C01
does not enumerate previous rounds, so an explicit `--round` remains a direct-command
option. The wrapper never reads exported game data or configuration contents.

Each invocation displays its argv as a JSON array for unambiguous argument
boundaries, then executes an argv list using `sys.executable` and the sibling
`badge_tool.py` resolved from `__file__`. Recovery references are masked by default
(for example, `ZT-AAAA-…-99`), including in previews and embedded in displayed paths.
`--show-id` on `list` or `status` explicitly reveals full references. The preview
therefore has one deliberate privacy exception to exact argv display: masked IDs;
the child receives the real ID. No shell interprets the arguments.

The child exit status is always shown; direct commands propagate it (signal exits
use the conventional `128 + signal` shell status). A failed child stops the current
command, including a multi-entry `list`, except that failed port discovery offers
the typed-path fallback described above (standalone `ports` still returns its
failure status). Its own `STOPPED: <CONDITION>` line appears
unchanged on inherited stderr. The wrapper points to the organizer-local
`invocations/` directory containing the private invocation record. It neither
captures that line nor opens private logs to reprint it. Argument-parser failures
may have no STOPPED line; the wrapper does not invent one. No refused operation is
retried or followed by an automatic alternative. The menu awaits a new choice.

## Automatic release selection for flash

```text
python3 tools/badge_ops.py releases
python3 tools/badge_ops.py flash badge-1
python3 tools/badge_ops.py flash badge-1 --release /private/local-release/release-manifest.json
```

Without `--release`, `flash` discovers manifests at
`<repo>/.orchestration/releases/<name>/release-manifest.json`. The repository is
located from `badge_ops.py` itself (`<repo>/tools/badge_ops.py`), with no absolute
path or environment override. It chooses the newest parsed `created_utc` timestamp
inside the manifests, **not** directory modification time or name order. It prints
the chosen manifest path, creation time and full source commit before flashing.
`releases` shows the same ordering with short commits and marks the selected entry.
Incomplete `.partial` directories are excluded. Unreadable/malformed selection
metadata stops automatic selection; equal newest timestamps require an explicit
`--release` instead of guessing. Automatic discovery refuses symlinked release paths.
These metadata checks select a candidate; they do not verify a release.

If no release exists, or `firmware/build/zombie_tag.bin` has a strictly newer file
modification time than the selected `release-manifest.json`, automatic flash offers
to generate a release. This staleness comparison uses file modification times at
nanosecond resolution; `created_utc` is used to choose the latest release, not to
compare it with the build. Equal modification times are not stale. If the local
binary is absent, an existing release remains selectable; a missing release still
offers generation and lets the guarded builder report any missing build files.

For a direct flash command, the operator types `generate` to accept the offer; the
menu presents numbered Generate/Cancel choices. Declining or
quitting stops without flashing, with no fallback to the stale release. On acceptance,
the wrapper creates the releases parent if needed and runs, with inherited stdio:

```text
badge_tool.py release-manifest --build <repo>/firmware/build --output <repo>/.orchestration/releases/auto-<utc>
```

Output names use the UTC time through microseconds, with an increasing numeric
suffix if that name already exists. The wrapper never creates the output directory
itself, reuses an existing output, or overwrites one; the guarded tool publishes the
release. The operator supplies the guarded tool's own review confirmation. Its
refusals, including a dirty Git tree or a build older than tracked sources, appear
unchanged and stop the flash. The wrapper never cleans/stages/commits sources,
changes modification times, or retries generation to bypass a refusal.

After successful generation, the wrapper selects that result and continues through
the ordinary port selection and nickname confirmation. If the build has become
newer than the automatically selected manifest before the flash starts, it stops
without flashing or automatically generating another release. Explicit `--release`
preserves the previous behavior: it wins outright and bypasses automatic discovery
and this staleness offer. Menu flash always uses automatic selection without asking
for a path. Direct `commission` still requires its explicit release path; the menu
supplies one from its numbered release picker.

Selection adds no capability and removes no gate: `verify_release()` remains inside
the guarded tool and runs on every flash, whether the path was automatic or explicit.
Before flashing, the wrapper prints that the guarded tool captures and verifies a
fresh backup before writing. **The pre-write backup is the guarded tool's guarantee,
not this wrapper's.** `flash-game` makes the fresh dual full-flash capture, verifies
both durable copies, writes only the application range, and compares the complete
4 MiB readback before any boot. The wrapper adds no capture, `snapshot`, `enrol`, or
backup-verification call. Restore remains tied to the nickname's recovery ID and
the guarded tool's full factory-MAC match.

## Example: enrol, flash, restore

These are operator commands, not instructions for an agent to touch hardware.
Use the existing private recovery setup, a reviewed release, and the actual port
observations. Required first-badge rehearsal and commissioning gates must already
be satisfied for the release; this example does not waive them.

```text
python3 tools/badge_ops.py enrol badge-1
  [view port observations; type a port number, or Enter for a sole candidate]
  [perform the guarded tool's observations and answer its prompts yourself]
  Enrolled badge-1: ZT-AAAA-…-99. The label grants no authorization.

python3 tools/badge_ops.py flash badge-1 --release /private/local-release/release-manifest.json
  [view port observations; type a port number, or Enter for a sole candidate]
  Selected badge-1 (ZT-AAAA-…-99): flash.
  Type the nickname to proceed: badge-1
  [perform the guarded tool's observations and answer its prompts yourself]

python3 tools/badge_ops.py restore badge-1
  [view port observations; type a port number, or Enter for a sole candidate]
  Selected badge-1 (ZT-AAAA-…-99): restore.
  Type the nickname to proceed: badge-1
  [follow the guarded restore and its subsequent physical observation prompts]
```

The same sequence works through the menu. A successful process exit is not a new
authorization or a claim beyond the guarded tool's reported result.

## Existing interface limitations

- `badge_tool.py status` returns a sorted set of historical outcomes, not the last
  outcome or a timestamp. `list` shows that output faithfully; it cannot honestly
  identify the latest result through this interface. Even status creates the
  guarded tool's private operation records; the wrapper itself never writes archives.
- An **initial** enrolment left at `AWAITING_SECOND_COPY` is absent from verified
  lookup and receives no nickname. Thus `verify NICK` cannot finish that initial
  capture, and `adopt` also refuses it. The organizer must run
  `python3 tools/badge_tool.py verify --recovery-id ID --snapshot original`
  with the full private recovery reference; adoption is available after verification
  succeeds. Pending later snapshots of an already verified, named original can use
  `verify NICK --snapshot SNAPSHOT` without hardware. The
  wrapper never weakens lookup or discovers IDs by reading private logs.
- Exact, unredacted argv previews conflict with default recovery-ID privacy.
  Previews retain the masking described above.

These interface limitations remain unchanged. No recovery source, dependency lock,
rehearsal receipt or configuration is changed. The wrapper lives outside the
recovery revision's hashed file set, so adding it does not invalidate the
demonstrated rehearsal.
