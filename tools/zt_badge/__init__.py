"""Private, offline recovery tool. Importing this package performs no I/O."""

# Set by badge_tool.private_output to the preserved original stderr fd. Operator
# prompts must reach the real terminal even though fds 1 and 2 are redirected into
# the private invocation log.
OPERATOR_FD = None

VERSION = "1.0.0"
BASE_REVISION = "fc762e6865b408a078769d0a5a8f19cbafa6f1a7"
FLASH_SIZE = 0x400000
APP_OFFSET = 0x10000
APP_SIZE = 0x2A0000
TAIL_OFFSET = 0x3F0000
ESPTOOL_VERSION = "5.4.0"


def revision():
    """Bind records to the actual tool sources, including uncommitted work."""
    import hashlib
    from pathlib import Path
    root = Path(__file__).resolve().parent.parent
    files = [root / "badge_tool.py", root / "requirements.txt"]
    files += sorted((root / "zt_badge").glob("*.py"))
    digest = hashlib.sha256()
    for path in files:
        digest.update(str(path.relative_to(root)).encode() + b"\0")
        with path.open("rb") as stream:
            digest.update(stream.read())
    return {"version": VERSION, "base_commit": BASE_REVISION,
            "source_sha256": digest.hexdigest()}
