"""Pinned read-only acquisition adapter. The sole flash call is gate-owned below."""
import contextlib
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid
from . import ESPTOOL_VERSION, FLASH_SIZE, APP_OFFSET, APP_SIZE, TAIL_OFFSET
from .errors import require
from .manifest import write_new, hash_file, read_json, json_new

# ESP32-C3 readable register map, from esptool/espefuse 5.4.0.
# Joint dump is little-endian words in ascending block/address order.
EFUSE_BLOCKS = [(0x02C, 6), (0x044, 6)] + [(offset, 8) for offset in range(0x05C, 0x17C, 0x20)]


def dependency():
    require(importlib.metadata.version("esptool") == ESPTOOL_VERSION, "ESPTOOL_VERSION_MISMATCH")
    import esptool
    return esptool


def ports():
    dependency()
    from serial.tools.list_ports import comports
    return [{"port": p.device, "vid": p.vid, "pid": p.pid,
             "usb_serial_observation": p.serial_number, "identity": False} for p in comports()]


def efuse_capture(port, directory):
    dependency()
    # Fixed read-only argv, never a shell, never a user-selected efuse operation.
    #
    # Two separate invocations, not one chained command. espefuse 5.4.0's `summary`
    # takes variadic EFUSES_TO_SHOW arguments, so chaining `summary ... dump ...`
    # makes it swallow `dump` and then parse `joint` as the next command, exiting 2
    # with "No such command 'joint'". Both subcommands are read-only and both use
    # --before/--after no-reset, so the chip stays in the ROM loader between them
    # and the subsequent owned session still starts from a fresh ROM entry.
    base = [sys.executable, "-m", "espefuse", "--chip", "esp32c3", "--port", port,
            "--before", "no-reset", "--after", "no-reset"]
    steps = (("summary", ["summary", "--format", "json",
                          "--file", str(directory / "efuses.json")]),
             ("dump", ["dump", "--format", "joint",
                       "--file-name", str(directory / "efuses-readable.bin")]))
    require(not (directory / "efuses.json").exists() and not (directory / "efuses-readable.bin").exists(), "EVIDENCE_ALREADY_EXISTS")
    with (directory / "efuse-acquisition.log").open("xb") as log:
        for name, step in steps:
            result = subprocess.run(base + step, stdout=log, stderr=log)
            log.flush()
            os.fsync(log.fileno())
            require(result.returncode == 0, "EFUSE_ACQUISITION_FAILED",
                    returncode=result.returncode, step=name)
    for name in ("efuses.json", "efuses-readable.bin"):
        with (directory / name).open("rb") as stream:
            os.fsync(stream.fileno())
    summary = read_json(directory / "efuses.json")
    require("RD_DIS" in summary, "EFUSE_READ_PROTECTION_MISSING")
    return {"read_disable": summary["RD_DIS"],
            "unreadable_fields": sorted(k for k, v in summary.items() if isinstance(v, dict) and v.get("readable") is False)}


class Session:
    """One exclusive connection; capture/readback/write errors invalidate it."""
    def __init__(self, port, evidence_dir, read_protection):
        self.port = port
        self.evidence_dir = evidence_dir
        self.read_protection = read_protection
        self.token = str(uuid.uuid4())
        self.loader = None
        self.stub = None
        self.serial = None
        self.fresh = None
        self.active = False
        self.written = False

    def __enter__(self):
        self.api = dependency()
        import serial
        from esptool.targets.esp32c3 import ESP32C3ROM
        try:
            self.serial = serial.Serial(port=None, baudrate=115200, timeout=3,
                                        write_timeout=10, exclusive=True)
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.port = self.port
            self.serial.open()
            # TIOCEXCL also rejects non-cooperating new opens where supported.
            import fcntl
            import termios
            require(hasattr(termios, "TIOCEXCL"), "SERIAL_EXCLUSIVE_UNAVAILABLE")
            fcntl.ioctl(self.serial.fileno(), termios.TIOCEXCL)
            from .locking import check_port_holders
            # Catch a non-cooperating holder that opened between the eFuse CLI
            # closing and this exclusive open. New opens are now OS-excluded.
            check_port_holders(self.port, allow_self=True)
            self.loader = ESP32C3ROM(self.serial, baud=115200, trace_enabled=False)
            self.loader.connect(mode="no-reset", attempts=1)
            require(not self.loader.sync_stub_detected, "PREEXISTING_STUB_REQUIRES_FRESH_ROM_ENTRY")
            self.active = True
            self.api.attach_flash(self.loader)
            before = self.identify(self.loader)
            require(before["chip"] == "ESP32-C3" and before["flash_size"] == FLASH_SIZE, "UNSUPPORTED_CHIP_OR_FLASH_SIZE")
            self.stub = self.loader.run_stub()
            # Public loader setting in pinned 5.4.0: write_flash otherwise closes,
            # reconnects and resumes after SerialException. That is prohibited.
            self.stub.WRITE_FLASH_ATTEMPTS = 1
            self.api.attach_flash(self.stub)
            self.identity = self.identify(self.stub)
            require(before == self.identity, "IDENTITY_CHANGED_DURING_CONNECT")
            require(self.identity["efuse_sha256"] == hash_file(self.evidence_dir / "efuses-readable.bin"), "EFUSE_SESSION_EVIDENCE_MISMATCH")
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def efuse_bytes(self, loader):
        return b"".join(struct.pack("<I", loader.read_reg(0x60008800 + offset + 4 * i))
                        for offset, count in EFUSE_BLOCKS for i in range(count))

    def identify(self, loader=None):
        require(self.active, "SESSION_NOT_ACTIVE")
        loader = loader or self.stub
        # Normalize tuples to JSON arrays so live and reopened evidence compare
        # identically; no identity fact is inferred from a USB observation.
        security = json.loads(json.dumps(loader.get_security_info(cache=False)))
        raw = self.efuse_bytes(loader)
        require(len(raw) == 336, "EFUSE_READ_LENGTH_MISMATCH")
        # RD_DIS is bits 0..6 of block0 word1; tie the CLI masks to this session.
        require((struct.unpack_from("<I", raw, 4)[0] & 127)
                == int(self.read_protection["read_disable"]["value"]), "EFUSE_READ_MASK_MISMATCH")
        evidence = self.evidence_dir / ("efuse-live-" + str(uuid.uuid4()) + ".bin")
        write_new(evidence, raw)
        efuse_hash = hash_file(evidence)
        jedec = loader.flash_id(cache=False)
        size_code = (jedec >> 16) & 255
        require(16 <= size_code <= 30, "UNKNOWN_FLASH_CAPACITY_CODE")
        return {"factory_mac": bytes(loader.read_mac()).hex(), "chip": loader.CHIP_NAME,
                "package": loader.get_pkg_version(),
                "revision": loader.get_major_chip_version() * 100 + loader.get_minor_chip_version(),
                "jedec_id": jedec, "flash_size": 1 << size_code,
                "security": security, "efuse_sha256": efuse_hash,
                "read_protection": self.read_protection}

    def read_full(self, destination):
        require(self.active and not destination.exists(), "INVALID_READ_DESTINATION_OR_SESSION")
        try:
            data = self.api.read_flash(self.stub, 0, FLASH_SIZE, flash_size="4MB", no_progress=True)
            write_new(destination, data)
            require(len(data) == FLASH_SIZE, "SHORT_FULL_FLASH_READ")
        except BaseException:
            self.active = False
            raise

    def read_installation_sector(self):
        """One optional fixed-range probe; caller falls back to ordinary capture.

        No reset, reconnect or retry. A failed probe cannot authorize a write;
        the normal full capture must still succeed on this owned connection.
        """
        require(self.active and not self.written, "SESSION_NOT_ACTIVE")
        data = self.api.read_flash(self.stub, TAIL_OFFSET, 4096,
                                  flash_size="4MB", no_progress=True)
        require(len(data) == 4096, "INSTALLATION_HEADER_TRUNCATED")
        return data

    def read_custom_tail(self):
        """Fixed 64 KiB prewrite expectation; never a full backup or write."""
        require(self.active and not self.written, "SESSION_NOT_ACTIVE")
        data = self.api.read_flash(self.stub, TAIL_OFFSET, FLASH_SIZE - TAIL_OFFSET,
                                  flash_size="4MB", no_progress=True)
        require(len(data) == FLASH_SIZE - TAIL_OFFSET, "SHORT_CUSTOM_TAIL_READ")
        return data

    def read_reflash(self, application_destination, tail_destination):
        """Postwrite evidence for the two fixed fast-path ranges only."""
        require(self.active and self.written
                and not application_destination.exists() and not tail_destination.exists(),
                "INVALID_READ_DESTINATION_OR_SESSION")
        try:
            for offset, length, destination in (
                    (APP_OFFSET, APP_SIZE, application_destination),
                    (TAIL_OFFSET, FLASH_SIZE - TAIL_OFFSET, tail_destination)):
                data = self.api.read_flash(self.stub, offset, length,
                                          flash_size="4MB", no_progress=True)
                write_new(destination, data)
                require(len(data) == length, "SHORT_REFLASH_READ", offset=offset, length=length)
        except BaseException:
            self.active = False
            raise

    def __exit__(self, *args):
        try:
            if self.active and self.stub is not None and (not args or args[0] is None):
                # Exact --after no-reset semantics: exit the stub to ROM, never
                # normal boot. Failed/disconnected sessions receive no more I/O.
                self.api.reset_chip(self.stub, reset_mode="no-reset")
        finally:
            self.active = False
            self.fresh = None
            if self.serial is not None:
                self.serial.close()


def guarded_write(session, archive, operation, target, release=None, rehearsal=False):
    """Sole persistent-device mutation entry point; callers cannot supply an offset.

    All gates are rerun here on the live connection and durable files. No reusable
    permit token or approved bit exists. Locks must be held by archive.hardware().
    """
    from . import gates
    from .layout import differing_ranges, PROTECTED
    from .manifest import utc
    require(archive.hardware_session is session and archive.locked, "WRITE_REQUIRES_OWNED_LOCKS")
    require(session.active and not session.written, "FRESH_SESSION_CAPTURE_REQUIRED")
    fast = None
    capture_decision = None
    if release is not None and session.fresh is None:
        # Decide here, under the same locks and connection as the sole write.
        # A historical result or caller-supplied approval cannot skip a capture.
        candidate = gates.fast_reflash(archive, session, release)
        capture_decision = candidate.evidence["decision"]
        if candidate.passed:
            write_new(operation.path / "prewrite-tail.bin", candidate.evidence["tail"])
        json_new(operation.path / "prewrite-capture.json", {
            "session_uuid": session.token, "utc": utc(), **capture_decision})
        if candidate.passed:
            fast = candidate.evidence
        else:
            archive.capture(session, archive.snapshot_manifest("original")["operator_label"], "flash-game")
    if fast is None:
        require(session.fresh is not None, "FRESH_SESSION_CAPTURE_REQUIRED")
        fresh = gates.backup(archive, session.fresh).enforce().evidence["manifest"]
        original = gates.backup(archive, "original").enforce().evidence["manifest"]
        selected = gates.backup(archive, target).enforce().evidence["manifest"]
        live = session.identify()
        for manifest in (fresh, original, selected):
            gates.identity(live, manifest["identity"]).enforce()
        gates.security(live).enforce()
        gates.fresh_capture(session, fresh).enforce()
        baseline = fresh
        expected = (archive.snapshot_path(baseline["snapshot_id"]) / "flash.bin").read_bytes()
        require(hashlib.sha256(expected).hexdigest() == baseline["reads"][0]["sha256"], "CURRENT_SNAPSHOT_CHANGED_AFTER_GATE")
    else:
        original = selected = baseline = fast["original"]
        live = fast["live"]
    if release is not None:
        reviewed = (fast["reviewed"] if fast is not None else
                    gates.application(archive, original, fresh, release, live).enforce().evidence)
        padded = Path(reviewed["padded_path"]).read_bytes()
        require(hashlib.sha256(padded).hexdigest() == reviewed["release"]["padded_sha256"]
                and len(padded) == APP_SIZE, "RELEASE_CHANGED_AFTER_GATE")
        offset, payload = APP_OFFSET, padded
        if fast is None:
            expected = expected[:APP_OFFSET] + padded + expected[APP_OFFSET + APP_SIZE:]
        outcome = "FLASH_VERIFIED"
    else:
        gates.restore_target(selected).enforce()
        offset, payload = 0, (archive.snapshot_path(target) / "flash.bin").read_bytes()
        expected = payload
        outcome = "RESTORE_VERIFIED"
        require(hashlib.sha256(payload).hexdigest() == selected["reads"][0]["sha256"], "RESTORE_TARGET_CHANGED_AFTER_GATE")
    if fast is None:
        require(len(expected) == FLASH_SIZE, "EXPECTED_IMAGE_SIZE_INVALID")
        write_new(operation.path / "expected.bin", expected)
        expected_hash = hash_file(operation.path / "expected.bin")
    else:
        # No whole-image expectation, artifact or hash exists on this path.
        # Operation.writing retains the legacy expected_hash key, explicitly null.
        write_new(operation.path / "expected-application.bin", padded)
        expected_hash = None
    installation_uuid = None
    if release:
        installation_uuid = reviewed["installation_uuid"]
        if reviewed["initialization_required"]:
            installation_uuid = str(uuid.uuid4())
    intent = {"operation_uuid": operation.id, "status": "PREPARED", "utc": utc(),
              "identity": live, "session_uuid": session.token,
              "original_snapshot": original["snapshot_id"], "current_snapshot": baseline["snapshot_id"],
              "target_snapshot": selected["snapshot_id"],
              "original_hash": original["reads"][0]["sha256"], "current_hash": baseline["reads"][0]["sha256"],
              "target_hash": selected["reads"][0]["sha256"], "expected_hash": expected_hash,
              "release_hash": hash_file(Path(release)) if release else None,
              "write_range": {"offset": offset, "length": len(payload)},
              "installation_uuid": installation_uuid, "rehearsal": rehearsal}
    if release:
        intent["baseline_transition"] = reviewed["transition"]
        intent["initialization_required"] = reviewed["initialization_required"]
        intent["installation_header"] = reviewed["installation_header"]
        intent["commissioning_exemptions"] = reviewed["commissioning_exemptions"]
        # Keep the snapshot reference consumed by commissioning without claiming
        # it was captured or used as a stock readback expectation this session.
        intent["current_snapshot_role"] = "recovery_archive_reference_only" if fast else "fresh_prewrite_capture"
        intent["prewrite_capture"] = {
            **(capture_decision or {"skipped": False}),
            "snapshot_id": None if fast else fresh["snapshot_id"]}
        intent["expectation_sources"] = {
            "application": {"source": "release_padded_application", "offset": APP_OFFSET,
                            "length": APP_SIZE, "sha256": reviewed["release"]["padded_sha256"]},
            "tail": (capture_decision["tail_expectation"] if fast else
                     {"snapshot_id": fresh["snapshot_id"], "offset": TAIL_OFFSET,
                      "length": FLASH_SIZE - TAIL_OFFSET,
                      "sha256": hashlib.sha256(expected[TAIL_OFFSET:]).hexdigest()})}
        if fast is None:
            intent["expectation_sources"]["stock"] = {
                "snapshot_id": baseline["snapshot_id"], "ranges": PROTECTED,
                "sha256": baseline["reads"][0]["sha256"]}
        else:
            intent["verification_scope"] = {
                "mode": "application_and_tail", "full_flash_verified": False,
                "comparison": "bytes_and_sha256",
                "stock_regions": {"ranges": PROTECTED, "range_convention": "start_inclusive_end_exclusive",
                                  "written": False, "read_back": False, "verified": False}}
            intent["verification_ranges"] = [
                {"name": "application", "offset": APP_OFFSET, "length": APP_SIZE,
                 "expected_path": "expected-application.bin", "readback_path": "post-write-application.bin",
                 "expected_sha256": reviewed["release"]["padded_sha256"]},
                {"name": "tail", "offset": TAIL_OFFSET, "length": FLASH_SIZE - TAIL_OFFSET,
                 "expected_path": "prewrite-tail.bin", "readback_path": "post-write-tail.bin",
                 "expected_sha256": capture_decision["tail_expectation"]["sha256"]}]
    operation.prepare(intent)
    operation.writing(intent)
    session.written = True
    session.fresh = None
    try:
        require(session.stub.WRITE_FLASH_ATTEMPTS == 1, "AUTOMATIC_WRITE_RETRY_PROHIBITED")
        session.api.write_flash(session.stub, [(offset, payload)], flash_mode="keep",
                                flash_freq="keep", flash_size="keep", no_progress=True)
        if fast is None:
            session.read_full(operation.path / "post-write.bin")
            actual = (operation.path / "post-write.bin").read_bytes()
            actual_hash = hash_file(operation.path / "post-write.bin")
            diffs = differing_ranges(expected, actual)
            require(not diffs and expected_hash == actual_hash, "WRITE_VERIFICATION_FAILED",
                    differing_ranges=diffs, expected_sha256=expected_hash, actual_sha256=actual_hash)
            verification = {"preboot_sha256": actual_hash, "original_preserved": True,
                            "exact_claim": "Verified before any application boot"}
        else:
            session.read_reflash(operation.path / "post-write-application.bin",
                                 operation.path / "post-write-tail.bin")
            range_results, diffs = [], []
            for item, expected_bytes in zip(intent["verification_ranges"], (padded, fast["tail"])):
                actual = (operation.path / item["readback_path"]).read_bytes()
                actual_hash = hash_file(operation.path / item["readback_path"])
                relative_diffs = differing_ranges(expected_bytes, actual)
                diffs.extend([[start + item["offset"], end + item["offset"]]
                              for start, end in relative_diffs])
                range_results.append({**item, "actual_sha256": actual_hash,
                                      "bytes_equal": not relative_diffs,
                                      "sha256_equal": item["expected_sha256"] == actual_hash})
            # Retain both comparisons even if one fails; diagnostics use absolute
            # flash addresses and each range has its own expected/observed hash.
            json_new(operation.path / "readback-verification.json", {
                "scope": intent["verification_scope"], "ranges": range_results})
            failed = [item for item in range_results if not (item["bytes_equal"] and item["sha256_equal"])]
            if failed:
                require(False, "WRITE_VERIFICATION_FAILED", differing_ranges=diffs,
                        expected_sha256=failed[0]["expected_sha256"], actual_sha256=failed[0]["actual_sha256"],
                        range_verification=range_results, verification_scope=intent["verification_scope"])
            verification = {
                "preboot_sha256": None, "range_verification": range_results,
                "original_archive_preserved": True,
                "exact_claim": "Application and tail verified before any application boot; stock regions were neither written nor re-verified"}
        gates.identity(session.identify(), live).enforce()
        operation.finish(outcome, **intent_without_status(intent),
                         **verification, efuse_comparison=True)
        return outcome
    except BaseException:
        session.active = False
        raise


def intent_without_status(intent):
    return {k: v for k, v in intent.items() if k not in ("status", "utc", "operation_uuid")}
