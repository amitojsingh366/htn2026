#!/usr/bin/env python3
"""Package an existing development build, explicitly allowing uncommitted source.

This offline packager does not open a badge. Keep it outside zt_badge so creating
a development release does not change the hardware recovery tool's revision.
"""
import argparse
import os
from pathlib import Path
import re
import sys
import uuid

sys.dont_write_bytecode = True

from zt_badge import APP_OFFSET, APP_SIZE, ESPTOOL_VERSION, revision
from zt_badge.errors import RecoveryError, require
from zt_badge.layout import image_header, partition_sector, STOCK
from zt_badge.manifest import (component, file_inventory, hash_file, json_new,
                               mkdir, publish, read_json, regular, utc, write_new)
from zt_badge.release import app_descriptor, git, reviewed_config, verify_release


def package(build, output):
    description = read_json(build / "project_description.json")
    require(description.get("target") == "esp32c3", "BUILD_TARGET_NOT_ESP32C3")
    source = Path(description["project_path"]).resolve(strict=True)
    root = Path(git(source, "rev-parse", "--show-toplevel"))
    commit = git(source, "rev-parse", "HEAD")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "SOURCE_COMMIT_INVALID")
    dirty = bool(git(source, "status", "--porcelain", "--untracked-files=normal"))
    sdk = description.get("git_revision", "")
    require(sdk in ("v5.5.3", "5.5.3"), "SDK_VERSION_NOT_PINNED")
    name = component(description["project_name"])
    app = regular(build / component(description.get("app_bin", name + ".bin")))
    elf = regular(build / component(Path(description.get("app_elf", name + ".elf")).name))
    map_file = regular(build / (name + ".map"))
    data = app.read_bytes()
    require(0 < len(data) <= APP_SIZE, "APPLICATION_EXCEEDS_FACTORY_PARTITION")
    header = image_header(data, release=True)
    descriptor = app_descriptor(data, elf)
    sanitized = reviewed_config(read_json(build / "config/sdkconfig.json"))
    table_path = regular(build / "partition_table/partition-table.bin")
    table = table_path.read_bytes()
    require(len(table) <= 4096, "BUILD_PARTITION_TABLE_SIZE_INVALID")
    entries = partition_sector(table.ljust(4096, b"\xff"))
    layout = [(p["label"], p["type"], p["subtype"], p["offset"], p["size"], p["flags"]) for p in entries]
    require(sorted(layout) == sorted(STOCK), "BUILD_PARTITION_LAYOUT_UNSUPPORTED")

    # Retain hashes, never a source diff that could disclose baked credentials.
    source_files = {}
    for relative in git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard").split("\0"):
        if not relative:
            continue
        path = root / relative
        if not path.is_file():
            source_files[relative] = None
            continue
        if path.is_relative_to(source):
            require(path.stat().st_mtime_ns <= app.stat().st_mtime_ns, "BUILD_OLDER_THAN_SOURCE")
        source_files[relative] = hash_file(path)
    # These ignored headers are required inputs for live gateway builds. Record
    # their digests and freshness only; never copy private credentials into notes.
    for relative in ("private/zt_gateway_private.h", "private/zt_device_private.h"):
        private_header = root / relative
        if private_header.is_file():
            require(private_header.stat().st_mtime_ns <= app.stat().st_mtime_ns,
                    "BUILD_OLDER_THAN_PRIVATE_CONFIG")
            source_files[relative] = hash_file(private_header)
    partial = mkdir(output.parent / (output.name + "." + str(uuid.uuid4()) + ".partial"))
    write_new(partial / "app.bin", data)
    write_new(partial / "app-padded.bin", data + b"\xff" * (APP_SIZE - len(data)))
    write_new(partial / "app.elf", elf.read_bytes())
    write_new(partial / "app.map", map_file.read_bytes())
    json_new(partial / "sdkconfig.sanitized.json", sanitized)
    json_new(partial / "source-snapshot.json", {"base_commit": commit, "source_dirty": dirty,
                                               "files_sha256": source_files})
    json_new(partial / "build-manifest.json", {
        "source_commit": commit, "source_dirty": dirty, "sdk_version": sdk,
        "project": name, "target": "esp32c3", "app_header": header,
        "app_descriptor": descriptor, "partition_table_sha256": hash_file(table_path),
        "stock_layout": layout, "compiler": description.get("c_compiler", "not-recorded").split("/")[-1],
        "configuration_review": "required SDK and storage configuration checked during packaging",
        "source_review": "development build; operator explicitly allowed uncommitted source",
        "source_snapshot": "source-snapshot.json"})
    manifest = {
        "schema_version": 1, "created_utc": utc(), "source_commit": commit,
        "source_dirty": dirty, "development_release": True,
        "sdk_version": sdk, "tool": revision(), "esptool_version": ESPTOOL_VERSION,
        "protocol_version": 1, "http_api_version": 1, "storage_schema_version": 1,
        "app": "app.bin", "padded_app": "app-padded.bin", "elf": "app.elf", "map": "app.map",
        "app_size": len(data), "app_sha256": hash_file(partial / "app.bin"),
        "padded_sha256": hash_file(partial / "app-padded.bin"),
        "permitted_write_range": {"offset": APP_OFFSET, "length": APP_SIZE},
        "reviewed_configuration": True, "files": file_inventory(partial)}
    json_new(partial / "release-manifest.json", manifest)
    verify_release(partial / "release-manifest.json")
    publish(partial, output)
    return output / "release-manifest.json"


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-dirty", action="store_true", required=True,
                        help="Explicitly permit uncommitted source in this development release.")
    parser.add_argument("--build", type=Path, default=root / "firmware/build")
    args = parser.parse_args()
    os.umask(0o077)
    releases = root / ".orchestration/releases"
    releases.mkdir(parents=True, exist_ok=True, mode=0o700)
    output = releases / ("dev-" + utc().replace(":", "").replace("-", ""))
    try:
        result = package(args.build.resolve(strict=True), output)
    except RecoveryError as error:
        print("STOPPED: " + error.condition, file=sys.stderr)
        return 1
    print("Development release created: " + str(result))
    print("Return to badge_ops and select flash. The newest release is selected automatically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
