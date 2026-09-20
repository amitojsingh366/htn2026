"""Rebuildable lookup cache, never a write authorization."""
import secrets
import sqlite3
from .errors import require
from .manifest import read_json, component, identity_valid, hash_file, fsync_dir

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def recovery_id():
    number = secrets.randbits(80)
    raw = "".join(ALPHABET[(number >> shift) & 31] for shift in range(75, -1, -5))
    # Restrict checksum to a filename-safe alphabet by using two base32 digits.
    suffix = ALPHABET[(number % 37) // 32] + ALPHABET[(number % 37) % 32]
    return "ZT-" + "-".join(raw[n:n + 4] for n in range(0, 16, 4)) + "-" + suffix


def scan(root):
    records = {}
    if not root.exists():
        return records
    for directory in root.iterdir():
        if not directory.is_dir() or directory.is_symlink() or not directory.name.startswith("ZT-") or directory.name.endswith(".partial"):
            continue
        manifest = directory / "original/manifest.json"
        if not manifest.is_file():
            continue
        data = read_json(manifest)
        identity_valid(data["identity"])
        require(data["recovery_id"] == directory.name, "ARCHIVE_ID_PATH_MISMATCH")
        # Signed-off local receipt binds the immutable manifest; verification of
        # copies is still required by every gate, even after this scan succeeds.
        signed = False
        for result in (directory / "operations").glob("*/result.json"):
            receipt = read_json(result)
            if receipt.get("READ_VERIFIED") is True and receipt.get("status") == "BACKUP_VERIFIED" and receipt.get("backup", {}).get("manifest_sha256") == hash_file(manifest):
                signed = True
        if signed:
            component(directory.name)
            records[directory.name] = data["identity"]["factory_mac"]
    require(len(set(records.values())) == len(records), "DUPLICATE_MAC_ARCHIVE")
    return records


def rebuild(root):
    records = scan(root)
    path = root / "index.sqlite"
    require(not path.is_symlink(), "UNSAFE_INDEX_PATH")
    with sqlite3.connect(path) as connection:
        connection.execute("CREATE TABLE IF NOT EXISTS devices (recovery_id TEXT PRIMARY KEY, mac TEXT UNIQUE NOT NULL)")
        for rid, mac in records.items():
            row = connection.execute("SELECT mac FROM devices WHERE recovery_id=?", (rid,)).fetchone()
            require(row is None or row[0] == mac, "RECOVERY_ID_MAC_COLLISION")
            connection.execute("INSERT OR IGNORE INTO devices VALUES (?, ?)", (rid, mac))
    path.chmod(0o600)
    fsync_dir(root)
    return records
