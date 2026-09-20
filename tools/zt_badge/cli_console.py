"""Console command registration; importing this module never imports transport."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import uuid
import zlib
from . import APP_OFFSET, APP_SIZE, FLASH_SIZE, TAIL_OFFSET, revision, config, gates
from .archive import Archive
from .cli_recovery import confirm
from .errors import RecoveryError, require
from .manifest import (read_json, regular, pairs_unique, hash_file, equal_files,
                       json_new, write_new, utc, component)


def hex_value(value, size, condition="CONSOLE_HEX_INVALID"):
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{%d}" % (size * 2), value), condition)
    return bytes.fromhex(value)


def mac_value(value):
    raw = hex_value(value, 6, "CONFIG_MAC_INVALID")
    require(any(raw) and not raw[0] & 1, "CONFIG_MAC_INVALID")
    return value


def bounded_text(value, minimum, maximum, ascii_only=False):
    require(type(value) is str and "\0" not in value, "CONFIG_TEXT_INVALID")
    try:
        raw = value.encode("utf-8")
    except UnicodeError:
        raise RecoveryError("CONFIG_TEXT_INVALID") from None
    require(minimum <= len(raw) <= maximum, "CONFIG_TEXT_LENGTH_INVALID")
    if ascii_only:
        require(all(32 <= c <= 126 for c in raw), "CONFIG_TEXT_INVALID")
    return value


def private_path(value, existing=True):
    path = Path(value)
    require(path.is_absolute() and path.parent.is_dir(), "PRIVATE_ABSOLUTE_PATH_REQUIRED")
    require(not path.is_symlink() and all(not p.is_symlink() for p in path.parents), "SYMLINK_PRIVATE_PATH")
    config.safe_root(path.parent)
    config.physical_devices(path.parent)
    require(path.parent.stat().st_mode & 0o077 == 0, "PRIVATE_DIRECTORY_PERMISSIONS_REQUIRED")
    if existing:
        regular(path)
        require(path.stat().st_mode & 0o077 == 0, "PRIVATE_FILE_PERMISSIONS_REQUIRED")
    else:
        require(not path.exists(), "DESTINATION_EXISTS")
    return path


def release_path_from_operator(value=None):
    # R01 retained a manifest hash, not its pathname. Do not search private disks.
    if value is None:
        prompt = "Enter the absolute local release-manifest path used for the recorded flash:\n"
        try:
            with open("/dev/tty", "r+", encoding="utf-8") as terminal:
                terminal.write(prompt)
                terminal.flush()
                value = terminal.readline(4097)
        except OSError:
            import zt_badge
            fd = zt_badge.OPERATOR_FD
            require(fd is not None and sys.stdin.isatty(), "LOCAL_OPERATOR_OBSERVATION_REQUIRED")
            os.write(fd, prompt.encode())
            value = sys.stdin.readline(4097)
    else:
        # Apply the same bounded-line validation to an argument as terminal input.
        value += "\n"
    require(value.endswith("\n") and len(value) <= 4096, "LOCAL_RELEASE_PATH_REQUIRED")
    path = Path(value.strip())
    require(path.is_absolute(), "LOCAL_RELEASE_PATH_REQUIRED")
    return regular(path)


def verified_flash(archive, operation_id):
    require(type(operation_id) is str, "VERIFIED_FLASH_OPERATION_REQUIRED")
    try:
        require(str(uuid.UUID(operation_id)) == operation_id, "VERIFIED_FLASH_OPERATION_REQUIRED")
    except ValueError:
        raise RecoveryError("VERIFIED_FLASH_OPERATION_REQUIRED") from None
    path = archive.device_dir / "operations" / component(operation_id)
    mirror = archive.mirror / path.relative_to(archive.root)
    for name in ("result.json", "intent-prepared.json", "intent.json", "expected.bin", "post-write.bin"):
        require(equal_files(path / name, mirror / name), "FLASH_OPERATION_COPY_MISMATCH")
    result = read_json(path / "result.json")
    require(result.get("operation_uuid") == operation_id and result.get("status") == "FLASH_VERIFIED"
            and result.get("initialization_required") is True and result.get("tool") == revision(),
            "VERIFIED_FLASH_CONTINUATION_REQUIRED")
    prepared = read_json(path / "intent-prepared.json")
    writing = read_json(path / "intent.json")
    require(prepared.get("status") == "PREPARED" and writing.get("status") == "WRITING",
            "FLASH_OPERATION_INTENT_INVALID")
    for key in ("operation_uuid", "identity", "session_uuid", "current_snapshot", "original_hash",
                "current_hash", "release_hash", "expected_hash", "installation_uuid", "write_range"):
        require(prepared.get(key) == writing.get(key) == result.get(key), "FLASH_OPERATION_INTENT_MISMATCH")
    require(result["write_range"] == {"offset": APP_OFFSET, "length": APP_SIZE}, "FLASH_OPERATION_RANGE_INVALID")
    results = archive.results()
    writes = [r for r in results if r.get("status") in ("FLASH_VERIFIED", "RESTORE_VERIFIED")]
    require(writes and writes[-1]["operation_uuid"] == operation_id, "FLASH_OPERATION_SUPERSEDED")
    installation = result["installation_uuid"]
    try:
        require(str(uuid.UUID(installation)) == installation and uuid.UUID(installation).int != 0,
                "INSTALLATION_UUID_INVALID")
    except (ValueError, TypeError, AttributeError):
        raise RecoveryError("INSTALLATION_UUID_INVALID") from None
    # An attempted send consumes the continuation even if its acknowledgment was lost.
    # An unresolved later flash/restore intent also prevents using stale evidence.
    for intent_path in (archive.device_dir / "operations").glob("*/intent.json"):
        intent = read_json(intent_path)
        if intent.get("kind") == "install-init":
            require(intent.get("installation_uuid") != installation, "INSTALLATION_UUID_ALREADY_USED")
        elif intent.get("utc", "") >= prepared.get("utc", "") and intent_path.parent != path:
            require((intent_path.parent / "result.json").exists(), "UNRESOLVED_WRITE_OPERATION")
            require(read_json(intent_path.parent / "result.json").get("status") not in ("FAILED", "WRITING"),
                    "LATER_WRITE_OPERATION_FAILED")
    return result, path


def installation_header(mac, installation, baseline):
    raw = (b"ZTIN" + struct.pack("<HH", 1, 80) + bytes.fromhex(mac) + bytes(2)
           + uuid.UUID(installation).bytes + bytes.fromhex(baseline)
           + struct.pack("<III", 0x3f1000, 0xf000, 1))
    return raw + struct.pack("<I", zlib.crc32(raw) & 0xffffffff)


def info_checked(connection, original, factory_digest=None):
    info = connection.request("info")
    mac = original["identity"]["factory_mac"]
    require(info.get("mac") == mac and info.get("chip") == "ESP32-C3"
            and type(info.get("chip_revision")) is int
            and info["chip_revision"] == original["identity"]["revision"], "CONSOLE_DEVICE_MISMATCH")
    layout_fields = {"protocol_version": 1, "flash_bytes": FLASH_SIZE, "factory_offset": APP_OFFSET,
                     "factory_bytes": APP_SIZE, "install_offset": TAIL_OFFSET,
                     "nvs_offset": 0x3f1000, "nvs_bytes": 0xf000}
    require(info.get("layout") == "stock-v1" and all(type(info.get(k)) is int and info[k] == v
                                                    for k, v in layout_fields.items()),
            "CONSOLE_LAYOUT_OR_PROTOCOL_MISMATCH")
    hex_value(info.get("factory_sha256"), 32)
    if factory_digest is not None:
        require(info["factory_sha256"] == factory_digest, "RUNNING_FACTORY_DIGEST_MISMATCH")
    require(info.get("install_state") in ("WAIT_INSTALL", "VALID", "ERROR")
            and info.get("config_status") in ("absent", "present", "error", "unopened")
            and type(info.get("tail_blank")) is bool, "CONSOLE_INSTALL_STATUS_INVALID")
    hex_value(info.get("active_round"), 8)
    return info


def install(args, archive, operation, original):
    from .release import verify_release
    from .console import Console
    result, path = verified_flash(archive, args.operation)
    gates.identity(result["identity"], original["identity"]).enforce()
    gates.security(result["identity"]).enforce()
    current = gates.backup(archive, result["current_snapshot"]).enforce().evidence["manifest"]
    gates.identity(current["identity"], original["identity"]).enforce()
    require(original["reads"][0]["sha256"] == result["original_hash"]
            and current["reads"][0]["sha256"] == result["current_hash"], "FLASH_BASELINE_MISMATCH")
    release_path = release_path_from_operator(args.release)
    release = verify_release(release_path, original["identity"]["revision"])
    require(hash_file(release_path) == result["release_hash"], "FLASH_RELEASE_MANIFEST_MISMATCH")
    gates.rehearsal(archive).enforce()
    from . import layout
    baseline = (archive.snapshot_path(current["snapshot_id"]) / "flash.bin").read_bytes()
    original_image = (archive.snapshot_path("original") / "flash.bin").read_bytes()
    require(all(layout.inspect(image)["LAYOUT_SUPPORTED"] and layout.inspect(image)["tail_blank"]
                for image in (baseline, original_image)), "INSTALL_BASELINE_LAYOUT_OR_TAIL_INVALID")
    padded = (release_path.parent / release["padded_app"]).read_bytes()
    expected = baseline[:APP_OFFSET] + padded + baseline[APP_OFFSET + APP_SIZE:]
    require((path / "expected.bin").read_bytes() == expected
            and equal_files(path / "expected.bin", path / "post-write.bin")
            and hash_file(path / "expected.bin") == result["expected_hash"] == result["preboot_sha256"],
            "FLASH_READBACK_EVIDENCE_INVALID")
    expected_header = installation_header(original["identity"]["factory_mac"], result["installation_uuid"], result["original_hash"])
    with Console(args.port) as connection:
        info = info_checked(connection, original, release["padded_sha256"])
        require(info["install_state"] == "WAIT_INSTALL" and info["tail_blank"] is True
                and info.get("header_hex") is None and info["config_status"] in ("absent", "unopened"),
                "INSTALL_REQUIRES_BLANK_WAIT_INSTALL")
        intent = {"kind": "install-init", "operation_uuid": operation.id, "status": "PREPARED", "utc": utc(),
                  "flash_operation": args.operation, "installation_uuid": result["installation_uuid"],
                  "mac": info["mac"], "factory_sha256": release["padded_sha256"],
                  "release_hash": result["release_hash"], "expected_hash": hashlib.sha256(expected_header).hexdigest()}
        operation.prepare(intent)
        operation.writing(intent)
        response = connection.request("install_init", expected_mac=info["mac"], schema=1,
                                      installation_uuid=result["installation_uuid"], baseline_sha256=result["original_hash"])
        require(response.get("header_hex") == expected_header.hex() and response.get("install_state") == "VALID"
                and response.get("config_status") == "absent", "INSTALL_COMMIT_READBACK_MISMATCH")
        after = info_checked(connection, original, release["padded_sha256"])
        require(after.get("header_hex") == expected_header.hex() and after["install_state"] == "VALID"
                and after["config_status"] == "absent" and after["tail_blank"] is False,
                "INSTALL_COMMIT_READBACK_MISMATCH")
    operation.finish("INSTALL_INITIALIZED", flash_operation=args.operation,
                     installation_uuid=result["installation_uuid"], factory_sha256=release["padded_sha256"],
                     header_sha256=hashlib.sha256(expected_header).hexdigest(), config_status="absent")
    return "INSTALL_INITIALIZED. Installation header and absent configuration verified."


def load_provision(args, archive, original):
    path = private_path(args.config)
    require(path.stat().st_size <= 16384, "CONFIG_FILE_TOO_LONG")
    try:
        with path.open("rb") as stream:
            raw = stream.read(16385)
        require(len(raw) <= 16384, "CONFIG_FILE_TOO_LONG")
        data = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs_unique,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (ValueError, UnicodeError, RecursionError, RecoveryError):
        raise RecoveryError("PRIVATE_CONFIG_INVALID") from None
    require(type(data) is dict and set(data) == {"game_id", "group_key", "host_mac", "last_channel",
            "provisioned_time_ms", "badges", "host"}, "PRIVATE_CONFIG_FIELDS_INVALID")
    game = hex_value(data["game_id"], 8)
    key = hex_value(data["group_key"], 32)
    require(any(game) and any(key), "CONFIG_ID_OR_KEY_INVALID")
    host_mac = mac_value(data["host_mac"])
    require(type(data["last_channel"]) is int and 1 <= data["last_channel"] <= 11, "CONFIG_CHANNEL_INVALID")
    require(type(data["provisioned_time_ms"]) is int and 0 <= data["provisioned_time_ms"] <= 9007199254740991,
            "CONFIG_TIME_INVALID")
    name = bounded_text(args.name, 1, 12, True)
    badges = data["badges"]
    require(type(badges) is list and 1 <= len(badges) <= 20, "CONFIG_BADGE_ROSTER_INVALID")
    roster = {}
    for badge in badges:
        require(type(badge) is dict and set(badge) == {"mac", "host"} and type(badge["host"]) is bool,
                "CONFIG_BADGE_ROSTER_INVALID")
        mac = mac_value(badge["mac"])
        require(mac not in roster, "CONFIG_DUPLICATE_BADGE")
        roster[mac] = badge["host"]
    require([mac for mac, host in roster.items() if host] == [host_mac], "CONFIG_REQUIRES_EXACTLY_ONE_HOST")
    self_mac = original["identity"]["factory_mac"]
    require(self_mac in roster and roster[self_mac] == args.host, "CONFIG_HOST_SELECTION_MISMATCH")
    from .index import scan
    enrolled = {mac: rid for rid, mac in scan(archive.root).items()}
    for mac in roster:
        require(mac in enrolled, "CONFIG_BADGE_NOT_ENROLLED")
        peer = Archive(archive.config, enrolled[mac])
        manifest = gates.backup(peer, "original").enforce().evidence["manifest"]
        require(manifest["identity"]["factory_mac"] == mac, "CONFIG_ENROLLMENT_MISMATCH")
    fields = {k: data[k] for k in ("game_id", "group_key", "host_mac", "last_channel", "provisioned_time_ms")}
    fields.update(expected_mac=self_mac, name=name, host_credentials_present=args.host)
    host = data["host"]
    require(type(host) is dict and set(host) == {"https_base", "ssid", "password", "token"}, "CONFIG_HOST_FIELDS_INVALID")
    url = bounded_text(host["https_base"], 9, 192, True)
    require(url.startswith("https://") and url[8] not in "/:?"
            and not any(ord(c) <= 32 or c in "@#\\" for c in url[8:]), "CONFIG_HTTPS_URL_REQUIRED")
    bounded_text(host["ssid"], 1, 32)
    bounded_text(host["password"], 0, 63)
    bounded_text(host["token"], 1, 256, True)
    if args.host:
        fields.update(host)
    # Check the encoded line before any connection, at the largest request ID.
    from .console import wire_request
    wire_request(0xffffffff, "configure", fields)
    return fields


def configuration_hash(fields):
    """Hash the 622-byte field encoding used by schema 1, excluding blob framing."""
    def fixed(name, length):
        return fields.get(name, "").encode("utf-8").ljust(length, b"\0")
    body = (bytes.fromhex(fields["expected_mac"]) + fixed("name", 13)
            + struct.pack("<Q", int(fields["game_id"], 16)) + bytes.fromhex(fields["group_key"])
            + bytes.fromhex(fields["host_mac"]) + bytes([fields["last_channel"], fields["host_credentials_present"]])
            + fixed("https_base", 193) + fixed("ssid", 33) + fixed("password", 64) + fixed("token", 257)
            + struct.pack("<Q", fields["provisioned_time_ms"]))
    require(len(body) == 622, "CONFIG_HASH_ENCODING_INVALID")
    return hashlib.sha256(body).hexdigest()


def provision(args, archive, operation, original):
    from .console import Console
    fields = load_provision(args, archive, original)
    digest = configuration_hash(fields)
    with Console(args.port) as connection:
        info = info_checked(connection, original)
        require(info["install_state"] == "VALID" and info.get("admission") in
                ("NEEDS_CONFIG", "LOBBY", "REGISTERING", "WAITING_FOR_ROUND")
                and info["active_round"] == "0000000000000000" and info.get("pending_round") is False,
                "CONFIGURATION_FROZEN")
        # Identity and release are rechecked on the same connection immediately
        # before the only secret-bearing write. Never reconnect on reboot/lost ack.
        after = info_checked(connection, original, info["factory_sha256"])
        require(after.get("header_hex") == info.get("header_hex") and after["install_state"] == "VALID",
                "INSTALLATION_CHANGED")
        operation.event("CONFIGURE_REQUESTED", config_sha256=digest)
        response = connection.request("configure", **fields)
        require(set(response) == {"id", "ok", "config_sha256"} and response["config_sha256"] == digest,
                "CONFIGURATION_HASH_MISMATCH")
    operation.finish("CONFIGURED", config_sha256=digest)
    return "CONFIGURED. SHA-256 " + digest


def diagnose(args, archive, operation, original):
    from .console import Console
    with Console(args.port) as connection:
        info = info_checked(connection, original)
        status = connection.request("status")
    # Persist only explicitly allowed diagnostics; never a raw device/config dump.
    counters = ("rx_drops", "tx_drops", "invalid_frames", "auth_failures", "dedupe_hits", "rx_high_water",
                "tx_high_water", "event_high_water", "input_drops", "tx_watchdogs", "radio_restarts",
                "gateway_drops", "gateway_reconnects", "replay_backlog", "free_heap", "minimum_heap",
                "largest_free_block", "clock_uncertainty_ms", "gateway_age_ms", "channel", "pending_event_count")
    safe = {}
    for key in counters:
        require(type(status.get(key)) is int and 0 <= status[key] <= 0xffffffff, "DIAGNOSTICS_COUNTER_INVALID")
        safe[key] = status[key]
    require(type(status.get("diagnostic_mode")) is bool, "DIAGNOSTICS_FLAG_INVALID")
    safe["diagnostic_mode"] = status["diagnostic_mode"]
    for key in ("peer_ages_ms", "pending_event_ids"):
        require(type(status.get(key)) is list and len(status[key]) <= 20, "DIAGNOSTICS_PAGE_INVALID")
        safe[key] = status[key]
    require(all(type(v) is int and 0 <= v <= 0xffffffff for v in safe["peer_ages_ms"]), "DIAGNOSTICS_AGE_INVALID")
    require(all(type(v) is str and re.fullmatch(r"[0-9a-f]{16}/[0-9a-f]{2}/[0-9a-f]{4}", v)
                for v in safe["pending_event_ids"]), "DIAGNOSTICS_EVENT_INVALID")
    require(type(status.get("pending_ids_complete")) is bool, "DIAGNOSTICS_PAGE_INVALID")
    safe["pending_ids_complete"] = status["pending_ids_complete"]
    require(len(safe["pending_event_ids"]) <= safe["pending_event_count"]
            and (not safe["pending_ids_complete"] or len(safe["pending_event_ids"]) == safe["pending_event_count"]),
            "DIAGNOSTICS_PENDING_COUNT_MISMATCH")
    json_new(operation.path / "diagnostics.json", safe)
    operation.finish("DIAGNOSED", factory_sha256=info["factory_sha256"], install_state=info["install_state"])
    return (f"DIAGNOSED. Channel {safe['channel']}; pending events {safe['pending_event_count']}; "
            f"free/minimum heap {safe['free_heap']}/{safe['minimum_heap']}. Private diagnostics recorded.")


def export_game(args, archive, operation, original):
    from .console import Console
    output = private_path(args.output, existing=False)
    receipt_path = output.with_name(output.name + ".receipt.json")
    require(not receipt_path.exists() and not receipt_path.is_symlink(), "DESTINATION_EXISTS")
    if args.round:
        require(any(hex_value(args.round, 8)), "EXPORT_ROUND_INVALID")
    with Console(args.port) as connection:
        info = info_checked(connection, original)
        selected = args.round or info["active_round"]
        require(selected != "0000000000000000", "NO_ACTIVE_ROUND_NAME_ONE_EXPLICITLY")
        records = bytearray()
        identities = set()
        cursor = 0
        frontiers = None
        for _ in range(129):
            page = connection.request("game_export", round_id=selected, cursor=cursor)
            require(page.get("round_id") == selected and page.get("cursor") == cursor
                    and page.get("scope") == "single_round" and page.get("retained_set_complete") is False
                    and page.get("limitation") == "PREVIOUS_ROUND_ENUMERATION_UNAVAILABLE", "EXPORT_SCOPE_INVALID")
            require(type(page.get("next_cursor")) is int and cursor <= page["next_cursor"] <= 128,
                    "EXPORT_CURSOR_INVALID")
            require(type(page.get("records_hex")) is str and len(page["records_hex"]) <= 1024
                    and len(page["records_hex"]) % 128 == 0, "EXPORT_PAGE_INVALID")
            raw = hex_value(page["records_hex"], len(page["records_hex"]) // 2)
            for start in range(0, len(raw), 64):
                record = raw[start:start + 64]
                identity = (record[16], int.from_bytes(record[17:19], "little"))
                require(record[:8] == b"ZTEV\x01\x00\x40\x00"
                        and int.from_bytes(record[8:16], "little") == int(selected, 16)
                        and record[46:60] == bytes(14)
                        and zlib.crc32(record[:60]) & 0xffffffff == int.from_bytes(record[60:64], "little"),
                        "EXPORT_RECORD_INVALID")
                require(identity[0] < 20 and identity[1] > 0 and identity not in identities,
                        "EXPORT_EVENT_ID_INVALID")
                identities.add(identity)
            vectors = []
            for key in ("produced", "decided"):
                vector = page.get(key)
                require(type(vector) is list and len(vector) == 20
                        and all(type(v) is int and 0 <= v <= 65535 for v in vector), "EXPORT_FRONTIER_INVALID")
                vectors.append(vector)
            if frontiers is None:
                frontiers = vectors
            require(vectors == frontiers, "EXPORT_CHANGED_RETRY_REQUIRED")
            records.extend(raw)
            require(len(records) <= 128 * 64, "EXPORT_JOURNAL_TOO_LONG")
            next_cursor = page["next_cursor"]
            if next_cursor == 128:
                break
            require(next_cursor > cursor, "EXPORT_CURSOR_STALLED")
            cursor = next_cursor
        else:
            raise RecoveryError("EXPORT_PAGE_LIMIT")
        after = info_checked(connection, original, info["factory_sha256"])
        require(after.get("header_hex") == info.get("header_hex"), "EXPORT_INSTALLATION_CHANGED")
    digest = hashlib.sha256(records).hexdigest()
    write_new(output, records)
    require(hash_file(output) == digest, "EXPORT_LOCAL_READBACK_MISMATCH")
    receipt = {"schema": 1, "mac": original["identity"]["factory_mac"], "round_id": selected,
               "factory_sha256": info["factory_sha256"], "scope": "single_round", "retained_set_complete": False,
               "limitation": "PREVIOUS_ROUND_ENUMERATION_UNAVAILABLE", "produced": frontiers[0],
               "decided": frontiers[1], "export_sha256": digest, "bytes": len(records), "utc": utc()}
    json_new(receipt_path, receipt)
    json_new(operation.path / "export-receipt.json", receipt)
    operation.finish("GAME_ROUND_EXPORTED", export_sha256=digest, round_id=selected, retained_set_complete=False)
    return "GAME_ROUND_EXPORTED. Single round only; previous-round enumeration unavailable. Private receipt written beside export."


def execute(args):
    archive = Archive(config.load(), args.recovery_id)
    with archive.lock():
        operation = archive.operation(args.command)
        try:
            original = gates.backup(archive, "original").enforce().evidence["manifest"]
            confirm("Do you confirm participant agreement and the selected badge is running the custom application with START released? [y/N]: ")
            handlers = {"install-init": install, "provision": provision, "diagnose": diagnose, "game-export": export_game}
            status = handlers[args.command](args, archive, operation, original)
            archive.mirror_operation(operation)
            return status
        except BaseException as error:
            # Never persist arbitrary exception repr: a config decoder may include
            # private input in its exception. R01 records this fixed error only.
            safe = error if isinstance(error, RecoveryError) else RecoveryError("CONSOLE_COMMAND_FAILED")
            operation.failure(safe)
            raise safe from None


def register(subparsers):
    """R01 extension hook: no serial module import, archive access, or I/O."""
    def command(name, description, fields):
        parser = subparsers.add_parser(name, help=description, description=description)
        for field in ("recovery-id", "port", *fields):
            parser.add_argument("--" + field, required=True)
        parser.set_defaults(func=execute)
        return parser
    parser = command("install-init", "Continue an unused verified flash installation over USB.", ("operation",))
    parser.add_argument("--release", help="Absolute local release-manifest path; prompts if omitted. Validation is unchanged.")
    parser = command("provision", "Provision from a private local session file; acknowledge hashes only.", ("config", "name"))
    parser.add_argument("--host", action="store_true")
    command("diagnose", "Read bounded device diagnostics into the private operation record.", ())
    parser = command("game-export", "Export one named or active round; does not enumerate prior rounds.", ("output",))
    parser.add_argument("--round", help="Explicit nonzero round ID, 16 lowercase hex digits; otherwise active round.")
