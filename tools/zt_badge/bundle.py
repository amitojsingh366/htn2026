"""Verified local owner handover. No network facilities or extraction shortcuts."""
from pathlib import Path, PurePosixPath
import hashlib
import re
import uuid
from . import FLASH_SIZE, VERSION, ESPTOOL_VERSION, revision
from .archive import INSTRUCTIONS, Operation
from .errors import require
from .manifest import (regular, read_json, json_new, write_new, mkdir, private_tree,
                       hash_file, file_inventory, verify_inventory, equal_files,
                       publish, fsync_dir, utc, identity_valid, component)
from . import index

OPTIONAL_PRIVATE = {"efuses.json", "efuses-readable.bin", "acquisition.log"}


def relative_name(value):
    require(isinstance(value, str) and value and "\\" not in value and "\x00" not in value,
            "BUNDLE_UNSAFE_PATH")
    path = PurePosixPath(value)
    require(not path.is_absolute() and value == path.as_posix()
            and all(p not in (".", "..", "") and re.fullmatch(r"[A-Za-z0-9_.-]+", p) for p in path.parts), "BUNDLE_PATH_TRAVERSAL")
    return path


def validate(source):
    source = Path(source)
    require(source.is_absolute() and source.is_dir() and not source.is_symlink(), "BUNDLE_DIRECTORY_INVALID")
    from .config import physical_devices
    physical_devices(source)
    for parent in source.parents:
        require(not parent.is_symlink(), "BUNDLE_SYMLINK_REJECTED")
    # Inspect every entry, including files not listed in the export manifest,
    # before any destination directory is created.
    all_files = set()
    for path in source.rglob("*"):
        require(not path.is_symlink(), "BUNDLE_SYMLINK_REJECTED")
        relative_name(path.relative_to(source).as_posix())
        if path.is_file():
            regular(path)
            all_files.add(path.relative_to(source).as_posix())
        else:
            require(path.is_dir(), "BUNDLE_SPECIAL_FILE_REJECTED")
    exported = read_json(source / "export-manifest.json")
    require(exported["schema_version"] == 1 and exported["tool"]["version"] == VERSION
            and exported["esptool_version"] == ESPTOOL_VERSION, "BUNDLE_TOOL_VERSION_UNSUPPORTED")
    identity_valid(exported["identity"])
    component(exported["recovery_id"])
    seen = set()
    for record in exported["files"]:
        name = str(relative_name(record["path"]))
        require(name not in seen, "BUNDLE_DUPLICATE_FILE")
        seen.add(name)
        path = source / name
        if not path.exists() and record.get("required") is False:
            continue
        require(path.exists(), "BUNDLE_REQUIRED_FILE_MISSING", file=name)
        require(path.stat().st_size == record["size"] and hash_file(path) == record["sha256"], "BUNDLE_FILE_HASH_MISMATCH", file=name)
    require(all_files <= seen | {"export-manifest.json"}, "BUNDLE_UNLISTED_FILE")
    required = {r["path"] for r in exported["files"] if r.get("required") is True}
    require({"flash.bin", "flash-second-read.bin", "manifest.json", "receipt.txt",
             "backup-receipt.json", "recovery-instructions.txt", "tool-revision.json",
             "tool/requirements.txt", "tool/badge_tool.py"} <= required, "BUNDLE_REQUIRED_FILE_DECLARATION_MISSING")
    require((source / "flash.bin").stat().st_size == FLASH_SIZE and (source / "flash-second-read.bin").stat().st_size == FLASH_SIZE, "BUNDLE_FULL_IMAGE_SIZE_INVALID")
    require(hash_file(source / "flash.bin") == exported["image_sha256"]
            and equal_files(source / "flash.bin", source / "flash-second-read.bin"), "BUNDLE_IMAGE_MISMATCH")
    original = read_json(source / "manifest.json")
    require(original["identity"] == exported["identity"] and original["recovery_id"] == exported["recovery_id"], "BUNDLE_IDENTITY_MISMATCH")
    require(hash_file(source / "manifest.json") == exported["source_manifest_sha256"], "BUNDLE_SOURCE_MANIFEST_MISMATCH")
    require(original["esptool_version"] == ESPTOOL_VERSION and original["schema_version"] == 1, "BUNDLE_SOURCE_VERSION_MISMATCH")
    reads = original["reads"]
    require(len(reads) == 2 and reads[0]["acquisition_uuid"] != reads[1]["acquisition_uuid"]
            and all(r["sha256"] == exported["image_sha256"] and r["return_code"] == 0 and r["length"] == FLASH_SIZE and r["offset"] == 0 for r in reads), "BUNDLE_INDEPENDENT_READ_PROOF_MISSING")
    receipt = read_json(source / "backup-receipt.json")
    require(receipt.get("READ_VERIFIED") is True and receipt["backup"]["manifest_sha256"] == exported["source_manifest_sha256"], "BUNDLE_BACKUP_SIGNOFF_INVALID")
    for name, fact in original["files"].items():
        component(name)
        require(name in OPTIONAL_PRIVATE or name in required, "BUNDLE_SOURCE_REQUIRED_FILE_MISSING", file=name)
        if (source / name).exists():
            require(hash_file(source / name) == fact["sha256"], "BUNDLE_SOURCE_FILE_HASH_MISMATCH")
    require(read_json(source / "tool-revision.json") == exported["tool"], "BUNDLE_TOOL_REVISION_MISMATCH")
    require((source / "tool/requirements.txt").read_text().strip() == "esptool==5.4.0", "BUNDLE_DEPENDENCY_LOCK_MISMATCH")
    tool = source / "tool"
    digest = hashlib.sha256()
    tool_files = [tool / "badge_tool.py", tool / "requirements.txt", *sorted((tool / "zt_badge").glob("*.py"))]
    require(len(tool_files) > 2, "BUNDLE_TOOL_PACKAGE_MISSING")
    for path in tool_files:
        require(path.relative_to(source).as_posix() in required, "BUNDLE_TOOL_FILE_NOT_REQUIRED")
        digest.update(path.relative_to(tool).as_posix().encode() + b"\0")
        with regular(path).open("rb") as stream:
            digest.update(stream.read())
    require(digest.hexdigest() == exported["tool"]["source_sha256"], "BUNDLE_TOOL_SOURCE_HASH_MISMATCH")
    return exported, original


def export_bundle(archive, snapshot, destination, confirm):
    original = archive.verify(snapshot)
    source = archive.snapshot_path(snapshot)
    destination = Path(destination)
    require(destination.is_absolute() and destination.is_dir() and not destination.is_symlink(), "OWNER_DESTINATION_INVALID")
    from .config import physical_devices
    physical_devices(destination)
    confirm("Confirm participant agreement and ownership of this local destination. The recovery code alone is insufficient. Type OWNER:", "OWNER")
    final = destination / ("badge-recovery-" + str(uuid.uuid4()))
    partial = mkdir(destination / (final.name + ".partial"))
    for name in file_inventory(source):
        if name not in OPTIONAL_PRIVATE:
            write_new(partial / name, (source / name).read_bytes())
    write_new(partial / "receipt.txt", (archive.device_dir / "receipt.txt").read_bytes())
    receipt = next(r for r in archive.results() if r.get("READ_VERIFIED") and r.get("backup", {}).get("manifest_sha256") == hash_file(source / "manifest.json"))
    json_new(partial / "backup-receipt.json", receipt)
    json_new(partial / "tool-revision.json", revision())
    mkdir(partial / "tool")
    mkdir(partial / "tool/zt_badge")
    tool_root = Path(__file__).resolve().parent.parent
    for path in [tool_root / "badge_tool.py", tool_root / "requirements.txt", *sorted((tool_root / "zt_badge").glob("*.py"))]:
        write_new(partial / "tool" / path.relative_to(tool_root), path.read_bytes())
    inventory = [{"path": p.relative_to(partial).as_posix(), "size": p.stat().st_size,
                  "sha256": hash_file(p), "required": True}
                 for p in sorted(partial.rglob("*")) if p.is_file()]
    exported = {"schema_version": 1, "created_utc": utc(), "recovery_id": archive.recovery_id,
                "snapshot_id": snapshot, "identity": original["identity"], "layout": original["layout"],
                "source_manifest_sha256": hash_file(source / "manifest.json"),
                "image_sha256": hash_file(source / "flash.bin"), "readable_efuse_sha256": original["identity"]["efuse_sha256"],
                "tool": revision(), "esptool_version": ESPTOOL_VERSION, "files": inventory,
                "omitted_optional_private_evidence": sorted(OPTIONAL_PRIVATE)}
    json_new(partial / "export-manifest.json", exported)
    validate(partial)
    for path in sorted((p for p in partial.rglob("*") if p.is_dir()), reverse=True):
        fsync_dir(path)
    publish(partial, final)
    validate(final)
    return final


def import_bundle(archive, source):
    source = Path(source)
    exported, original = validate(source)
    records = index.scan(archive.root)
    rid, mac = exported["recovery_id"], exported["identity"]["factory_mac"]
    # Check all original manifests, even incompletely signed-off archives.
    for path in archive.root.glob("ZT-*/original/manifest.json"):
        candidate = read_json(path)
        records[path.parents[1].name] = candidate["identity"]["factory_mac"]
    require(rid not in records or records[rid] == mac, "RECOVERY_ID_MAC_COLLISION")
    existing = next((key for key, value in records.items() if value == mac), None)
    new = existing is None
    archive.recovery_id = existing or rid
    if new:
        require(original["snapshot_kind"] == "original", "ORIGINAL_BUNDLE_REQUIRED_FOR_NEW_DEVICE")
        require(not archive.device_dir.exists() and not (archive.mirror / rid).exists(), "IMPORT_DESTINATION_EXISTS")
    from .config import independent
    independent(archive.config)
    # All validation/collision checks precede any import mutation.
    if new:
        final_device = archive.device_dir
        staged_device = mkdir(final_device.parent / (rid + "." + str(uuid.uuid4()) + ".partial"))
        for name in ("snapshots", "operations", "tool"):
            mkdir(staged_device / name)
        snapshot_id = "original"
        partial = mkdir(staged_device / "original")
    else:
        snapshot_id = utc().replace(":", "").replace(".", "-") + "-" + str(uuid.uuid4())
        final = archive.snapshot_path(snapshot_id)
        partial = mkdir(final.parent / (snapshot_id + ".partial"))
    for name in original["files"]:
        if (source / name).exists():
            write_new(partial / name, (source / name).read_bytes())
    prefix = "import-" + hash_file(source / "export-manifest.json")
    evidence = {"source_manifest_file": prefix + "-source.json",
                "export_manifest_file": prefix + "-export.json",
                "backup_receipt_file": prefix + "-receipt.json"}
    for key, source_name in (("source_manifest_file", "manifest.json"),
                             ("export_manifest_file", "export-manifest.json"),
                             ("backup_receipt_file", "backup-receipt.json")):
        target = partial / evidence[key]
        if target.exists():
            require(hash_file(target) == hash_file(source / source_name), "IMPORT_PROVENANCE_COLLISION")
        else:
            write_new(target, (source / source_name).read_bytes())
    manifest = {**original, "recovery_id": archive.recovery_id, "snapshot_id": snapshot_id,
                "snapshot_kind": "original" if new else "imported",
                "original_link": None if new else {"snapshot_id": "original", "manifest_sha256": hash_file(archive.device_dir / "original/manifest.json")},
                "files": file_inventory(partial),
                "import_provenance": {"export_sha256": hash_file(source / "export-manifest.json"),
                                      "source_manifest_sha256": exported["source_manifest_sha256"],
                                      "imported_utc": utc(), "omitted_optional": sorted(OPTIONAL_PRIVATE), **evidence},
                "duplicate_intent": {"location": str(archive.mirror / archive.recovery_id), "medium_label": archive.config["mirror_medium_label"]}}
    json_new(partial / "manifest.json", manifest)
    fsync_dir(partial)
    if new:
        write_new(staged_device / "receipt.txt", (source / "receipt.txt").read_bytes())
        publish(staged_device, final_device)
    else:
        publish(partial, final)
    operation = archive.operation("import-bundle")
    try:
        receipt = archive.duplicate(snapshot_id)
        operation.finish("BACKUP_VERIFIED", snapshot_id=snapshot_id, READ_VERIFIED=True, backup=receipt,
                         provenance="Verified historical independent reads from owner bundle; not a fresh device capture")
        archive.mirror_operation(operation)
        archive.verify(snapshot_id)
        if new:
            mirror_receipt = archive.mirror / archive.recovery_id / "receipt.txt"
            write_new(mirror_receipt, (archive.device_dir / "receipt.txt").read_bytes())
        archive.save_tool()
        index.rebuild(archive.root)
    except BaseException as error:
        operation.failure(error)
        raise
    return snapshot_id
