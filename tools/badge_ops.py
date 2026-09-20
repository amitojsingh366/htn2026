#!/usr/bin/env python3
"""Private nickname registry and sequential menu for the guarded badge tool."""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import signal
import stat
import subprocess
import sys
import tempfile
import uuid
from datetime import datetime, timezone

# Even help must leave the recovery package (and its revision inputs) untouched.
sys.dont_write_bytecode = True
import zt_badge.config as config
import zt_badge.index as index
import zt_badge.console as console


TOOL = Path(__file__).resolve().with_name("badge_tool.py")
REPO = TOOL.parent.parent
RELEASES = REPO / ".orchestration" / "releases"
BUILD = REPO / "firmware" / "build"
ID_PATTERN = re.compile(r"ZT-(?:[A-Z0-9]{4}-){4}[A-Z0-9]{2}")
RESERVED = {"list", "help", "quit", "all"}
HARDWARE = {"enrol", "flash", "restore", "rehearse", "commission",
            "install", "provision", "diagnose", "export-game", "demo"}
WRITES = {"flash", "restore", "rehearse", "install", "provision"}
CHILD_COMMAND = {"enrol": "enroll", "flash": "flash-game",
                 "rehearse": "rehearse-restore", "install": "install-init",
                 "export-game": "game-export"}
DEMO_ROUND_IDS = set()


class Refused(Exception):
    """A fixed, public explanation; never wrap private exception text."""


class MenuBack(Exception):
    """Return to the action menu without executing the selected operation."""


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, "Invalid arguments. Use the command's --help.\n")


def nickname(value):
    name = value.casefold()
    if (not value.isascii() or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,23}", name)
            or name in RESERVED):
        raise Refused("Nickname must be 1–24 ASCII letters/digits/hyphens, start "
                      "with a letter or digit, and not be list, help, quit or all.")
    return name


def masked(value):
    return value[:7] + "-…-" + value[-2:]


def display(value, show_id=False):
    """Escape terminal control characters and conceal embedded recovery IDs."""
    if not show_id:
        value = ID_PATTERN.sub(lambda match: masked(match.group()), value)
    return json.dumps(value, ensure_ascii=False)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Refused("Duplicate JSON keys; file left untouched.")
        result[key] = value
    return result


def registry_path():
    root = config.local_dir()
    for parent in (root, *root.parents):
        if parent.is_symlink() or (parent / ".git").exists():
            raise Refused("Registry location must be private and outside repositories and symlinks.")
    if any(word in str(root).casefold() for word in
           (".worktrees", "mobile documents", "cloudstorage", "dropbox", "onedrive", "google drive")):
        raise Refused("Registry location must be outside worktrees and synced folders.")
    if root.exists():
        info = root.stat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_mode & 0o077
                or info.st_uid != os.getuid()):
            raise Refused("Organizer-local directory must be owned by you and private (0700).")
    return root / "badge-names.json"


def validate_registry(data):
    if (not isinstance(data, dict) or set(data) not in
            ({"schema", "badges"}, {"schema", "badges", "operator_defaults"})
            or type(data["schema"]) is not int or data["schema"] != 1
            or not isinstance(data["badges"], dict)):
        raise Refused("Invalid nickname registry schema; nothing changed.")
    if "operator_defaults" in data:
        defaults = data["operator_defaults"]
        if (not isinstance(defaults, dict) or set(defaults) != {"provision_config"}
                or not isinstance(defaults["provision_config"], str)
                or not defaults["provision_config"] or "\0" in defaults["provision_config"]):
            raise Refused("Invalid operator defaults in registry; nothing changed.")
    ids = set()
    for name, entry in data["badges"].items():
        if nickname(name) != name or not isinstance(entry, dict) or set(entry) != {"recovery_id", "added_utc"}:
            raise Refused("Invalid nickname registry entry; nothing changed.")
        rid, added = entry["recovery_id"], entry["added_utc"]
        if not isinstance(rid, str) or not ID_PATTERN.fullmatch(rid) or rid in ids:
            raise Refused("Invalid or duplicate recovery reference in registry; nothing changed.")
        if not isinstance(added, str) or not added.endswith("Z"):
            raise Refused("Invalid registry timestamp; nothing changed.")
        try:
            datetime.fromisoformat(added)
        except ValueError:
            raise Refused("Invalid registry timestamp; nothing changed.") from None
        ids.add(rid)
    return data


def read_registry():
    path = registry_path()
    try:
        info = path.lstat()
    except FileNotFoundError:
        return {"schema": 1, "badges": {}}
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600 or info.st_uid != os.getuid()):
        raise Refused("Nickname registry must be a private, owned, regular 0600 file without links.")
    with path.open(encoding="utf-8") as stream:
        return validate_registry(json.load(stream, object_pairs_hook=unique_pairs))


def save_registry(data):
    validate_registry(data)
    path = registry_path()
    # Only this local file is written. Also reject a misconfigured archive that
    # contains the organizer-local directory itself; never write within either copy.
    settings = path.parent / "recovery-config.json"
    if settings.exists():
        with settings.open(encoding="utf-8") as stream:
            roots = json.load(stream, object_pairs_hook=unique_pairs)
        for key in ("archive", "mirror"):
            root = Path(roots[key]).expanduser().resolve()
            if path.resolve().is_relative_to(root):
                raise Refused("Registry location is inside archive or mirror; nothing changed.")
    fd, temporary = tempfile.mkstemp(prefix=".badge-names-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(data, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        Path(temporary).unlink(missing_ok=True)


def verified_ids():
    if not (config.local_dir() / "recovery-config.json").exists():
        raise Refused("No organizer configuration. Complete the existing private recovery setup before enrol.")
    try:
        # Discard the values immediately: full device identities are never output
        # or stored by this wrapper, and the lookup is never a write permission.
        ids = set(index.scan(config.load()["archive"]))
        if any(not ID_PATTERN.fullmatch(rid) for rid in ids):
            raise ValueError
        return ids
    except Exception:
        raise Refused("Verified archive lookup unavailable. Check organizer configuration/media privately; "
                      "run enrol for a missing or unverified badge. No automatic retry.") from None


def resolve(name):
    entry = read_registry()["badges"].get(name)
    if entry is None:
        raise Refused("Unknown nickname. Run enrol; no label was created.")
    rid = entry["recovery_id"]
    if rid not in verified_ids():
        raise Refused("Nickname archive is unknown or unverified. Run enrol; no badge operation started.")
    return rid


def archive_for_lookup(name):
    """Read-only metadata access for a freshly resolved, verified badge."""
    rid = resolve(name)
    try:
        root = config.load()["archive"]
        if rid not in index.scan(root):
            raise ValueError
        return rid, root / rid
    except Exception:
        raise Refused("Verified archive metadata unavailable; no badge operation started.") from None


def metadata_path(path, badge_root):
    if not path.is_relative_to(badge_root):
        raise Refused("Archive metadata path refused.")
    for part in (path, *path.parents):
        if part.is_symlink():
            raise Refused("Symlinked archive metadata refused.")
        if part == badge_root:
            break
    return path


def latest_flash_operation(name, expected_id):
    rid, root = archive_for_lookup(name)
    if rid != expected_id:
        raise Refused("Nickname changed during selection; no badge operation started.")
    operations = metadata_path(root / "operations", root)
    flashes, installed = [], set()
    for path in operations.glob("*/result.json"):
        metadata_path(path, root)
        if not stat.S_ISREG(path.lstat().st_mode):
            raise Refused("Operation metadata is not a regular file.")
        with path.open(encoding="utf-8") as stream:
            result = json.load(stream, object_pairs_hook=unique_pairs)
        if not isinstance(result, dict):
            raise Refused("Invalid operation metadata; no badge operation started.")
        if result.get("status") == "INSTALL_INITIALIZED":
            reference = result.get("flash_operation")
            if isinstance(reference, str):
                installed.add(reference)
        if result.get("status") != "FLASH_VERIFIED":
            continue
        operation = result.get("operation_uuid")
        try:
            if str(uuid.UUID(operation)) != operation or operation != path.parent.name:
                raise ValueError
            instant = datetime.fromisoformat(result["utc"])
            if instant.tzinfo is None:
                raise ValueError
            flashes.append((instant.astimezone(timezone.utc), operation))
        except (ValueError, TypeError, KeyError, AttributeError, OverflowError):
            raise Refused("Invalid successful flash metadata; no badge operation started.") from None
    if not flashes:
        raise Refused("This badge has not been flashed successfully; no install continuation is available.")
    flashes.sort(reverse=True)
    if len(flashes) > 1 and flashes[0][0] == flashes[1][0]:
        raise Refused("Latest successful flash is ambiguous; no install continuation selected.")
    operation = flashes[0][1]
    if operation in installed:
        raise Refused("The most recent successful flash has already been consumed by a successful install; "
                      "it will not be retried.")
    return operation


def remember_provision_path(value):
    # Remember only the path argument. Never open the provisioning file.
    try:
        data = read_registry()
        data["operator_defaults"] = {"provision_config": os.path.abspath(value)}
        save_registry(data)
    except (Refused, OSError, ValueError, TypeError, KeyError):
        print("Provision command finished, but its path default could not be saved. "
              "The reported child exit status is unchanged.", file=sys.stderr)


def run_tool(arguments, show_id=False):
    argv = [sys.executable, str(TOOL), *arguments]
    print("badge_tool.py argv (recovery references masked unless explicitly requested):", flush=True)
    print("[" + ", ".join(display(part, show_id) for part in argv) + "]", flush=True)
    interrupted = False
    # No redirection, pipes or answers: the child owns all its observations.
    with subprocess.Popen(argv) as child:
        try:
            code = child.wait()
        except KeyboardInterrupt:
            interrupted = True
            print("\nInterrupted; waiting for the guarded tool to finish stopping.", flush=True)
            if child.poll() is None:
                child.send_signal(signal.SIGINT)
            while True:
                try:
                    code = child.wait()
                    break
                except KeyboardInterrupt:
                    continue
    print(f"badge_tool.py exit status: {code}", flush=True)
    if code != 0 or interrupted:
        # The child's original STOPPED line has already appeared on inherited
        # stderr. Do not read/reprint private logs or invent a condition code.
        print("No automatic retry. Inspect the private invocation record under "
              + display(str(config.local_dir() / "invocations")) + ".", file=sys.stderr, flush=True)
    if code != 0:
        return code if code > 0 else 128 - code
    return 130 if interrupted else 0


def port_observations():
    """Capture only the fixed, prompt-free ports command; never its stderr."""
    print("Port candidates are observations. A port name never identifies a badge; "
          "the guarded tool rechecks the archived full MAC on every connection.", flush=True)
    argv = [sys.executable, str(TOOL), "ports"]
    print("badge_tool.py argv:", flush=True)
    print("[" + ", ".join(display(part) for part in argv) + "]", flush=True)
    try:
        child = subprocess.run(argv, stdout=subprocess.PIPE, text=True,
                               encoding="utf-8", errors="replace")
    except OSError:
        print("Could not run port discovery; no automatic retry.", file=sys.stderr, flush=True)
        return [], 1
    print(child.stdout, end="", flush=True)
    if child.stdout and not child.stdout.endswith("\n"):
        print(flush=True)
    code = child.returncode
    print(f"badge_tool.py exit status: {code}", flush=True)
    if code:
        print("No automatic retry. Inspect the private invocation record under "
              + display(str(config.local_dir() / "invocations")) + ".", file=sys.stderr, flush=True)
        return [], code if code > 0 else 128 - code
    lines = child.stdout.splitlines()
    header = "Candidate port observations (a port name never identifies a badge):"
    candidates = lines[1:] if lines and lines[0] == header else []
    if (not candidates or any(not re.fullmatch(r"/dev/[A-Za-z0-9._/-]+", path)
                              for path in candidates)
            or len(set(candidates)) != len(candidates)):
        return [], 0
    for number, path in enumerate(candidates, 1):
        print(f"{number}. {display(path)}", flush=True)
    return candidates, 0


def ports():
    candidates, code = port_observations()
    if not candidates:
        print("No numbered port candidates available; operations will ask for an explicit port.")
    return code


def release_root():
    if RELEASES.is_symlink() or RELEASES.parent.is_symlink():
        raise Refused("Automatic releases require a repository-local directory without symlinks.")
    return RELEASES


def release_metadata(path):
    """Read selection/display fields only; verification belongs to the child."""
    try:
        if path.is_symlink() or not path.is_file():
            raise ValueError
        with path.open(encoding="utf-8") as stream:
            data = json.load(stream, object_pairs_hook=unique_pairs)
        created, commit = data["created_utc"], data["source_commit"]
        if not isinstance(created, str) or not isinstance(commit, str):
            raise ValueError
        instant = datetime.fromisoformat(created)
        if instant.tzinfo is None or not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError
        return {"path": path, "created_utc": created,
                "instant": instant.astimezone(timezone.utc), "source_commit": commit}
    except (OSError, ValueError, TypeError, KeyError, OverflowError, Refused):
        raise Refused("Cannot read release selection metadata from " + display(str(path))
                      + "; automatic selection stopped. No older release substituted.") from None


def known_releases():
    root = release_root()
    if not root.exists():
        return []
    records = []
    for directory in root.iterdir():
        # The guarded builder publishes complete releases by renaming these.
        if directory.name.endswith(".partial") or not directory.is_dir():
            continue
        if directory.is_symlink():
            raise Refused("Automatic release discovery refuses symlinked release directories.")
        manifest = directory / "release-manifest.json"
        if manifest.exists() or manifest.is_symlink():
            records.append(release_metadata(manifest))
    return sorted(records, key=lambda record: record["instant"], reverse=True)


def newest_release(records):
    if not records:
        return None
    if len(records) > 1 and records[0]["instant"] == records[1]["instant"]:
        raise Refused("Newest release timestamps tie; use an explicit --release path. Nothing flashed.")
    return records[0]


def build_newer_than(path):
    try:
        build_time = (BUILD / "zombie_tag.bin").stat().st_mtime_ns
    except FileNotFoundError:
        return False
    return build_time > path.stat().st_mtime_ns


def list_releases():
    records = known_releases()
    if not records:
        print("No releases found. Flash without --release will offer guarded release generation.")
        return 0
    tied = len(records) > 1 and records[0]["instant"] == records[1]["instant"]
    print("Known releases (newest created_utc first):")
    for number, record in enumerate(records):
        marker = " [selected]" if number == 0 and not tied else ""
        print(display(record["path"].parent.name) + "  " + display(record["created_utc"])
              + "  " + record["source_commit"][:12] + marker)
    if tied:
        print("No automatic selection: newest timestamps tie. Supply --release explicitly.")
    elif build_newer_than(records[0]["path"]):
        print("Selected release is stale: build binary is newer than its manifest. Flash will offer generation.")
    return 0


def announce_release(record, short_commit=False):
    print("Selected release: " + display(str(record["path"])), flush=True)
    print("created_utc: " + display(record["created_utc"])
          + "; source_commit: " + record["source_commit"][:12 if short_commit else 40], flush=True)


def prepare_latest_release(menu_mode=False):
    record = newest_release(known_releases())
    if record is not None:
        announce_release(record, menu_mode)
        if not build_newer_than(record["path"]):
            return record["path"], 0
        print("Selected release is stale: firmware/build/zombie_tag.bin is newer than its manifest.")
    else:
        print("No release exists for automatic selection.")
    print("The guarded tool can generate a release from " + display(str(BUILD)) + ".")
    if not sys.stdin.isatty():
        raise Refused("Release generation requires an interactive choice; nothing flashed.")
    if menu_mode:
        print("1. Generate a release with the guarded tool\n2. Cancel")
        accepted = choose_number(2, "Release action [2] (q quits): ", default=2) == 1
    else:
        answer = input("Type generate to create a release, or press Enter to cancel (q quits): ").strip()
        if answer.casefold() == "q":
            raise EOFError
        accepted = answer == "generate"
    if not accepted:
        raise Refused("Release generation declined; nothing flashed. No stale-release fallback.")
    root = release_root()
    root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("auto-%Y%m%dT%H%M%S%fZ")
    output, suffix = root / stamp, 0
    while output.exists() or output.is_symlink():
        suffix += 1
        output = root / f"{stamp}-{suffix}"
    # Never create output itself, reuse it, or retry a refused generation.
    code = run_tool(["release-manifest", "--build", str(BUILD), "--output", str(output)])
    if code:
        return None, code
    record = release_metadata(output / "release-manifest.json")
    announce_release(record, menu_mode)
    if build_newer_than(record["path"]):
        raise Refused("Build is newer than the generated manifest; nothing flashed. No automatic retry.")
    return record["path"], 0


def ask(prompt):
    value = input(prompt).strip()
    if value.casefold() == "q":
        raise EOFError
    if not value:
        raise Refused("A value is required; operation cancelled.")
    return value


def select_port(explicit, menu_mode=False):
    candidates, _ = port_observations()
    port = explicit
    if port is None and candidates:
        print("m. Enter another port path")
        default = 1 if len(candidates) == 1 else None
        prompt = "Port number [1]" if default else "Port number"
        choice = choose_number(len(candidates), prompt + " (m for another path, q quits): ",
                               default=default, shortcuts=("m",))
        if choice != "m":
            port = candidates[choice - 1]
    elif port is None:
        print("No usable numbered port candidates; enter the port path explicitly.")
    if port is None:
        port = ask("Enter a port from the observations (q quits): ")
    if not re.fullmatch(r"/dev/[A-Za-z0-9._/-]+", port):
        raise Refused("Enter an explicit /dev/ port path; it is not saved with the nickname.")
    print("Selected port observation: " + display(port), flush=True)
    return port, 0


def confirm_selection(name, rid, args):
    print(f"Selected {name} ({masked(rid)}): {args.command}.", flush=True)
    if args.command == "flash":
        print("Release: " + display(args.release), flush=True)
        print("The guarded tool captures and verifies a fresh backup before writing.", flush=True)
    if args.command == "restore":
        print("Snapshot: " + display(args.snapshot), flush=True)
    print("This nickname check only guards selection. The guarded tool's own confirmations still follow.")
    if not sys.stdin.isatty():
        raise Refused("Nickname confirmation requires interactive stdin.")
    if input("Type the nickname to proceed: ").strip() != name:
        raise Refused("Nickname did not match; no badge operation started.")


def list_badges(show_id=False):
    badges = read_registry()["badges"]
    configured = (config.local_dir() / "recovery-config.json").exists()
    ids = verified_ids() if configured else set()
    print("Registered badges:")
    for name, entry in sorted(badges.items()):
        rid = entry["recovery_id"]
        visible = "visible (verified)" if rid in ids else "not visible as verified"
        print(f"  {name}: {display(rid, show_id)} — {visible}")
    if not badges:
        print("  (none)")
    unnamed = sorted(ids - {entry["recovery_id"] for entry in badges.values()})
    print("Verified archives without a nickname:")
    for rid in unnamed:
        print("  " + display(rid, show_id))
    if not unnamed:
        print("  (none visible)")
    else:
        print("  To name an archive, use adopt NICK --recovery-id FULL_ID, or choose adopt in the menu.")
        print("  Enter the exact full ID from your private receipt or list --show-id; masked IDs are not accepted.")
    if not configured:
        print("No organizer configuration; archive visibility and outcomes unavailable. Nothing created.")
        return 0
    print("Outcomes below are the guarded tool's historical set; it does not expose a latest outcome.")
    for name, entry in sorted(badges.items()):
        if entry["recovery_id"] not in ids:
            print(f"{name}: outcomes unavailable; run enrol.")
            continue
        rid = resolve(name)
        print(f"{name} — recorded outcomes:", flush=True)
        code = run_tool(["status", "--recovery-id", rid], show_id)
        if code:
            return code
    for rid in unnamed:
        if rid not in verified_ids():
            raise Refused("Unnamed archive is no longer verified; run enrol.")
        print("Unnamed " + display(rid, show_id) + " — recorded outcomes:", flush=True)
        code = run_tool(["status", "--recovery-id", rid], show_id)
        if code:
            return code
    return 0


def enrol(args, name):
    if name in read_registry()["badges"]:
        raise Refused("Nickname already exists; choose another name or use rename.")
    before = verified_ids()
    port, code = select_port(args.port, getattr(args, "menu_mode", False))
    if code:
        return code
    code = run_tool(["enroll", "--port", port, "--owner-label", name])
    if code:
        print("No nickname bound. For an initial pending second copy, use badge_tool.py verify "
              "--recovery-id FULL_ID --snapshot original in the private recovery workflow.")
        return code
    appeared = verified_ids() - before
    if len(appeared) != 1:
        raise Refused(f"Enrol found {len(appeared)} newly visible verified archives; no nickname bound. "
                      "Existing archives remain available through the guarded tool; list shows unnamed archives.")
    rid = appeared.pop()
    data = read_registry()
    if name in data["badges"] or any(entry["recovery_id"] == rid for entry in data["badges"].values()):
        raise Refused("Nickname or recovery reference was already bound; nothing changed.")
    data["badges"][name] = {"recovery_id": rid,
                            "added_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")}
    save_registry(data)
    print(f"Enrolled {name}: {masked(rid)}. The label grants no authorization.")
    return 0


def adopt(name, rid):
    if not ID_PATTERN.fullmatch(rid):
        raise Refused("A full recovery ID matching ZT-AAAA-BBBB-CCCC-DDDD-99 is required exactly; "
                      "prefixes, masked IDs and list indices are not accepted.")
    data = read_registry()
    if name in data["badges"]:
        raise Refused("Nickname already exists; choose another name or use rename.")
    for existing, entry in data["badges"].items():
        if entry["recovery_id"] == rid:
            raise Refused(f"Recovery reference is already bound to nickname {existing}; nothing changed.")
    if rid not in verified_ids():
        raise Refused("Recovery reference is not in the verified archive lookup; no nickname bound.")
    data["badges"][name] = {"recovery_id": rid,
                            "added_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")}
    save_registry(data)
    print(f"Adopted {name}: {masked(rid)}. Label only; no badge read and no archive changed.")
    return 0


def choose_demo_players():
    badges = sorted(read_registry()["badges"].items())
    if len(badges) < 2:
        raise Refused("A demo needs at least two registered badges. Enrol or adopt the players first.")
    print("Demo players — enter 2–20 distinct badge numbers in the desired roster order:")
    for number, (name, entry) in enumerate(badges, 1):
        print(f"{number}. {name} — {masked(entry['recovery_id'])}")
    numbers = {str(number): number - 1 for number in range(1, len(badges) + 1)}
    while True:
        answer = input("Player numbers, separated by spaces or commas (q quits): ").strip()
        if answer.casefold() == "q":
            raise EOFError
        parts = answer.replace(",", " ").split()
        if (2 <= len(parts) <= 20 and len(set(parts)) == len(parts)
                and all(part in numbers for part in parts)):
            return [(badges[numbers[part]][0], badges[numbers[part]][1]["recovery_id"])
                    for part in parts]
        print("Enter 2–20 distinct numbers from the list; no player identifiers are needed.")


def resolve_demo_players(selected):
    badges = read_registry()["badges"]
    try:
        identities = index.scan(config.load()["archive"])
    except Exception:
        raise Refused("Verified player lookup unavailable; no demo request sent.") from None
    roster = []
    for name, expected_id in selected:
        entry = badges.get(name)
        if entry is None or entry["recovery_id"] != expected_id:
            raise Refused("A selected player's nickname mapping changed; no demo request sent.")
        mac = identities.get(expected_id)
        if mac is None:
            raise Refused(f"Selected player {name} has no visible verified archive; enrol or verify it first.")
        if (not isinstance(mac, str) or not re.fullmatch(r"[0-9a-f]{12}", mac)
                or int(mac, 16) == 0 or int(mac[:2], 16) & 1):
            raise Refused("A selected player's archived identity is invalid; no demo request sent.")
        roster.append((name, expected_id, mac))
    if len({player[2] for player in roster}) != len(roster):
        raise Refused("Duplicate archived player identities; no demo request sent.")
    return roster


def reserve_demo_round(value):
    if value is not None:
        if not re.fullmatch(r"[0-9a-f]{16}", value) or int(value, 16) == 0:
            raise Refused("Round ID must be exactly 16 lowercase hex digits and nonzero.")
        if value in DEMO_ROUND_IDS:
            raise Refused("Round ID already reserved in this CLI session; it will not be reused.")
    else:
        while True:
            value = secrets.token_bytes(8).hex()
            if int(value, 16) != 0 and value not in DEMO_ROUND_IDS:
                break
    # Keep reserved even after refusal, interruption or an uncertain acknowledgment.
    DEMO_ROUND_IDS.add(value)
    return value


def demo(args, name, rid):
    if not 3000 <= args.start_delay <= 30000:
        raise Refused("Demo start delay must be 3000–30000 milliseconds.")
    round_id = reserve_demo_round(args.round_id)
    selected = choose_demo_players()
    if (name, rid) not in selected:
        raise Refused("The selected host badge must be in the demo roster; no request sent.")
    roster = resolve_demo_players(selected)
    print("Choose patient zero (list number maps to the displayed roster slot):")
    for slot, (player, _, _) in enumerate(roster):
        note = " — selected host" if player == name else ""
        print(f"{slot + 1}. {player} — slot {slot}{note}")
    default = next(slot + 1 for slot, player in enumerate(roster) if player[0] != name)
    patient_zero = choose_number(len(roster), f"Patient zero [{default}] (q quits): ", default=default) - 1
    port, code = select_port(args.port, getattr(args, "menu_mode", False))
    if code:
        return code
    if resolve_demo_players(selected) != roster:
        raise Refused("A selected player's archived identity changed; no demo request sent.")
    print(f"Local demo via {name} ({masked(rid)}); round {round_id}:", flush=True)
    for slot, (player, recovery_id, mac) in enumerate(roster):
        print(f"  Slot {slot}: {player} — {masked(recovery_id)} — MAC ********{mac[-4:]}", flush=True)
    print(f"Patient-zero slot: {patient_zero}; channel: {args.channel}; duration: 600000 ms; "
          f"start delay: {args.start_delay} ms.", flush=True)
    print("Connect the designated host with its host switch latched. Firmware enforces the host role; "
          "this single request performs no separate device-identity probe.", flush=True)
    response, failure, attempted = None, None, False
    try:
        # Gameplay only: one existing-transport request, no guarded-tool dispatch,
        # framing implementation, serial access, archive mutation, or retry here.
        with console.Console(port) as connection:
            attempted = True
            response = connection.request("demo_round", round_id=round_id, duration_ms=600000,
                                          players=[player[2] for player in roster],
                                          patient_zero_slot=patient_zero, channel=args.channel,
                                          start_delay_ms=args.start_delay)
    except BaseException as error:
        # Transport exceptions can contain private I/O details; never echo repr,
        # messages, raw responses, or arbitrary evidence.
        failure = error
    if (isinstance(response, dict) and set(response) == {"id", "ok"}
            and type(response["id"]) is int and 0 <= response["id"] <= 0xffffffff
            and response["ok"] is True):
        print(json.dumps({"id": response["id"], "ok": True}, separators=(",", ":")))
        print("Demo request accepted; acknowledgment is not confirmation that the round has started.")
        if failure is not None:
            print("The connection did not close cleanly after acknowledgment; no further request was sent.")
        print("The host admits itself; each OTHER listed badge must press A to join the lobby.")
        print("Once every listed player has joined, the host sends PREPARE, then START after the delay.")
        return 0
    badge_error = None
    if getattr(failure, "condition", None) == "CONSOLE_OPERATION_REFUSED":
        evidence = getattr(failure, "evidence", {})
        if isinstance(evidence, dict):
            badge_error = evidence.get("error")
    elif isinstance(response, dict) and response.get("ok") is False:
        badge_error = response.get("error")
    if type(badge_error) is int and 1 <= badge_error <= 20:
        print(f"Badge refused the demo: error {badge_error}. No retry or resend.", file=sys.stderr)
    elif attempted:
        print("Demo outcome UNCERTAIN: no valid success/refusal acknowledgment was confirmed. "
              "The request may have taken effect; it was not resent. Inspect the badges before any new action.",
              file=sys.stderr)
    else:
        print("Console connection failed before the demo request; nothing sent and no retry.", file=sys.stderr)
    return 1


def dispatch(args):
    command = args.command
    if command == "enroll":
        command = args.command = "enrol"
    if command == "list":
        return list_badges(args.show_id)
    if command == "ports":
        return ports()
    if command == "releases":
        return list_releases()
    name = nickname(args.nick)
    if command == "adopt":
        return adopt(name, args.recovery_id)
    if command in {"rename", "forget"}:
        data = read_registry()
        if name not in data["badges"]:
            raise Refused("Unknown nickname. Run enrol; nothing changed.")
        if command == "rename":
            new = nickname(args.new)
            if new in data["badges"]:
                raise Refused("New nickname already exists; nothing changed.")
            data["badges"][new] = data["badges"].pop(name)
            save_registry(data)
            print(f"Renamed {name} to {new}.")
        else:
            del data["badges"][name]
            save_registry(data)
            print(f"Forgot label {name}. The archive, its images and its receipts are untouched. "
                  "The badge is still recoverable by recovery ID.")
        return 0
    if command == "enrol":
        return enrol(args, name)
    rid = resolve(name)
    if getattr(args, "selected_id", rid) != rid:
        raise Refused("The selected nickname now names a different archive; select the badge again.")
    if command == "demo":
        return demo(args, name, rid)
    operation = latest_flash_operation(name, rid) if command == "install" else None
    if operation is not None:
        print("Continuing successful flash operation: " + operation, flush=True)
    automatic_release = command == "flash" and args.release is None
    if automatic_release:
        release, code = prepare_latest_release(getattr(args, "menu_mode", False))
        if code:
            return code
        args.release = str(release)
    port = None
    if command in HARDWARE:
        port, code = select_port(args.port, getattr(args, "menu_mode", False))
        if code:
            return code
    if command in WRITES:
        confirm_selection(name, rid, args)
    arguments = [CHILD_COMMAND.get(command, command), "--recovery-id", rid]
    if port is not None:
        arguments += ["--port", port]
    if command in {"flash", "commission"}:
        arguments += ["--release", args.release]
    if command in {"restore", "verify", "export"}:
        arguments += ["--snapshot", args.snapshot]
    if command == "export":
        arguments += ["--destination", args.to]
    if command == "install":
        arguments += ["--operation", operation]
    if command == "provision":
        arguments += ["--config", args.config, "--name", args.name]
        if args.host:
            arguments.append("--host")
    if command == "export-game":
        arguments += ["--output", args.output]
        if args.round is not None:
            arguments += ["--round", args.round]
    if resolve(name) != rid:
        raise Refused("Nickname changed during selection; no badge operation started.")
    if automatic_release and build_newer_than(Path(args.release)):
        raise Refused("Build became newer than the selected manifest; nothing flashed. Start a new command.")
    if operation is not None and latest_flash_operation(name, rid) != operation:
        raise Refused("Latest successful flash changed during selection; no install operation started.")
    code = run_tool(arguments, getattr(args, "show_id", False))
    if command == "provision":
        remember_provision_path(args.config)
    if command == "flash" and code == 0:
        print("Next step: power-cycle with START released, then choose install, then provision. "
              "These are operator actions; nothing runs automatically.")
    return code


def parser():
    result = Parser(description="Private badge menu: guarded recovery via badge_tool.py, local demos via console transport.",
                    epilog="With no arguments, enter the menu. No configuration is created by help or list.",
                    allow_abbrev=False)
    subs = result.add_subparsers(dest="command")
    descriptions = {
        "list": "List labels, visible verified archives and guarded historical outcomes.",
        "enrol": "Capture with the guarded tool; bind exactly one newly verified archive.",
        "adopt": "Label an existing verified archive by its exact full ID; no badge operation.",
        "flash": "Run guarded flash-game; default to the newest release or offer generation.",
        "install": "Continue the latest successful, unused flash operation with guarded install-init.",
        "provision": "Run guarded provision using a private configuration file; remember only its path.",
        "diagnose": "Run guarded device diagnostics.",
        "demo": "Start a local demo on the designated host using a numbered player roster.",
        "export-game": "Run guarded game-export to a private local output file.",
        "releases": "List local release metadata and automatic selection; read-only.",
        "restore": "Run guarded restore (original snapshot by default).",
        "rehearse": "Run guarded original restore rehearsal.",
        "verify": "Verify stored copies without hardware (verified original lookup required).",
        "status": "Show guarded historical outcomes for a nickname.",
        "export": "Run guarded export to a private local directory.",
        "commission": "Run guarded commissioning for an initialized release.",
        "ports": "Show port observations, never badge identity.",
        "rename": "Change a label only.",
        "forget": "Remove a label only; preserve all recovery data.",
    }
    for command, description in descriptions.items():
        options = {"aliases": ["enroll"]} if command == "enrol" else {}
        sub = subs.add_parser(command, description=description, help=description,
                              allow_abbrev=False, **options)
        if command not in {"list", "ports", "releases"}:
            sub.add_argument("nick", help="Memorable nickname (case-folded).")
        if command == "adopt":
            sub.add_argument("--recovery-id", required=True, help="Exact full recovery ID of an unlabelled verified archive.")
        if command in HARDWARE:
            sub.add_argument("--port", help="Explicit /dev/ observation; otherwise prompted after ports.")
        if command in {"flash", "commission"}:
            sub.add_argument("--release", required=command == "commission",
                             help="Reviewed release manifest path; flash defaults to the newest local release.")
        if command in {"restore", "verify", "export"}:
            sub.add_argument("--snapshot", default="original", help="Snapshot name (default: original).")
        if command == "export":
            sub.add_argument("--to", required=True, help="Private local owner destination directory.")
        if command == "provision":
            sub.add_argument("--config", required=True, help="Private configuration path; contents read only by badge_tool.py.")
            sub.add_argument("--name", required=True, help="Provisioned display name.")
            sub.add_argument("--host", action="store_true", help="Pass the explicit host selection to the guarded tool.")
        if command == "export-game":
            sub.add_argument("--output", required=True, help="New private local output file required by game-export.")
            sub.add_argument("--round", help="Optional round reference passed to the guarded tool.")
        if command == "demo":
            sub.add_argument("--round-id", help="Unused nonzero 16-digit lowercase hex round ID; random by default.")
            sub.add_argument("--channel", type=int, choices=range(1, 12), default=6, help="Radio channel (default: 6).")
            sub.add_argument("--start-delay", type=int, default=10000, metavar="MS",
                             help="Start delay, 3000–30000 milliseconds (default: 10000).")
        if command == "rename":
            sub.add_argument("new", help="New, unused nickname.")
        if command in {"list", "status"}:
            sub.add_argument("--show-id", action="store_true", help="Explicitly display private recovery references in full.")
    return result


def safely_dispatch(args):
    try:
        return dispatch(args)
    except Refused as error:
        print("Operator CLI stopped: " + str(error), file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError):
        print("Local file or process error. Inspect release files or organizer-local configuration/registry privately. "
              "No automatic retry; registry replacement is atomic.", file=sys.stderr)
        return 1


def choose_number(count, prompt, default=None, shortcuts=()):
    choices = {str(number) for number in range(1, count + 1)}
    while True:
        answer = input(prompt).strip().casefold()
        if answer == "q":
            raise EOFError
        if answer in shortcuts:
            return answer
        if not answer and default is not None:
            return default
        if answer in choices:
            return int(answer)
        print("Invalid choice; enter a listed number or q to quit.")


def choose_badge(unnamed_only=False):
    badges = read_registry()["badges"]
    ids = verified_ids() if (config.local_dir() / "recovery-config.json").exists() else set()
    choices = []
    if not unnamed_only:
        print("Registered badges:")
        for name, entry in sorted(badges.items()):
            rid = entry["recovery_id"]
            choices.append((name, rid))
            note = "" if rid in ids else " (not currently visible as verified)"
            print(f"{len(choices)}. {name} — {masked(rid)}{note}")
        if not badges:
            print("  (none)")
    unnamed = sorted(ids - {entry["recovery_id"] for entry in badges.values()})
    print("Verified archives without a nickname — adoption required:")
    for rid in unnamed:
        choices.append((None, rid))
        print(f"{len(choices)}. {masked(rid)}")
    if not unnamed:
        print("  (none visible)")
    if not choices:
        print("No selectable badges are visible. Choose enrol for a new badge; "
              "an existing archive must be visible and verified before adoption.")
        return None
    print("a. Return to actions (including enrol)   q. Quit")
    choice = choose_number(len(choices), "Badge number: ", shortcuts=("a",))
    if choice == "a":
        return None
    name, rid = choices[choice - 1]
    if name is not None:
        return name, rid
    print("Selected unnamed archive: " + masked(rid))
    print("1. Adopt this archive, then continue\n2. Return to actions")
    if choose_number(2, "Adopt first? [2] (q quits): ", default=2) != 1:
        raise MenuBack
    name = nickname(ask("New nickname (q quits): "))
    supplied = input("Type the selected archive's exact full recovery ID (q quits): ")
    if supplied.casefold() == "q":
        raise EOFError
    if supplied != rid:
        raise Refused("Full recovery ID did not exactly match the selected archive; no nickname bound.")
    adopt(name, supplied)
    return name, rid


def choose_snapshot(name, expected_id):
    rid, root = archive_for_lookup(name)
    if rid != expected_id:
        raise Refused("Nickname changed during snapshot selection; choose the badge again.")
    snapshots = ["original"]
    directory = metadata_path(root / "snapshots", root)
    if directory.exists():
        for path in sorted(directory.iterdir(), reverse=True):
            if path.name.endswith(".partial") or not path.is_dir():
                continue
            metadata_path(path, root)
            manifest = metadata_path(path / "manifest.json", root)
            if manifest.is_file() and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,199}", path.name):
                snapshots.append(path.name)
    print("1. original — default; immutable enrolment state")
    for number, snapshot in enumerate(snapshots[1:], 2):
        print(f"{number}. " + display(snapshot) + " — later checkpoint")
    print("Restoring a later checkpoint restores exactly those bytes, damage included. "
          "The guarded tool verifies the selected snapshot.")
    choice = choose_number(len(snapshots), "Snapshot [1] (q quits): ", default=1)
    return snapshots[choice - 1]


def choose_commission_release():
    records = known_releases()
    if not records:
        raise Refused("No local release is available to select for commissioning. "
                      "The direct commission command accepts an explicit release path.")
    print("Select the release used for this badge's flash:")
    for number, record in enumerate(records, 1):
        print(f"{number}. " + display(record["path"].parent.name) + "  "
              + display(record["created_utc"]) + "  " + record["source_commit"][:12])
    return str(records[choose_number(len(records), "Release number (q quits): ") - 1]["path"])


def prompt_provision_config():
    saved = read_registry().get("operator_defaults", {}).get("provision_config")
    if saved is not None and Path(saved).exists():
        prompt = "Private configuration path [" + display(saved) + "] (q quits): "
    else:
        if saved is not None:
            print("The remembered configuration path no longer exists; enter a path.")
        saved = None
        prompt = "Private configuration path (q quits): "
    while True:
        value = input(prompt).strip()
        if value.casefold() == "q":
            raise EOFError
        if value or saved is not None:
            return value or saved
        print("A path is required. Only the guarded tool reads its contents.")


def menu_arguments(command, selected):
    arguments = [command]
    if command in {"list", "ports", "releases"}:
        return arguments, selected
    if command == "enrol":
        arguments.append(nickname(ask("New nickname for enrolment (q quits): ")))
        return arguments, selected
    if selected is None:
        selected = choose_badge()
        if selected is None:
            raise MenuBack
    name, rid = selected
    arguments.append(name)
    if command == "commission":
        arguments += ["--release", choose_commission_release()]
    if command in {"restore", "verify", "export"}:
        arguments += ["--snapshot", choose_snapshot(name, rid)]
    if command == "provision":
        arguments += ["--config", prompt_provision_config()]
        label = input("Provisioned display name [" + name + "] (q quits): ").strip()
        if label.casefold() == "q":
            raise EOFError
        arguments += ["--name", label or name]
        print("1. Standard badge\n2. Host badge")
        if choose_number(2, "Provisioning role [1] (q quits): ", default=1) == 2:
            arguments.append("--host")
    if command == "export":
        arguments += ["--to", ask("Private destination directory (q quits): ")]
    if command == "export-game":
        arguments += ["--output", ask("New private game-export file (q quits): ")]
    if command == "rename":
        arguments.append(nickname(ask("New nickname (q quits): ")))
    return arguments, selected


def menu(arg_parser):
    commands = ("enrol", "flash", "install", "provision", "diagnose", "demo", "commission",
                "restore", "rehearse", "verify", "export", "export-game", "status",
                "list", "ports", "releases", "adopt", "rename", "forget")
    selected, pick_badge = None, True
    while True:
        try:
            print("\nBadge operator menu")
            if pick_badge:
                pick_badge = False
                selected = choose_badge()
            if selected is not None:
                print(f"Selected: {selected[0]} — {masked(selected[1])}")
            else:
                print("No badge selected. Enrol a new badge or choose an action and select an existing badge.")
            for number, command in enumerate(commands, 1):
                print(f"{number}. {command}")
            print("b. Change badge   h. Help   q. Quit")
            choice = choose_number(len(commands), "Action number: ", shortcuts=("b", "h"))
            if choice == "b":
                selected, pick_badge = None, True
                continue
            if choice == "h":
                arg_parser.print_help()
                continue
            command = commands[choice - 1]
            if command == "adopt":
                adopted = choose_badge(unnamed_only=True)
                if adopted is not None:
                    selected = adopted
                continue
            arguments, chosen = menu_arguments(command, selected)
            try:
                args = arg_parser.parse_args(arguments)
            except SystemExit:
                continue
            args.menu_mode = True
            if chosen is not None and command not in {"enrol", "list", "ports", "releases"}:
                args.selected_id = chosen[1]
            selected = chosen
            code = safely_dispatch(args)
            print(f"Command exit status: {code}")
            if code == 0 and command == "enrol":
                selected = (args.nick, read_registry()["badges"][args.nick]["recovery_id"])
            if code == 0 and command in {"rename", "forget"}:
                selected = None
        except MenuBack:
            continue
        except Refused as error:
            print("Operator CLI stopped: " + str(error), file=sys.stderr)
        except (OSError, ValueError, TypeError, KeyError):
            print("Local metadata or menu input unavailable; nothing is retried.", file=sys.stderr)


def main():
    arg_parser = parser()
    args = arg_parser.parse_args()
    try:
        return menu(arg_parser) if args.command is None else safely_dispatch(args)
    except EOFError:
        print("\nGoodbye.")
        return 0
    except KeyboardInterrupt:
        print("\nCancelled. Any registry replacement is complete or unchanged; nothing is retried.")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
