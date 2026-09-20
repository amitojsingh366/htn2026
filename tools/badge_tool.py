#!/usr/bin/env python3
"""Thin sanitized CLI boundary; real logic lives in zt_badge."""
import argparse
import contextlib
import os
import re
import sys
import uuid
import zt_badge
from zt_badge import cli_recovery


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's default error interpolates arguments (including recovery IDs).
        self.exit(2, "INVALID_ARGUMENTS. Use the relevant --help.\n")


def parser():
    result = Parser(description="Private, offline, same-device badge recovery. No force or arbitrary offsets.")
    subs = result.add_subparsers(dest="command", required=True)
    cli_recovery.register(subs)
    # Mandatory extension point: C01 adds this module and register(subparsers)
    # without editing R01 files. --help must also work before that packet exists.
    try:
        from zt_badge import cli_console
    except ImportError:
        pass
    else:
        cli_console.register(subs)
    return result


@contextlib.contextmanager
def private_output(stream):
    """Capture Python and native/subprocess stdout/stderr before loading esptool."""
    sys.stdout.flush()
    sys.stderr.flush()
    saved = os.dup(1), os.dup(2)
    zt_badge.OPERATOR_FD = saved[1]
    try:
        os.dup2(stream.fileno(), 1)
        os.dup2(stream.fileno(), 2)
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            yield
        stream.flush()
        os.fsync(stream.fileno())
    finally:
        zt_badge.OPERATOR_FD = None
        os.dup2(saved[0], 1)
        os.dup2(saved[1], 2)
        os.close(saved[0])
        os.close(saved[1])


def main(argv=None):
    args = parser().parse_args(argv)
    # Parsing/help imports are side-effect free and need neither esptool nor config.
    from zt_badge.config import local_dir
    from zt_badge.manifest import private_tree, mkdir, json_new, utc
    from zt_badge.errors import RecoveryError
    os.umask(0o077)
    try:
        root = private_tree(local_dir() / "invocations")
        invocation = mkdir(root / str(uuid.uuid4()))
        fd = os.open(invocation / "private.log", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream, private_output(stream):
            try:
                status = args.func(args)
            except BaseException as error:
                json_new(invocation / "failure.json", {"utc": utc(), "condition": getattr(error, "condition", "UNEXPECTED_PRIVATE_ERROR"),
                         "evidence": getattr(error, "evidence", {}), "private_exception": repr(error)})
                raise
        print(status)
        return 0
    except BaseException as error:
        code = error.condition if isinstance(error, RecoveryError) else "PRIVATE_OPERATION_FAILED"
        if not re.fullmatch(r"[A-Z_]{1,100}", code):
            code = "PRIVATE_OPERATION_FAILED"
        print(f"STOPPED: {code}. Inspect the private operation record; no automatic retry or reboot.", file=sys.stderr)
        # Comparison diagnostics contain offsets and SHA-256 only, never bytes.
        evidence = getattr(error, "evidence", {})
        ranges = evidence.get("differing_ranges", [])
        if ranges:
            safe = [(a, b) for a, b in ranges if isinstance(a, int) and isinstance(b, int) and 0 <= a < b <= 0x400000]
            print("Differing ranges [start,end): " + ", ".join(f"[0x{a:06x},0x{b:06x})" for a, b in safe[:32]), file=sys.stderr)
            if len(safe) > 32:
                print("Remaining ranges retained in the private operation record.", file=sys.stderr)
        for key in ("expected_sha256", "actual_sha256"):
            value = evidence.get(key)
            if isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value):
                print(f"{key}: {value}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
