"""Offline release production and verification; no bootloader deployment target."""
import json
from pathlib import Path
import re
import struct
import subprocess
import uuid
from . import APP_OFFSET, APP_SIZE, ESPTOOL_VERSION, revision
from .errors import require
from .layout import image_header, partition_sector, STOCK
from .manifest import (read_json, json_new, write_new, mkdir, publish, hash_file,
                       verify_inventory, file_inventory, component, utc, regular)

REQUIRED = {"CONFIG_IDF_TARGET": "esp32c3", "CONFIG_ESPTOOLPY_FLASHMODE_DIO": True,
            "CONFIG_ESPTOOLPY_FLASHFREQ_80M": True, "CONFIG_ESPTOOLPY_FLASHSIZE_4MB": True,
            "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ": 160, "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": True,
            "CONFIG_ESP_WIFI_ENABLED": True, "CONFIG_BT_ENABLED": False,
            "CONFIG_ESP_WIFI_NVS_ENABLED": False, "CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE": False,
            "CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION": False, "CONFIG_ESP_PHY_ENABLE_USB": True}
PROHIBITED = re.compile(r"^CONFIG_(?:SECURE_|BOOTLOADER_APP_ANTI_ROLLBACK|BOOTLOADER_ANTI_ROLLBACK|BOOTLOADER_(?:APP_)?SECURE_VERSION|BOOTLOADER_EFUSE|ESP.*(?:DISABLE_ROM_DOWNLOAD|DIS_DOWNLOAD|DIS_USB|DIS_JTAG)|EFUSE_VIRTUAL)")
CAPABILITY_ONLY = {"CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED", "CONFIG_SECURE_BOOT_V2_ECC_SUPPORTED",
                   "CONFIG_SECURE_BOOT_V1_SUPPORTED", "CONFIG_SECURE_BOOT_V2_PREFERRED",
                   "CONFIG_SECURE_FLASH_HAS_WRITE_PROTECTION_CACHE",
                   # Kconfig internal default, not the activation choice:
                   "CONFIG_SECURE_ROM_DL_MODE_ENABLED", "CONFIG_SECURE_INSECURE_ALLOW_DL_MODE"}
SAFE_STRINGS = {"CONFIG_IDF_TARGET", "CONFIG_ESPTOOLPY_FLASHMODE", "CONFIG_ESPTOOLPY_FLASHFREQ", "CONFIG_ESPTOOLPY_FLASHSIZE", "CONFIG_PARTITION_TABLE_FILENAME", "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME"}


def app_descriptor(data, elf):
    # ESP-IDF app descriptor starts in the first segment at image offset 0x20.
    require(len(data) >= 288 and struct.unpack_from("<I", data, 32)[0] == 0xABCD5432,
            "IDF_APP_DESCRIPTOR_MISSING")
    require(struct.unpack_from("<I", data, 36)[0] == 0, "APP_SECURITY_VERSION_NOT_ZERO")
    sdk = data[144:176].split(b"\0", 1)[0].decode("ascii")
    require(sdk in ("v5.5.3", "5.5.3"), "APP_DESCRIPTOR_SDK_VERSION_MISMATCH")
    require(data[176:208].hex() == hash_file(elf), "APP_ELF_BINDING_MISMATCH")
    return {"sdk_version": sdk, "elf_sha256": hash_file(elf), "secure_version": 0}


def reviewed_config(raw):
    config = {(k if k.startswith("CONFIG_") else "CONFIG_" + k): v for k, v in raw.items()}
    for key, value in config.items():
        if PROHIBITED.match(key) and key not in CAPABILITY_ONLY:
            require(value in (False, None, "", "n", 0), "PROHIBITED_SECURITY_CONFIGURATION", symbol=key)
    for key, expected in REQUIRED.items():
        require(key in config and config[key] == expected, "REQUIRED_BUILD_CONFIGURATION_MISMATCH", symbol=key)
    sanitized = {}
    for key, value in config.items():
        if any(secret in key for secret in ("PASSWORD", "TOKEN", "SECRET", "SSID", "PRIVATE_KEY", "API_KEY", "GROUP_KEY")):
            continue
        if key not in REQUIRED and key not in SAFE_STRINGS and key not in CAPABILITY_ONLY and not PROHIBITED.match(key):
            continue
        if isinstance(value, bool) or isinstance(value, int):
            sanitized[key] = value
        elif key in SAFE_STRINGS:
            # Only known public enumerations/basenames; never paths/credentials.
            require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", value), "UNSAFE_PUBLIC_CONFIG_VALUE")
            sanitized[key] = value
    return sanitized


def git(source, *args):
    result = subprocess.run(["git", "-C", str(source), *args], capture_output=True, check=True)
    return result.stdout.decode().strip()


def build_release(build, output, confirm):
    build, output = Path(build), Path(output)
    require(build.is_absolute() and output.is_absolute(), "ABSOLUTE_PATH_REQUIRED")
    require(build.is_dir() and output.parent.is_dir() and not output.exists(), "RELEASE_DIRECTORY_INVALID")
    from .config import physical_devices
    physical_devices(build)
    physical_devices(output.parent)
    description = read_json(build / "project_description.json")
    require(description.get("target") == "esp32c3", "BUILD_TARGET_NOT_ESP32C3")
    source = Path(description["project_path"]).resolve(strict=True)
    physical_devices(source)
    commit = git(source, "rev-parse", "HEAD")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "SOURCE_COMMIT_INVALID")
    require(not git(source, "status", "--porcelain", "--untracked-files=normal"), "SOURCE_TREE_NOT_CLEAN")
    # ESP-IDF 5.5.3 calls its SDK version git_revision, not the app source SHA.
    sdk_version = description.get("git_revision", "")
    require(sdk_version in ("v5.5.3", "5.5.3"), "SDK_VERSION_NOT_PINNED")
    name = component(description["project_name"])
    app = regular(build / component(description.get("app_bin", name + ".bin")))
    elf = regular(build / component(Path(description.get("app_elf", name + ".elf")).name))
    map_file = regular(build / (name + ".map"))
    data = app.read_bytes()
    require(0 < len(data) <= APP_SIZE, "APPLICATION_EXCEEDS_FACTORY_PARTITION")
    header = image_header(data, release=True)
    descriptor = app_descriptor(data, elf)
    generated_config = read_json(build / "config/sdkconfig.json")
    sanitized = reviewed_config(generated_config)
    table_path = regular(build / "partition_table/partition-table.bin")
    table = table_path.read_bytes()
    require(len(table) <= 4096, "BUILD_PARTITION_TABLE_SIZE_INVALID")
    entries = partition_sector(table.ljust(4096, b"\xff"))
    stock_entries = [(p["label"], p["type"], p["subtype"], p["offset"], p["size"], p["flags"]) for p in entries]
    require(sorted(stock_entries) == sorted(STOCK), "BUILD_PARTITION_LAYOUT_UNSUPPORTED")
    # Avoid claiming a stale binary is built from the current clean source.
    tracked = git(source, "ls-files", "-z").split("\0")
    require(all(not (source / p).is_file() or (source / p).stat().st_mtime_ns <= app.stat().st_mtime_ns for p in tracked if p), "BUILD_OLDER_THAN_SOURCE")
    confirm("Do you confirm this build was produced from the recorded clean source commit, and security/storage configuration and firmware write paths were reviewed? [y/N]: ")
    partial = mkdir(output.parent / (output.name + "." + str(uuid.uuid4()) + ".partial"))
    write_new(partial / "app.bin", data)
    write_new(partial / "app-padded.bin", data + b"\xff" * (APP_SIZE - len(data)))
    write_new(partial / "app.elf", elf.read_bytes())
    write_new(partial / "app.map", map_file.read_bytes())
    json_new(partial / "sdkconfig.sanitized.json", sanitized)
    # Do not copy raw project_description or raw sdkconfig: both can hold private paths/values.
    json_new(partial / "build-manifest.json", {"source_commit": commit, "sdk_version": sdk_version,
             "project": name, "target": "esp32c3", "app_header": header,
             "app_descriptor": descriptor,
             "partition_table_sha256": hash_file(table_path), "stock_layout": stock_entries,
             "compiler": description.get("c_compiler", "not-recorded").split("/")[-1],
             "configuration_review": "operator confirmed", "source_review": "operator confirmed clean build provenance"})
    manifest = {"schema_version": 1, "created_utc": utc(), "source_commit": commit,
                "sdk_version": sdk_version, "tool": revision(), "esptool_version": ESPTOOL_VERSION,
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


def verify_release(path, chip_revision=None):
    manifest = read_json(path)
    require(manifest["schema_version"] == 1 and manifest["sdk_version"] in ("v5.5.3", "5.5.3")
            and manifest["esptool_version"] == ESPTOOL_VERSION, "RELEASE_VERSION_MISMATCH")
    require(re.fullmatch(r"[0-9a-f]{40}", manifest["source_commit"]), "RELEASE_SOURCE_COMMIT_MISSING")
    require(manifest.get("reviewed_configuration") is True, "RELEASE_CONFIGURATION_NOT_REVIEWED")
    require(manifest["permitted_write_range"] == {"offset": APP_OFFSET, "length": APP_SIZE}, "RELEASE_WRITE_RANGE_INVALID")
    require(APP_OFFSET % 4096 == 0 and APP_SIZE % 4096 == 0, "RELEASE_ERASE_RANGE_INVALID")
    require(all(manifest[k] == 1 for k in ("protocol_version", "http_api_version", "storage_schema_version")), "UNSUPPORTED_RELEASE_SCHEMA")
    verify_inventory(path.parent, manifest["files"])
    for key in ("app", "padded_app", "elf", "map"):
        require(component(manifest[key]) in manifest["files"], "RELEASE_ARTIFACT_NOT_HASHED")
    app, padded = (path.parent / manifest[k] for k in ("app", "padded_app"))
    data = app.read_bytes()
    require(0 < len(data) == manifest["app_size"] <= APP_SIZE, "APPLICATION_EXCEEDS_FACTORY_PARTITION")
    image_header(data, revision=chip_revision, release=True)
    app_descriptor(data, path.parent / manifest["elf"])
    require(hash_file(app) == manifest["app_sha256"] and hash_file(padded) == manifest["padded_sha256"], "RELEASE_IMAGE_HASH_MISMATCH")
    require(padded.read_bytes() == data + b"\xff" * (APP_SIZE - len(data)), "RELEASE_PADDING_INVALID")
    reviewed_config(read_json(path.parent / "sdkconfig.sanitized.json"))
    build = read_json(path.parent / "build-manifest.json")
    require(build["source_commit"] == manifest["source_commit"] and build["sdk_version"] == manifest["sdk_version"], "BUILD_MANIFEST_MISMATCH")
    require(sorted(tuple(p) for p in build["stock_layout"]) == sorted(STOCK), "BUILD_PARTITION_LAYOUT_UNSUPPORTED")
    return manifest
