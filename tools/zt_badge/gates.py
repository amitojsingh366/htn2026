"""Structured gates; pass results are never reusable write permits."""
from pathlib import Path
import hashlib
import struct
import zlib
from . import FLASH_SIZE, APP_OFFSET, APP_SIZE, TAIL_OFFSET, revision
from . import layout
from .errors import check, RecoveryError, require
from .manifest import hash_file, read_json


def backup(archive, snapshot):
    try:
        return check("DURABLE_INDEPENDENT_READS_VERIFIED", True, manifest=archive.verify(snapshot))
    except RecoveryError as error:
        return check(error.condition, False, **error.evidence)


def fresh_capture(session, manifest):
    return check("FRESH_UNINTERRUPTED_CAPTURE_REQUIRED",
                 session.active and not session.written and session.fresh is not None
                 and manifest["session_uuid"] == session.token
                 and manifest["snapshot_id"] == session.fresh)


def identity(live, archived):
    keys = ("factory_mac", "chip", "package", "revision", "jedec_id", "flash_size",
            "security", "efuse_sha256", "read_protection")
    for key in keys:
        if live.get(key) != archived.get(key):
            return check("LIVE_IDENTITY_MISMATCH", False, field=key)
    return check("LIVE_IDENTITY_MATCH", True)


def security(live):
    state = live.get("security", {})
    flags = state.get("parsed_flags", {})
    for key in ("SECURE_BOOT_EN", "SECURE_DOWNLOAD_ENABLE", "SOFT_DIS_JTAG", "HARD_DIS_JTAG", "DIS_USB"):
        if flags.get(key) is not False:
            return check("UNSUPPORTED_SECURITY_STATE", False, field=key)
    return check("SUPPORTED_SECURITY_STATE", state.get("flash_crypt_cnt") == 0
                 and live.get("chip") == "ESP32-C3" and live.get("flash_size") == FLASH_SIZE)


def restore_target(manifest):
    # Restoring a captured image must not depend on its bootability or partition
    # map. It is an exact same-device byte restoration, including damaged states.
    return check("RESTORE_TARGET_FULL_IMAGE", manifest["identity"]["flash_size"] == FLASH_SIZE
                 and all(r["length"] == FLASH_SIZE for r in manifest["reads"]))


def rehearsal(archive):
    for result_path in archive.root.glob("ZT-*/operations/*/result.json"):
        result = read_json(result_path)
        if result.get("status") != "STOCK_BOOT_CONFIRMED" or not result.get("rehearsal_accepted"):
            continue
        if result.get("tool") != revision():
            continue
        restore_path = result_path.parent.parent / result["restore_operation"] / "result.json"
        restored = read_json(restore_path)
        if restored.get("status") == "RESTORE_VERIFIED" and restored.get("rehearsal") and restored.get("tool") == revision():
            return check("RESTORE_REHEARSAL_ACCEPTED", True, device_directory=result_path.parents[2].name)
    return check("RESTORE_REHEARSAL_NOT_ACCEPTED_FOR_THIS_TOOL", False)


def flashing_installation_header(sector, mac, original_hash, allowed_uuids):
    """Flashing-only classification; commissioning retains the strict parser."""
    require(len(sector) == 4096, "INSTALLATION_HEADER_TRUNCATED")
    require(sector[:4] == b"ZTIN" and struct.unpack_from("<HH", sector, 4) == (1, 80),
            "INVALID_INSTALLATION_HEADER")
    require(sector[8:14] == bytes.fromhex(mac) and sector[14:16] == b"\0\0",
            "INSTALLATION_MAC_MISMATCH")
    nvs_offset, nvs_length, flags = struct.unpack_from("<III", sector, 64)
    require((nvs_offset, nvs_length) == (0x3F1000, 0xF000) and flags in (1, 3),
            "INSTALLATION_RANGE_MISMATCH")
    require(zlib.crc32(sector[:76]) == struct.unpack_from("<I", sector, 76)[0],
            "INSTALLATION_CRC_MISMATCH")
    require(sector[80:] == b"\xff" * (4096 - 80), "INSTALLATION_SECTOR_NOT_RESERVED")
    if flags == 3:
        require(sector[16:32] == bytes(16) and sector[32:64] == bytes(32),
                "INVALID_SELF_INSTALLED_HEADER")
        installation_uuid = None
        kind = "self_installed"
    else:
        installation_uuid = layout.installation_header(
            b"\xff" * TAIL_OFFSET + sector, mac, original_hash, allowed_uuids)
        kind = "tool_installed"
    return {"kind": kind, "magic": "ZTIN", "schema": 1, "length": 80,
            "mac": mac, "reserved_zero": True, "flags": flags,
            "installation_uuid": installation_uuid, "uuid_hex": sector[16:32].hex(),
            "baseline_sha256": sector[32:64].hex(),
            "backup_binding_verified": kind == "tool_installed",
            "nvs_offset": nvs_offset, "nvs_length": nvs_length,
            "initialized": True, "crc32_covered_bytes": 76, "crc32_valid": True,
            "remaining_sector_erased": True, "sector_hex": sector.hex()}


def fast_reflash(archive, session, release_path):
    """Optional capture optimization, evaluated only inside the owned write gate.

    Failure is not write authorization or a new refusal: the caller must capture
    normally and run the ordinary gates. Never reconnect or retry this probe.
    """
    evidence = {"skipped": False, "conditions": {}}
    try:
        original = backup(archive, "original").enforce().evidence["manifest"]
        evidence["conditions"]["original_backup_both_media"] = True
        live = session.identify()
        identity(live, original["identity"]).enforce()
        evidence["conditions"]["full_identity_match"] = live
        security(live).enforce()
        evidence["conditions"]["security_supported"] = True
        accepted = rehearsal(archive).enforce()
        evidence["conditions"]["rehearsal"] = {"tool": revision(), **accepted.evidence}
        sector = session.read_installation_sector()
        evidence["installation_header"] = {"offset": TAIL_OFFSET, "length": len(sector),
                                           "sector_hex": sector.hex()}
        # The flashing-only parser accepts precisely tool-issued or self-issued
        # headers. It never grants commissioning to the latter.
        reviewed = application(archive, original, original, release_path, live,
                               installation_sector=sector).enforce().evidence
        evidence["conditions"]["installation_header"] = reviewed["installation_header"]
        evidence["conditions"]["application_gate"] = reviewed
        evidence["recovery_snapshot"] = {
            "snapshot_id": "original", "sha256": original["reads"][0]["sha256"],
            "purpose": "recovery_archive_not_live_stock_expectation",
            "manifest_sha256": hash_file(archive.snapshot_path("original") / "manifest.json")}
        # Last device read before the write: preserve the entire mutable custom
        # tail, and tie it to the header that authorized the fast path. Any short
        # read, I/O failure or changed header selects ordinary capture instead.
        tail = session.read_custom_tail()
        require(tail[:4096] == sector, "INSTALLATION_HEADER_CHANGED_DURING_PROBE")
        evidence["tail_expectation"] = {
            "source": "same_session_prewrite_read", "path": "prewrite-tail.bin",
            "offset": TAIL_OFFSET, "length": len(tail),
            "sha256": hashlib.sha256(tail).hexdigest()}
        evidence["skipped"] = True
        return check("FAST_REFLASH_ELIGIBLE", True, decision=evidence,
                     original=original, live=live, reviewed=reviewed, tail=tail)
    except Exception as error:
        # Even malformed optional evidence or a failed header read only selects
        # the ordinary path. Its capture and mandatory gates still fail closed.
        evidence["fallback_condition"] = getattr(error, "condition", "FAST_REFLASH_PROBE_FAILED")
        return check("FAST_REFLASH_INELIGIBLE", False, decision=evidence)


def application(archive, original, current, release_path, live, installation_sector=None):
    from .release import verify_release
    try:
        release = verify_release(Path(release_path), live["revision"])
        release_hash = hash_file(Path(release_path))
        for path in archive.root.glob("ZT-*/operations/*/result.json"):
            result = read_json(path)
            require(not (result.get("condition") == "COMMISSION_PROTECTED_BYTES_CHANGED"
                         and result.get("evidence", {}).get("release_hash") == release_hash),
                    "RELEASE_ROLLOUT_STOPPED_BY_PROTECTED_REGION_FAILURE")
        rehearsal_result = rehearsal(archive).enforce()
        original_image = (archive.snapshot_path("original") / "flash.bin").read_bytes()
        current_image = ((archive.snapshot_path(current["snapshot_id"]) / "flash.bin").read_bytes()
                         if installation_sector is None else None)
        # Fast eligibility inspects the verified archive's layout. It does not
        # re-read or assert the current contents of stock regions on the device.
        for image in ((original_image, current_image) if current_image is not None else (original_image,)):
            require(layout.inspect(image)["LAYOUT_SUPPORTED"], "STOCK_LAYOUT_UNSUPPORTED")
        # The immutable original must authorize use of an unoccupied tail forever.
        require(layout.inspect(original_image)["tail_blank"], "ORIGINAL_TAIL_NOT_BLANK")
        results = archive.results()
        writes = [r for r in results if r.get("status") in ("FLASH_VERIFIED", "RESTORE_VERIFIED")]
        latest = writes[-1] if writes else None
        sector = (current_image[TAIL_OFFSET:TAIL_OFFSET + 4096]
                  if installation_sector is None else installation_sector)
        header = None
        if sector != b"\xff" * 4096:
            uuids = [r["installation_uuid"] for r in writes if r.get("installation_uuid")]
            header = flashing_installation_header(sector, live["factory_mac"],
                                                  original["reads"][0]["sha256"], uuids)
        self_installed = header is not None and header["kind"] == "self_installed"
        first_or_stock = not self_installed and (latest is None or latest["status"] == "RESTORE_VERIFIED")
        installation_uuid = header["installation_uuid"] if header else None
        commissioning_exemptions = []
        if installation_sector is not None:
            require(header is not None and not first_or_stock, "CUSTOM_INSTALL_HISTORY_REQUIRED")
        if self_installed:
            # Development fleet: the header proves no backup binding and cannot
            # earn commissioning. Do not invent an issued UUID or require a
            # commissioning history that this installation can never produce.
            commissioning_exemptions = [
                {"gate": "PREVIOUS_CUSTOM_INSTALL_NOT_COMMISSIONED",
                 "reason": "VALID_SELF_INSTALLED_DEVELOPMENT_FLEET_HEADER",
                 "backup_binding_verified": False}]
            if current_image is not None:
                diffs = layout.differing_ranges(original_image, current_image, layout.PROTECTED)
                require(not diffs, "PROTECTED_STOCK_CHANGED_DURING_CUSTOM_USE", differing_ranges=diffs)
        elif first_or_stock:
            require(layout.inspect(current_image)["tail_blank"], "FIRST_INSTALL_TAIL_NOT_BLANK")
            if latest:
                require(any(r.get("status") == "STOCK_BOOT_CONFIRMED" and r.get("restore_operation") == latest["operation_uuid"] for r in results), "INTERVENING_STOCK_BOOT_NOT_CONFIRMED")
        else:
            require(header is not None, "INVALID_INSTALLATION_HEADER")
            commissions = [r for r in results if r.get("status") == "COMMISSIONED" and r.get("flash_operation") == latest["operation_uuid"]]
            require(commissions, "PREVIOUS_CUSTOM_INSTALL_NOT_COMMISSIONED")
            baseline_id = latest["current_snapshot"]
            archive.verify(baseline_id)
            baseline = (archive.snapshot_path(baseline_id) / "flash.bin").read_bytes()
            if current_image is not None:
                diffs = layout.differing_ranges(baseline, current_image, layout.PROTECTED)
                require(not diffs, "PROTECTED_STOCK_CHANGED_DURING_CUSTOM_USE", differing_ranges=diffs)
        # Participant rollout requires both roles on the first device. A recorded
        # self-installed first device cannot earn commissioning, even when the
        # current badge is still stock; its own accepted flash is the evidence.
        first_device = rehearsal_result.evidence["device_directory"]
        if archive.recovery_id != first_device:
            roles = set()
            first_device_evidence = None
            for path in sorted((archive.root / first_device / "operations").glob("*/result.json")):
                result = read_json(path)
                if result.get("status") == "COMMISSIONED" and result.get("tool") == revision() and result.get("release_hash") == release_hash:
                    roles.update(result.get("observed_roles", []))
                if result.get("status") == "FLASH_VERIFIED" and first_device_evidence is None:
                    exemption = next((entry for entry in result.get("commissioning_exemptions", [])
                                      if entry.get("reason") == "VALID_SELF_INSTALLED_DEVELOPMENT_FLEET_HEADER"
                                      and entry.get("gate") in (
                                          "PREVIOUS_CUSTOM_INSTALL_NOT_COMMISSIONED",
                                          "FIRST_BADGE_PLAYER_AND_HOST_COMMISSIONING_REQUIRED")), None)
                    if exemption is not None:
                        first_device_evidence = {
                            "device_directory": first_device,
                            "operation_uuid": result["operation_uuid"],
                            "result_path": str(path.relative_to(archive.root)),
                            "result_sha256": hash_file(path),
                            "recorded_exemption": exemption}
            if first_device_evidence is not None:
                commissioning_exemptions.append({
                    "gate": "FIRST_BADGE_PLAYER_AND_HOST_COMMISSIONING_REQUIRED",
                    "reason": "FIRST_DEVICE_SELF_INSTALLED_DEVELOPMENT_FLEET",
                    "evidence": first_device_evidence})
            else:
                require({"player", "host"} <= roles, "FIRST_BADGE_PLAYER_AND_HOST_COMMISSIONING_REQUIRED")
        return check("APPLICATION_WRITE_GATE", True, padded_path=str(Path(release_path).parent / release["padded_app"]),
                     transition="STOCK_TO_CUSTOM_NEW_PREFLASH_BASELINE" if first_or_stock else "CUSTOM_TO_CUSTOM",
                     installation_uuid=installation_uuid, initialization_required=first_or_stock,
                     installation_header=header, commissioning_exemptions=commissioning_exemptions,
                     release=release)
    except RecoveryError as error:
        return check(error.condition, False, **error.evidence)


def commissioning(archive, current, flash_result, release_path):
    from .release import verify_release
    try:
        release = verify_release(Path(release_path), current["identity"]["revision"])
        require(hash_file(Path(release_path)) == flash_result["release_hash"], "COMMISSION_RELEASE_MISMATCH")
        baseline = archive.verify(flash_result["current_snapshot"])
        identity(current["identity"], baseline["identity"]).enforce()
        first = (archive.snapshot_path(baseline["snapshot_id"]) / "flash.bin").read_bytes()
        second = (archive.snapshot_path(current["snapshot_id"]) / "flash.bin").read_bytes()
        diffs = layout.differing_ranges(first, second, layout.PROTECTED)
        require(not diffs, "COMMISSION_PROTECTED_BYTES_CHANGED", differing_ranges=diffs,
                release_hash=hash_file(Path(release_path)))
        padded = (Path(release_path).parent / release["padded_app"]).read_bytes()
        require(second[APP_OFFSET:APP_OFFSET + APP_SIZE] == padded, "COMMISSION_APPLICATION_MISMATCH")
        layout.installation_header(second, current["identity"]["factory_mac"],
                                   archive.verify("original")["reads"][0]["sha256"], [flash_result["installation_uuid"]])
        return check("COMMISSIONING_PRESERVATION_GATE", True)
    except RecoveryError as error:
        return check(error.condition, False, **error.evidence)
