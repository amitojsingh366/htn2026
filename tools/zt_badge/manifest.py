"""Durable private files and strict, unambiguous JSON."""
import hashlib
import json
import os
import re
import stat
from datetime import datetime, timezone
from pathlib import Path
from .errors import require

SCHEMA = 1


def utc():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def pairs_unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "DUPLICATE_JSON_KEY")
        result[key] = value
    return result


def read_json(path):
    with regular(path).open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=pairs_unique)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode()


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def mkdir(path):
    path = Path(path)
    path.mkdir(mode=0o700)
    fsync_dir(path.parent)
    return path


def private_tree(path):
    path = Path(path)
    if not path.exists():
        private_tree(path.parent)
        mkdir(path)
    require(path.is_dir() and not path.is_symlink(), "UNSAFE_DIRECTORY")
    require(all(not p.is_symlink() for p in path.parents), "SYMLINK_DIRECTORY_ANCESTOR")
    return path


def regular(path):
    path = Path(path)
    require(stat.S_ISREG(path.lstat().st_mode) and not path.is_symlink(), "NOT_REGULAR_FILE")
    require(path.stat().st_nlink == 1, "HARDLINK_REJECTED")
    require(all(not p.is_symlink() for p in path.parents), "SYMLINK_FILE_ANCESTOR")
    return path


def write_new(path, data):
    """Never truncate. Callers use new names or unpublished partial directories."""
    path = Path(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    fsync_dir(path.parent)


def json_new(path, value):
    write_new(path, encoded(value))


def hash_file(path):
    digest = hashlib.sha256()
    with regular(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def equal_files(first, second):
    if regular(first).stat().st_size != regular(second).stat().st_size:
        return False
    with first.open("rb") as a, second.open("rb") as b:
        while True:
            block = a.read(1024 * 1024)
            if block != b.read(1024 * 1024):
                return False
            if not block:
                return True


def publish(partial, final):
    require(not final.exists() and not final.is_symlink(), "DESTINATION_EXISTS")
    fsync_dir(partial)
    os.rename(partial, final)
    fsync_dir(final.parent)


def component(value):
    require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,160}", value)
            and value not in (".", "..") and not value.endswith(".partial"), "UNSAFE_PATH_COMPONENT")
    return value


def identity_valid(identity):
    require(isinstance(identity, dict), "IDENTITY_METADATA_MISSING")
    require(re.fullmatch(r"[0-9a-f]{12}", identity.get("factory_mac", "")), "INVALID_FACTORY_MAC")
    for key in ("chip", "package", "revision", "jedec_id", "flash_size", "security",
                "efuse_sha256", "read_protection"):
        require(key in identity, "IDENTITY_METADATA_MISSING", field=key)
    require(re.fullmatch(r"[0-9a-f]{64}", identity["efuse_sha256"]), "INVALID_EFUSE_HASH")
    return identity


def file_inventory(directory, exclude=()):
    result = {}
    for path in sorted(directory.iterdir()):
        if path.name not in exclude:
            regular(path)
            result[path.name] = {"size": path.stat().st_size, "sha256": hash_file(path)}
    return result


def verify_inventory(directory, inventory):
    for name, fact in inventory.items():
        component(name)
        path = regular(directory / name)
        require(path.stat().st_size == fact["size"], "FILE_SIZE_MISMATCH", file=name)
        require(hash_file(path) == fact["sha256"], "FILE_HASH_MISMATCH", file=name)
