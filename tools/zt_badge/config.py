"""Organizer-local configuration and conservative physical-medium checks."""
import os
import plistlib
import subprocess
import sys
from pathlib import Path
from .errors import require, RecoveryError
from .manifest import read_json, json_new, private_tree


def local_dir():
    return Path.home() / "Library/Application Support/ZombieTag" if sys.platform == "darwin" else Path.home() / ".local/share/zombie-tag"


def load():
    config = read_json(local_dir() / "recovery-config.json")
    for key in ("archive", "mirror"):
        config[key] = safe_root(config[key])
    physical_devices(config["archive"])
    return config


def safe_root(value):
    path = Path(value).expanduser()
    require(path.is_absolute(), "ABSOLUTE_PATH_REQUIRED")
    path = path.resolve()
    require(path != Path(path.anchor), "UNSAFE_ARCHIVE_ROOT")
    for parent in (path, *path.parents):
        require(not (parent / ".git").exists(), "ARCHIVE_INSIDE_REPOSITORY")
    words = str(path).lower()
    require(not any(x in words for x in (".worktrees", "mobile documents", "cloudstorage", "dropbox", "onedrive", "google drive")), "SHARED_ARCHIVE_PATH")
    return path


def _mount_point(path):
    """Resolve a filesystem path to the volume root that contains it.

    `diskutil info` accepts a device node or a MOUNT POINT, never an arbitrary
    directory: given one it exits 1 with Error/ErrorMessage "Could not find disk".
    Walking up to the containing mount point is what makes the medium question
    answerable at all.
    """
    resolved = Path(path).resolve()
    while not os.path.ismount(resolved) and resolved != resolved.parent:
        resolved = resolved.parent
    return resolved


def _disk_info(target, is_path=False):
    """Query diskutil for a device identifier, or for the volume holding a path.

    Fails closed through the structured vocabulary: diskutil reports a missing or
    unreadable target as Error inside the plist, sometimes with exit 0, so both the
    return code and the Error key are checked. A raw CalledProcessError here would
    escape the recovery error vocabulary and lose the fail-closed guarantee.
    """
    if is_path:
        target = _mount_point(target)
    result = subprocess.run(["/usr/sbin/diskutil", "info", "-plist", str(target)],
                            capture_output=True)
    data = {}
    if result.stdout.startswith(b"<?xml"):
        try:
            data = plistlib.loads(result.stdout)
        except Exception:
            data = {}
    require(result.returncode == 0 and not data.get("Error") and data,
            "UNKNOWN_PHYSICAL_MEDIUM")
    return data


def physical_devices(path):
    """Fail closed when physical backing cannot be established (RAID/network included)."""
    if sys.platform == "darwin":
        info = _disk_info(path, is_path=True)
        require(info.get("VirtualOrPhysical") != "Virtual" or info.get("APFSContainerReference"), "UNKNOWN_PHYSICAL_MEDIUM")
        container = info.get("APFSContainerReference")
        if container:
            result = subprocess.run(["/usr/sbin/diskutil", "apfs", "list", "-plist", container], capture_output=True, check=True)
            entries = plistlib.loads(result.stdout)["Containers"]
            stores = [s["DeviceIdentifier"] for c in entries for s in c.get("PhysicalStores", [])]
            require(stores, "UNKNOWN_PHYSICAL_MEDIUM")
            return frozenset(_disk_info(s).get("ParentWholeDisk", s) for s in stores)
        # A non-APFS volume is a plain partition. diskutil reports VirtualOrPhysical
        # only on the WHOLE disk, never on the partition, so asking the partition
        # rejected every ExFAT/FAT/HFS+ external drive. Resolve to the whole disk and
        # ask it instead. Note the key is "WholeDisk"; "Whole" does not exist here.
        disk = info.get("ParentWholeDisk") or (info.get("DeviceIdentifier") if info.get("WholeDisk") else None)
        require(disk, "UNKNOWN_PHYSICAL_MEDIUM")
        whole = info if disk == info.get("DeviceIdentifier") and info.get("WholeDisk") else _disk_info(disk)
        # Still fails closed: a disk image or other virtual device reports Virtual here.
        require(whole.get("VirtualOrPhysical") == "Physical", "UNKNOWN_PHYSICAL_MEDIUM")
        return frozenset([disk])
    if sys.platform.startswith("linux"):
        dev = path.stat().st_dev
        node = Path(f"/sys/dev/block/{os.major(dev)}:{os.minor(dev)}")
        require(node.exists(), "UNKNOWN_PHYSICAL_MEDIUM")
        def backing(node):
            node = node.resolve()
            children = list((node / "slaves").iterdir()) if (node / "slaves").exists() else []
            if children:
                return set().union(*(backing(p) for p in children))
            if (node / "partition").exists():
                node = node.parent
            require(not node.name.startswith(("loop", "ram", "zram")) and (node / "device").exists(), "UNKNOWN_PHYSICAL_MEDIUM")
            return {str(node)}
        return frozenset(backing(node))
    raise RecoveryError("UNSUPPORTED_MEDIUM_INSPECTION_PLATFORM")


def independent(config):
    primary, mirror = (Path(config[k]).resolve(strict=True) for k in ("archive", "mirror"))
    require(primary != mirror and primary not in mirror.parents and mirror not in primary.parents, "SAME_ARCHIVE_TREE")
    require(primary.stat().st_dev != mirror.stat().st_dev, "SAME_FILESYSTEM")
    first, second = physical_devices(primary), physical_devices(mirror)
    require(first.isdisjoint(second), "SAME_PHYSICAL_DISK")
    require(config.get("mirror_medium_label", "").strip(), "MEDIUM_LABEL_REQUIRED")
    return {"archive_devices": sorted(first), "mirror_devices": sorted(second),
            "medium_label": config["mirror_medium_label"],
            "limitation": "Filesystem IDs alone cannot prove independent hardware; the organizer's recorded medium label is evidence."}


def initialize(archive, mirror, label):
    config = {"archive": safe_root(archive), "mirror": safe_root(mirror), "mirror_medium_label": label}
    # Require mounted, existing roots: never create a missing external mount point.
    require(all(config[k].is_dir() for k in ("archive", "mirror")), "ARCHIVE_ROOT_MUST_EXIST")
    for k in ("archive", "mirror"):
        require(config[k].stat().st_mode & 0o077 == 0, "ARCHIVE_ROOT_NOT_PRIVATE")
    facts = independent(config)
    private_tree(local_dir())
    json_new(local_dir() / "recovery-config.json", {**config, "archive": str(config["archive"]), "mirror": str(config["mirror"]), "medium_evidence": facts})
    return facts
