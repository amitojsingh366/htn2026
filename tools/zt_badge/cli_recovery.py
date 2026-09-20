"""Recovery command registration and sequential operator workflows.

C01 can register additional commands without editing this module. Its handlers
must use Archive.lock()/hardware(), capture()/verify(), and durable Operation
records; a previous FLASH_VERIFIED result is not a reusable write authorization.
"""
from pathlib import Path
import os
import sys
from . import revision, config, gates
from .archive import Archive, Operation
from .errors import require, RecoveryError
from .manifest import (read_json, json_new, write_new, hash_file, utc,
                       file_inventory, fsync_dir)


def confirm(prompt, expected=None):
    """Operator observations are explicit, local, and unavailable in batch mode.

    /dev/tty is preferred because it reaches the controlling terminal regardless of
    redirection. It is not always available: some terminal hosts and IDE consoles
    leave a process without an accessible controlling terminal, where opening it
    fails with ENXIO, and the operator is then real but unreachable.

    The fallback keeps the property that actually matters: a human must be present.
    It requires stdin to be an interactive terminal, so a piped or redirected answer
    still refuses rather than auto-confirming. Prompts are written to the ORIGINAL
    stderr preserved by badge_tool.private_output, because fds 1 and 2 are redirected
    into the private invocation log and a prompt written there would be invisible.

    Only an explicit y or yes (case insensitive) agrees; empty input and all other
    answers refuse. Each call reads a fresh answer. The optional expected token is
    only legacy prompt metadata for bundle.py, never an accepted answer.
    """
    if expected is not None:
        # Keep the unmodified bundle callback's full attestation, replacing only
        # its sentinel instruction. Unknown legacy prompt formats fail closed.
        suffix = f" Type {expected}:"
        require(prompt.endswith(suffix), "OPERATOR_OBSERVATION_NOT_CONFIRMED")
        prompt = prompt[:-len(suffix)] + " Do you confirm? [y/N]: "
    import zt_badge
    answer = None
    try:
        with open("/dev/tty", "r+", encoding="utf-8") as terminal:
            terminal.write(prompt)
            terminal.flush()
            answer = terminal.readline().strip()
    except (OSError, ValueError, UnicodeError):
        operator_fd = getattr(zt_badge, "OPERATOR_FD", None)
        # Fail closed unless a real person can both see the prompt and type a reply.
        try:
            if operator_fd is None or sys.stdin is None or not sys.stdin.isatty():
                raise RecoveryError("LOCAL_OPERATOR_OBSERVATION_REQUIRED")
            os.write(operator_fd, prompt.encode("utf-8"))
            line = sys.stdin.readline()
        except (OSError, ValueError, UnicodeError) as error:
            raise RecoveryError("LOCAL_OPERATOR_OBSERVATION_REQUIRED") from error
        require(line != "", "LOCAL_OPERATOR_OBSERVATION_REQUIRED")
        answer = line.strip()
    require(answer.lower() in ("y", "yes"), "OPERATOR_OBSERVATION_NOT_CONFIRMED")


def agreement():
    confirm("Do you confirm the participant agreed to this capture and any requested write, and the selected badge is in ROM download mode? [y/N]: ")


def stock_observation(archive, restored):
    confirm("RESTORE_VERIFIED: normal power cycle is now permitted. Release START, then verify stock launcher/display, buttons, saved state and app availability with the owner. Do not dump wallet secrets. Have you observed all these stock functions with the owner after the normal power cycle? [y/N]: ")
    accepted = False
    if restored.get("rehearsal"):
        confirm("Do you confirm this first-badge restoration rehearsal passed for this tool and recovery workflow? [y/N]: ")
        accepted = True
    observation = archive.operation("stock-boot-observation")
    observation.finish("STOCK_BOOT_CONFIRMED", restore_operation=restored["operation_uuid"],
                       rehearsal_accepted=accepted, observation="Operator and owner confirmed stock functions after preboot equality; no postboot hash claim")
    archive.mirror_operation(observation)


def execute(args):
    command = args.command
    if command == "archive-init":
        confirm("Confirm both existing private roots are outside repositories, shared/cloud folders and worker access, on encrypted independent durable hardware. The medium label records organizer evidence; filesystem IDs alone cannot prove independent hardware. Do you confirm? [y/N]: ")
        config.initialize(args.archive, args.mirror, args.mirror_medium_label)
        return "ARCHIVE_CONFIGURED. Filesystem IDs alone cannot prove independent hardware; retain the organizer's recorded medium label."
    if command == "ports":
        from .device import ports
        import re
        candidates = [p["port"] for p in ports() if re.fullmatch(r"/dev/[A-Za-z0-9._/-]+", p["port"])]
        return "Candidate port observations (a port name never identifies a badge):\n" + "\n".join(candidates)
    if command == "release-manifest":
        from .release import build_release
        build_release(args.build, args.output, confirm)
        return "RELEASE_MANIFEST_CREATED. Review the local output directory."
    archive = Archive(config.load(), getattr(args, "recovery_id", None))
    with archive.lock():
        operation = archive.operation(command)
        try:
            result = execute_archive(args, archive, operation)
            if not operation.done:
                operation.finish("COMPLETED")
            return result
        except BaseException as error:
            operation.failure(error)
            raise


def execute_archive(args, archive, operation):
    command = args.command
    if command == "import-bundle":
        from .bundle import import_bundle
        snapshot = import_bundle(archive, args.source)
        operation.finish("IMPORTED", snapshot_id=snapshot)
        return "BUNDLE_IMPORTED_AND_VERIFIED. Device identity is still checked live before restoration."
    if command == "verify":
        # May finish an AWAITING_SECOND_COPY capture, without changing its files.
        manifest = archive.verify_primary(args.snapshot)
        path = archive.snapshot_path(args.snapshot)
        from .manifest import verify_inventory, equal_files
        verify_inventory(path, manifest["files"])
        require(equal_files(path / "flash.bin", path / "flash-second-read.bin"), "INDEPENDENT_READS_MISMATCH")
        receipt = archive.duplicate(args.snapshot)
        operation.finish("BACKUP_VERIFIED", snapshot_id=args.snapshot, READ_VERIFIED=True, backup=receipt)
        archive.mirror_operation(operation)
        archive.verify(args.snapshot)
        archive.create_receipt(archive.snapshot_manifest("original"))
        archive.save_tool()
        from .index import rebuild
        rebuild(archive.root)
        return "READ_VERIFIED; BACKUP_VERIFIED. Bootability and layout are separate recorded results."
    if command == "status":
        original = archive.snapshot_manifest("original")
        results = archive.results()
        # Fixed vocabulary only; recovery IDs, operator labels and file paths stay private.
        vocabulary = {"READ_VERIFIED", "BACKUP_VERIFIED", "FLASH_VERIFIED", "RESTORE_VERIFIED", "STOCK_BOOT_CONFIRMED", "COMMISSIONED", "AWAITING_SECOND_COPY", "FAILED", "INSTALL_INITIALIZED"}
        statuses = sorted({r["status"] for r in results if r.get("status") in vocabulary})
        return "Recorded outcomes (historical, not a fresh gate): " + ", ".join(statuses) + ". Original retained."
    if command == "receipt":
        archive.verify("original")
        output = Path(args.output)
        require(output.is_absolute() and output.parent.is_dir(), "LOCAL_ABSOLUTE_OUTPUT_REQUIRED")
        config.physical_devices(output.parent)
        write_new(output, (archive.device_dir / "receipt.txt").read_bytes())
        require(hash_file(output) == hash_file(archive.device_dir / "receipt.txt"), "RECEIPT_COPY_MISMATCH")
        return "PRIVATE_RECEIPT_WRITTEN. Recovery reference is in that file only."
    if command == "export":
        from .bundle import export_bundle
        final = export_bundle(archive, args.snapshot, args.destination, confirm)
        operation.finish("OWNER_BUNDLE_VERIFIED", destination=str(final), export_manifest_sha256=hash_file(final / "export-manifest.json"))
        return "OWNER_BUNDLE_VERIFIED. Local files only; nothing was uploaded or sent."
    agreement()
    # Resolve archived targets before serial work. flash-game checks original in
    # its optional probe, then again in the ordinary gate if capture is required.
    if command not in ("enroll", "snapshot", "flash-game"):
        archive.verify("original")
    if command == "restore":
        archive.verify(args.snapshot)
    release = getattr(args, "release", None)
    if release:
        from .release import verify_release
        verify_release(Path(release))
    with archive.hardware(args.port, operation) as session:
        # select_identity already requires the same full MAC. Preserve readable
        # evidence even if other state changed; every mutation and commissioning
        # gate below still requires the complete identity/security match.
        owner = getattr(args, "owner_label", None)
        if owner is None:
            owner = archive.snapshot_manifest("original")["operator_label"]
        # flash-game decides inside guarded_write whether the fresh capture is
        # necessary; all other commands retain their unconditional capture.
        if command != "flash-game":
            current = archive.capture(session, owner, getattr(args, "reason", command))
        if command in ("enroll", "snapshot"):
            operation.finish("BACKUP_VERIFIED", snapshot_id=current)
            return "READ_VERIFIED; BACKUP_VERIFIED. Receipt and independent layout/bootability results are private."
        if command == "commission":
            results = archive.results()
            writes = [r for r in results if r.get("status") in ("FLASH_VERIFIED", "RESTORE_VERIFIED")]
            require(writes and writes[-1]["status"] == "FLASH_VERIFIED", "NO_CURRENT_VERIFIED_FLASH_OPERATION")
            current_manifest = archive.verify(current)
            gates.commissioning(archive, current_manifest, writes[-1], release).enforce()
            confirm("Do you confirm the badge completed provisioning, radio initialization and reached a working game screen before this capture? [y/N]: ")
            # One capture observes one exercised path; run commission for each role.
            try:
                with open("/dev/tty", "r+", encoding="utf-8") as terminal:
                    terminal.write("Record the exercised radio role: type p/player or h/host:\n")
                    terminal.flush()
                    role = terminal.readline().strip()
            except OSError as error:
                raise RecoveryError("LOCAL_OPERATOR_OBSERVATION_REQUIRED") from error
            role = {"p": "player", "h": "host"}.get(role, role)
            require(role in ("player", "host"), "COMMISSION_ROLE_OBSERVATION_REQUIRED")
            json_new(operation.path / "commissioning.json", {"preflash_snapshot": writes[-1]["current_snapshot"],
                     "post_snapshot": current, "protected_ranges_equal": True, "app_equal": True,
                     "efuse_security_equal": True, "observed_role": role})
            operation.finish("COMMISSIONED", flash_operation=writes[-1]["operation_uuid"],
                             snapshot_id=current, observed_roles=[role], release_hash=hash_file(Path(release)))
            archive.mirror_operation(operation)
            return "COMMISSIONED. Protected stock ranges, padded app, and readable security/eFuse state matched."
        from .device import guarded_write
        if command == "flash-game":
            guarded_write(session, archive, operation, "original", release=release)
        elif command in ("restore", "rehearse-restore"):
            guarded_write(session, archive, operation, getattr(args, "snapshot", "original"),
                          rehearsal=command == "rehearse-restore")
        else:
            raise RecoveryError("UNKNOWN_RECOVERY_COMMAND")
    archive.mirror_operation(operation)
    if command in ("restore", "rehearse-restore"):
        if command == "restore" and args.snapshot != "original":
            return "RESTORE_VERIFIED before boot. Normal power cycle is permitted; no stock-boot observation was recorded for this checkpoint."
        stock_observation(archive, read_json(operation.path / "result.json"))
        return "RESTORE_VERIFIED before boot; STOCK_BOOT_CONFIRMED as a separate operator observation."
    return "FLASH_VERIFIED before boot. Normal power cycle with START released is now permitted."


def register(subparsers):
    """Optional console module uses the same register(subparsers) protocol.

    Register an argparse parser and set_defaults(func=handler). A handler receives
    Namespace and returns a sanitized fixed status string. It executes inside the
    entry point's private stdout/stderr capture. It must never print private values.
    """
    def command(name, help_text, fields):
        parser = subparsers.add_parser(name, help=help_text, description=help_text)
        for field in fields:
            parser.add_argument("--" + field, required=True)
        parser.set_defaults(func=execute)
        return parser
    command("archive-init", "Configure existing private roots on independent durable media.", ["archive", "mirror", "mirror-medium-label"])
    command("ports", "List candidate ports as observations, never badge identity.", [])
    command("enroll", "Capture and mirror original state after participant agreement.", ["port", "owner-label"])
    command("snapshot", "Capture two independent full reads and verify both durable copies.", ["recovery-id", "port", "reason"])
    command("verify", "Rehash both reads and both durable copies; finish a pending mirror.", ["recovery-id", "snapshot"])
    command("receipt", "Write a private local receipt without printing its recovery reference.", ["recovery-id", "output"])
    command("rehearse-restore", "Capture, restore original, compare 4 MiB, then observe stock boot.", ["recovery-id", "port"])
    command("flash-game", "Automatic capture selection, atomic gates, app-only write and recorded preboot verification.", ["recovery-id", "port", "release"])
    command("commission", "Capture after initialized firmware use and compare protected stock bytes.", ["recovery-id", "port", "release"])
    command("restore", "Capture current state and restore a verified same-device full snapshot.", ["recovery-id", "snapshot", "port"])
    command("export", "Create a verified private owner bundle on a local destination.", ["recovery-id", "snapshot", "destination"])
    command("status", "Show sanitized historical outcomes; does not authorize a write.", ["recovery-id"])
    command("release-manifest", "Bind reviewed ESP-IDF build artifacts, configuration and clean source.", ["build", "output"])
    command("import-bundle", "Validate a local owner bundle before recreating mirrored archive state.", ["source"])
