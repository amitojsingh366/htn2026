"""Cooperative archive locks plus port-holder detection. Never terminate holders."""
import contextlib
import fcntl
import hashlib
import os
import shutil
import stat
import subprocess
from pathlib import Path
from .errors import require
from .config import local_dir
from .manifest import private_tree


@contextlib.contextmanager
def process_lock(path):
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            require(False, "LOCK_HELD_BY_ANOTHER_PROCESS")
        yield
    finally:
        os.close(fd)


@contextlib.contextmanager
def archive_lock(config):
    # One local lock also prevents reconfiguration/parallel commands across archives.
    private_tree(local_dir())
    with process_lock(local_dir() / "process.lock"), process_lock(config["archive"] / ".archive.lock"):
        yield


@contextlib.contextmanager
def hardware_lock(port):
    path = Path(port)
    require(path.is_absolute() and str(path).startswith("/dev/"), "LOCAL_SERIAL_PORT_REQUIRED")
    require(stat.S_ISCHR(path.stat().st_mode), "LOCAL_SERIAL_PORT_REQUIRED")
    canonical = str(path.resolve()).replace("/dev/cu.", "/dev/tty.")
    lock = local_dir() / ("port-" + hashlib.sha256(canonical.encode()).hexdigest() + ".lock")
    with process_lock(lock):
        check_port_holders(port)
        yield


def check_port_holders(port, allow_self=False):
    executable = shutil.which("lsof")
    require(executable is not None, "PORT_HOLDER_CHECK_UNAVAILABLE")
    aliases = [str(port)]
    other = str(port).replace("/dev/cu.", "/dev/tty.") if "/dev/cu." in str(port) else str(port).replace("/dev/tty.", "/dev/cu.")
    if other != str(port) and Path(other).exists():
        aliases.append(other)
    result = subprocess.run([executable, "-t", "--", *aliases], capture_output=True)
    require(result.returncode in (0, 1), "PORT_HOLDER_CHECK_FAILED")
    holders = set(result.stdout.decode().split())
    if allow_self:
        holders.discard(str(os.getpid()))
    require(not holders, "PORT_HELD_BY_ANOTHER_PROCESS", holders=sorted(holders))
