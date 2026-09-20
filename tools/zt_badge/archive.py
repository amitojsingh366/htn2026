"""Append-only, mirrored per-device archives and operation journals."""
import contextlib
import os
from pathlib import Path
import shutil
import uuid
from . import FLASH_SIZE, ESPTOOL_VERSION, revision
from . import config as configuration, index, layout
from .errors import RecoveryError, require
from .manifest import (utc, mkdir, private_tree, fsync_dir, write_new, json_new, read_json,
                       hash_file, file_inventory, verify_inventory, equal_files, publish,
                       component, identity_valid)
from .locking import archive_lock, hardware_lock

INSTRUCTIONS = """Private same-device recovery archive. Do not upload or send it.
Install Python 3.12 and the included exact dependency lock. Use badge_tool.py
archive-init with two independent encrypted media, then import-bundle if needed.
Use restore --recovery-id <receipt code> --snapshot original --port <selected port>.
Obtain owner agreement; select the same physical badge. Hold START on USB connect
for ROM download. The tool captures current state, verifies both copies, restores
all 4 MiB, and compares full preboot readback before normal boot is permitted.
A zero write return code is not a restoration claim. Keep failed devices in ROM.
After RESTORE_VERIFIED power-cycle with START released and observe stock boot.
Cannot recover eFuses, flash status/OTP, RAM, external NFC/account/blockchain state,
physical damage, unreadable state, or any earlier state not captured.
"""


class Operation:
    def __init__(self, parent, kind):
        private_tree(parent)
        self.id = str(uuid.uuid4())
        self.path = mkdir(parent / self.id)
        self.kind = kind
        self.sequence = 0
        self.done = False
        self.event("STARTED", kind=kind, tool=revision())

    def event(self, status, **facts):
        self.sequence += 1
        json_new(self.path / f"event-{self.sequence:04d}.json",
                 {"operation_uuid": self.id, "utc": utc(), "status": status, **facts})

    def prepare(self, intent):
        json_new(self.path / "intent.json", intent)
        json_new(self.path / "intent-prepared.json", intent)

    def writing(self, intent):
        # Only operation state is replaceable; snapshots and original are never
        # opened for truncation or replaced. Retain the initial intent separately.
        json_new(self.path / "intent-writing.partial", {**intent, "status": "WRITING"})
        os.replace(self.path / "intent-writing.partial", self.path / "intent.json")
        fsync_dir(self.path)
        self.event("WRITING", expected_hash=intent["expected_hash"])

    def finish(self, status, **facts):
        require(not self.done, "OPERATION_ALREADY_FINISHED")
        json_new(self.path / "result.json", {"operation_uuid": self.id, "kind": self.kind,
                 "status": status, "utc": utc(), "tool": revision(), **facts})
        self.done = True

    def failure(self, error):
        # A later failure (e.g. stock observation) gets its own record, never
        # overwrites a successful preboot restoration result.
        failure = Operation(self.path.parent, "failure")
        failure.finish("FAILED", failed_operation=self.id,
                       condition=getattr(error, "condition", "UNEXPECTED_PRIVATE_ERROR"),
                       evidence=getattr(error, "evidence", {}),
                       private_exception=repr(error))
        if not self.done:
            self.finish("FAILED", condition=getattr(error, "condition", "UNEXPECTED_PRIVATE_ERROR"),
                        failure_operation=failure.id)


def mirror_directory(source, target):
    require(not target.exists(), "MIRROR_DESTINATION_EXISTS")
    private_tree(target.parent)
    partial = mkdir(target.parent / (target.name + "." + str(uuid.uuid4()) + ".partial"))
    inventory = file_inventory(source)
    for name in inventory:
        write_new(partial / name, (source / name).read_bytes())
    verify_inventory(partial, inventory)  # reopened on the second filesystem
    fsync_dir(partial)
    publish(partial, target)
    verify_inventory(target, inventory)
    return inventory


class Archive:
    def __init__(self, config, recovery_id=None):
        self.config = config
        self.root = config["archive"]
        self.mirror = config["mirror"]
        self.recovery_id = component(recovery_id) if recovery_id else None
        self.locked = False
        self.hardware_session = None

    @property
    def device_dir(self):
        require(self.recovery_id is not None, "DEVICE_NOT_SELECTED")
        path = self.root / self.recovery_id
        require(not path.is_symlink(), "SYMLINK_DEVICE_DIRECTORY")
        return path

    @contextlib.contextmanager
    def lock(self):
        with archive_lock(self.config):
            self.locked = True
            try:
                yield self
            finally:
                self.locked = False

    def operation(self, kind):
        return Operation((self.device_dir if self.recovery_id else self.root) / "operations", kind)

    def select_identity(self, identity, operation):
        records = index.scan(self.root)
        # A published original awaiting its second copy still owns its MAC/ID.
        for path in self.root.glob("ZT-*/original/manifest.json"):
            candidate = read_json(path)
            records[path.parents[1].name] = candidate["identity"]["factory_mac"]
        require(len(set(records.values())) == len(records), "DUPLICATE_MAC_ARCHIVE")
        found = next((rid for rid, mac in records.items() if mac == identity["factory_mac"]), None)
        if self.recovery_id:
            require(self.device_dir.is_dir(), "RECOVERY_ID_NOT_FOUND")
            original = read_json(self.device_dir / "original/manifest.json")
            require(original["identity"]["factory_mac"] == identity["factory_mac"], "LIVE_MAC_MISMATCH")
        elif found:
            self.recovery_id = found
        else:
            while True:
                rid = index.recovery_id()
                if not (self.root / rid).exists() and not (self.mirror / rid).exists():
                    self.recovery_id = rid
                    break
            mkdir(self.device_dir)
            for name in ("snapshots", "operations", "tool"):
                mkdir(self.device_dir / name)
        if operation.path.parent != self.device_dir / "operations":
            target = self.device_dir / "operations" / operation.id
            os.rename(operation.path, target)
            fsync_dir(target.parent)
            fsync_dir(operation.path.parent)
            operation.path = target
        return self.recovery_id

    @contextlib.contextmanager
    def hardware(self, port, operation):
        from .device import efuse_capture, Session
        require(self.locked, "ARCHIVE_LOCK_REQUIRED")
        with hardware_lock(port):
            evidence = mkdir(operation.path / "acquisition")
            masks = efuse_capture(port, evidence)
            with Session(port, evidence, masks) as session:
                self.select_identity(session.identity, operation)
                # Enrollment can move the operation after Session has stored paths.
                session.evidence_dir = operation.path / "acquisition"
                self.hardware_session = session
                try:
                    yield session
                finally:
                    self.hardware_session = None

    def snapshot_path(self, snapshot):
        component(snapshot)
        path = self.device_dir / "original" if snapshot == "original" else self.device_dir / "snapshots" / snapshot
        require(not path.is_symlink() and path.resolve().is_relative_to(self.device_dir.resolve()), "UNSAFE_SNAPSHOT_PATH")
        require(not self.device_dir.is_symlink() and not path.parent.is_symlink(), "UNSAFE_SNAPSHOT_PATH")
        return path

    def snapshot_manifest(self, snapshot):
        return read_json(self.snapshot_path(snapshot) / "manifest.json")

    def capture(self, session, operator, reason):
        require(self.hardware_session is session and self.locked, "CAPTURE_REQUIRES_OWNED_LOCKS")
        operation = self.operation("capture")
        started = utc()
        kind = "original" if not (self.device_dir / "original").exists() else "snapshot"
        sid = "original" if kind == "original" else started.replace(":", "").replace(".", "-") + "-" + str(uuid.uuid4())
        final = self.snapshot_path(sid)
        partial = mkdir(final.parent / (sid + "." + str(uuid.uuid4()) + ".partial"))
        try:
            initial = session.identify()
            session.read_full(partial / "flash.bin")
            session.read_full(partial / "flash-second-read.bin")
            require(session.identify() == initial, "IDENTITY_CHANGED_DURING_CAPTURE")
            reads = [{"path": name, "offset": 0, "length": FLASH_SIZE, "sha256": hash_file(partial / name),
                      "acquisition_uuid": str(uuid.uuid4()), "return_code": 0}
                     for name in ("flash.bin", "flash-second-read.bin")]
            require(equal_files(partial / "flash.bin", partial / "flash-second-read.bin")
                    and reads[0]["sha256"] == reads[1]["sha256"], "INDEPENDENT_READS_MISMATCH")
            image = (partial / "flash.bin").read_bytes()
            parsed = layout.inspect(image)
            for name in ("efuses.json", "efuses-readable.bin"):
                write_new(partial / name, (session.evidence_dir / name).read_bytes())
            json_new(partial / "security-info.txt", initial["security"])
            json_new(partial / "flash-info.txt", {k: initial[k] for k in ("chip", "package", "revision", "jedec_id", "flash_size")})
            write_new(partial / "partition-table.bin", image[0x8000:0x9000])
            json_new(partial / "partitions.json", parsed)
            write_new(partial / "acquisition.log", (session.evidence_dir / "efuse-acquisition.log").read_bytes()
                      + b"\nTwo independent public-API full reads completed without reset; return codes 0,0.\n")
            write_new(partial / "recovery-instructions.txt", INSTRUCTIONS.encode())
            manifest = {"schema_version": 1, "recovery_id": self.recovery_id, "snapshot_id": sid,
                        "snapshot_kind": kind, "capture_started_utc": started, "capture_finished_utc": utc(),
                        "identity": identity_valid(initial), "observations": self.port_observations(session.port),
                        "esptool_version": ESPTOOL_VERSION, "tool": revision(),
                        "session_uuid": session.token, "read_mode": "stub; no-reset; explicit 4MB; two full independent reads",
                        "reads": reads, "layout": parsed, "operator_label": operator, "reason": reason,
                        "original_link": None if kind == "original" else {"snapshot_id": "original", "manifest_sha256": hash_file(self.device_dir / "original/manifest.json")},
                        "acquisition_return_codes": {"efuse": 0, "read_one": 0, "read_two": 0},
                        "duplicate_intent": {"location": str(self.mirror / self.recovery_id / final.relative_to(self.device_dir)),
                                             "medium_label": self.config["mirror_medium_label"]},
                        "files": file_inventory(partial), "read_protection": initial["read_protection"]}
            json_new(partial / "manifest.json", manifest)
            fsync_dir(partial)
            publish(partial, final)
            try:
                receipt = self.duplicate(sid)
            except Exception as error:
                operation.finish("AWAITING_SECOND_COPY", snapshot_id=sid,
                                 condition=getattr(error, "condition", "SECOND_COPY_IO_FAILED"), private_exception=repr(error))
                raise RecoveryError("AWAITING_SECOND_COPY") from error
            operation.finish("BACKUP_VERIFIED", snapshot_id=sid, READ_VERIFIED=True,
                             STOCK_IMAGE_VALID=parsed["STOCK_IMAGE_VALID"], LAYOUT_SUPPORTED=parsed["LAYOUT_SUPPORTED"], backup=receipt)
            self.mirror_operation(operation)
            self.verify(sid)
            session.fresh = sid
            if kind == "original":
                self.create_receipt(manifest)
                self.save_tool()
            index.rebuild(self.root)
            return sid
        except BaseException as error:
            session.fresh = None
            operation.failure(error)
            raise

    @staticmethod
    def port_observations(port):
        from .device import ports
        return {"port": port, "usb": next((p for p in ports() if p["port"] == port), {}), "identity": False}

    def duplicate(self, snapshot):
        evidence = configuration.independent(self.config)
        source = self.snapshot_path(snapshot)
        target = self.mirror / source.relative_to(self.root)
        inventory = file_inventory(source)
        if target.exists():
            verify_inventory(target, inventory)
            require(file_inventory(target) == inventory, "MIRROR_EXTRA_FILES")
        else:
            mirror_directory(source, target)
        verify_inventory(source, inventory)
        return {"manifest_sha256": hash_file(source / "manifest.json"), "files": inventory,
                "duplicate_path": str(target), "medium": evidence, "verified_utc": utc()}

    def verify(self, snapshot):
        configuration.independent(self.config)
        path = self.snapshot_path(snapshot)
        manifest = self.verify_primary(snapshot)
        copies = self.mirror / path.relative_to(self.root)
        verify_inventory(copies, file_inventory(path))
        require(file_inventory(copies) == file_inventory(path), "MIRROR_FILE_SET_MISMATCH")
        signed = [r for r in self.results() if r.get("backup", {}).get("manifest_sha256") == hash_file(path / "manifest.json")
                  and r.get("READ_VERIFIED") is True]
        require(signed, "BACKUP_SIGNOFF_MISSING")
        require(any(r["backup"]["files"] == file_inventory(path) for r in signed), "BACKUP_SIGNOFF_HASH_MISMATCH")
        return manifest

    def verify_primary(self, snapshot):
        path = self.snapshot_path(snapshot)
        manifest = read_json(path / "manifest.json")
        identity_valid(manifest["identity"])
        require(manifest["schema_version"] == 1 and manifest["snapshot_id"] == snapshot
                and manifest["recovery_id"] == self.recovery_id, "SNAPSHOT_METADATA_MISMATCH")
        require(manifest["esptool_version"] == ESPTOOL_VERSION, "EVIDENCE_TOOL_VERSION_MISMATCH")
        required = {"flash.bin", "flash-second-read.bin", "security-info.txt", "flash-info.txt",
                    "partition-table.bin", "partitions.json", "recovery-instructions.txt"}
        if "import_provenance" not in manifest:
            required |= {"efuses.json", "efuses-readable.bin", "acquisition.log"}
        else:
            provenance = manifest["import_provenance"]
            required |= {component(provenance[k]) for k in
                         ("source_manifest_file", "export_manifest_file", "backup_receipt_file")}
        require(required <= manifest["files"].keys(), "CAPTURE_REQUIRED_FILES_MISSING")
        verify_inventory(path, manifest["files"])
        require(set(file_inventory(path)) == set(manifest["files"]) | {"manifest.json"}, "CAPTURE_FILE_SET_MISMATCH")
        reads = manifest["reads"]
        require(len(reads) == 2 and reads[0]["acquisition_uuid"] != reads[1]["acquisition_uuid"], "INDEPENDENT_ACQUISITION_EVIDENCE_MISSING")
        for record, name in zip(reads, ("flash.bin", "flash-second-read.bin")):
            require(record["path"] == name and record["offset"] == 0 and record["length"] == FLASH_SIZE
                    and (path / name).stat().st_size == FLASH_SIZE and record["return_code"] == 0
                    and hash_file(path / name) == record["sha256"], "FULL_READ_EVIDENCE_INVALID")
        require(reads[0]["sha256"] == reads[1]["sha256"] and equal_files(path / "flash.bin", path / "flash-second-read.bin"), "INDEPENDENT_READS_MISMATCH")
        if "efuses-readable.bin" in manifest["files"]:
            require(hash_file(path / "efuses-readable.bin") == manifest["identity"]["efuse_sha256"], "EFUSE_EVIDENCE_HASH_MISMATCH")
        return manifest

    def results(self):
        return sorted((read_json(p) for p in (self.device_dir / "operations").glob("*/result.json")), key=lambda r: r["utc"])

    def mirror_operation(self, operation):
        # Operation results contain the immutable capture sign-off; acquisition
        # logs/subdirectories are intentionally not needed for index rebuilding.
        target = self.mirror / self.recovery_id / "operations" / operation.id
        private_tree(target.parent)
        partial = mkdir(target.parent / (operation.id + ".partial"))
        for path in operation.path.iterdir():
            if path.is_file():
                write_new(partial / path.name, path.read_bytes())
                require(hash_file(partial / path.name) == hash_file(path), "OPERATION_COPY_MISMATCH")
        publish(partial, target)

    def create_receipt(self, manifest):
        path = self.device_dir / "receipt.txt"
        if path.exists():
            return
        text = (f"Private recovery reference: {self.recovery_id}\nEnrollment: {manifest['capture_started_utc']}\n"
                f"Badge: **:**:**:**:{manifest['identity']['factory_mac'][-4:]}\n"
                f"Original SHA-256: {manifest['reads'][0]['sha256']}\n"
                "Return with the same badge and this receipt. This reference does not authorize backup disclosure.\n")
        write_new(path, text.encode())
        mirror_receipt = self.mirror / self.recovery_id / "receipt.txt"
        if not mirror_receipt.exists():
            write_new(mirror_receipt, path.read_bytes())
        require(hash_file(path) == hash_file(mirror_receipt), "RECEIPT_MIRROR_MISMATCH")

    def save_tool(self):
        root = Path(__file__).resolve().parent.parent
        final = self.device_dir / "tool" / revision()["source_sha256"]
        if final.exists():
            return
        partial = mkdir(final.parent / (final.name + ".partial"))
        mkdir(partial / "zt_badge")
        for source in [root / "badge_tool.py", root / "requirements.txt", *sorted((root / "zt_badge").glob("*.py"))]:
            write_new(partial / source.relative_to(root), source.read_bytes())
        fsync_dir(partial / "zt_badge")
        publish(partial, final)
        mirror = self.mirror / final.relative_to(self.root)
        private_tree(mirror.parent)
        stage = mkdir(mirror.parent / (mirror.name + ".partial"))
        mkdir(stage / "zt_badge")
        for source in final.rglob("*"):
            if source.is_file():
                target = stage / source.relative_to(final)
                write_new(target, source.read_bytes())
                require(hash_file(source) == hash_file(target), "TOOL_COPY_MISMATCH")
        fsync_dir(stage / "zt_badge")
        publish(stage, mirror)
