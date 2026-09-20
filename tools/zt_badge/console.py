"""Bounded USB JSON-Lines transport. Imported only inside console handlers."""
import contextlib
import json
import re
import secrets
import time
from .errors import RecoveryError, require
from .manifest import pairs_unique

LINE_MAX = 2048
WRITE_MAX = 192


def wire_request(request_id, operation, fields):
    require(type(request_id) is int and 0 <= request_id <= 0xffffffff,
            "CONSOLE_REQUEST_ID_INVALID")
    require("id" not in fields and "op" not in fields, "CONSOLE_RESERVED_FIELD")
    try:
        data = json.dumps({"id": request_id, "op": operation, **fields},
                          ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    except (ValueError, TypeError, UnicodeError):
        raise RecoveryError("CONSOLE_REQUEST_INVALID") from None
    require(len(data) <= LINE_MAX, "CONSOLE_REQUEST_TOO_LONG")
    return data + b"\n"


def decode_response(raw):
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs_unique,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (ValueError, UnicodeError, RecursionError, RecoveryError):
        raise RecoveryError("CONSOLE_RESPONSE_INVALID") from None
    require(type(value) is dict and len(value) <= 40, "CONSOLE_RESPONSE_INVALID")
    require(type(value.get("id")) is int and 0 <= value["id"] <= 0xffffffff
            and type(value.get("ok")) is bool, "CONSOLE_RESPONSE_ENVELOPE_INVALID")
    # The wire grammar is deliberately flat, including diagnostics and exports.
    def scalar(v):
        if type(v) is str:
            try:
                v.encode("utf-8")
            except UnicodeError:
                return False
            return "\0" not in v
        return v is None or type(v) is bool or (type(v) is int and -(2**64 - 1) <= v <= 2**64 - 1)
    require(all(isinstance(k, str) and re.fullmatch(r"[a-z0-9_]+", k) and (scalar(v) or
                (type(v) is list and len(v) <= 20 and all(scalar(x) for x in v)))
                for k, v in value.items()), "CONSOLE_RESPONSE_SHAPE_INVALID")
    if not value["ok"]:
        require(type(value.get("error")) is int and 1 <= value["error"] <= 20,
                "CONSOLE_RESPONSE_ERROR_INVALID")
    return value


class Console:
    """One exclusive connection; failure closes it. Never reconnect or replay."""
    def __init__(self, port, timeout=15):
        self.port = port
        self.timeout = timeout
        self.serial = None
        self.stack = None
        self.buffer = bytearray()
        self.next_id = secrets.randbelow(0x7fffffff) + 1

    def __enter__(self):
        from .device import dependency
        from .locking import hardware_lock, check_port_holders
        import fcntl
        import termios
        dependency()
        import serial
        self.stack = contextlib.ExitStack()
        try:
            self.stack.enter_context(hardware_lock(self.port))
            self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                                        write_timeout=1, exclusive=True)
            self.stack.callback(self.serial.close)
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.port = self.port
            self.serial.open()
            require(hasattr(termios, "TIOCEXCL"), "SERIAL_EXCLUSIVE_UNAVAILABLE")
            fcntl.ioctl(self.serial.fileno(), termios.TIOCEXCL)
            check_port_holders(self.port, allow_self=True)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise RecoveryError("CONSOLE_OPEN_FAILED") from None

    def __exit__(self, *args):
        if self.stack is not None:
            self.stack.close()
            self.stack = None
        self.serial = None
        self.buffer.clear()

    def request(self, operation, **fields):
        require(self.serial is not None, "CONSOLE_SESSION_CLOSED")
        request_id = self.next_id
        self.next_id += 1
        data = wire_request(request_id, operation, fields)
        deadline = time.monotonic() + self.timeout
        try:
            for start in range(0, len(data), WRITE_MAX):
                remaining = deadline - time.monotonic()
                require(remaining > 0, "CONSOLE_TIMEOUT")
                self.serial.write_timeout = min(1, remaining)
                chunk = data[start:start + WRITE_MAX]
                require(self.serial.write(chunk) == len(chunk), "CONSOLE_SHORT_WRITE")
            while True:
                remaining = deadline - time.monotonic()
                require(remaining > 0, "CONSOLE_TIMEOUT")
                # Consume complete lines already read before requesting more bytes.
                if b"\n" in self.buffer:
                    end = self.buffer.index(10)
                    require(end <= LINE_MAX, "CONSOLE_RESPONSE_TOO_LONG")
                    line = bytes(self.buffer[:end])
                    del self.buffer[:end + 1]
                    if not line.startswith(b"{"):
                        # Boot text and '# ' diagnostics never become evidence/logs.
                        continue
                    response = decode_response(line)
                    if response["id"] != request_id:
                        continue
                    require(response["ok"], "CONSOLE_OPERATION_REFUSED",
                            error=response.get("error"))
                    return response
                require(len(self.buffer) <= LINE_MAX, "CONSOLE_RESPONSE_TOO_LONG")
                self.serial.timeout = min(0.1, remaining)
                # A single bounded read can contain LF and a following line.
                self.buffer.extend(self.serial.read(WRITE_MAX))
        except RecoveryError:
            self.__exit__(None, None, None)
            raise
        except BaseException:
            self.__exit__(None, None, None)
            raise RecoveryError("CONSOLE_IO_FAILED") from None
        finally:
            # No request/response logging, including exception messages.
            del data
