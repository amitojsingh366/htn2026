#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -I "$task_root/firmware/components/zt_contract/include" \
  "$task_root/firmware/tests/gateway_diagnostics.c" \
  "$task_root/firmware/components/zt_gateway/codec.c" \
  "$task_root/firmware/components/zt_contract/ids.c" \
  -o "$task_tmp/gateway-diagnostics"
"$task_tmp/gateway-diagnostics" > "$task_tmp/messages.jsonl"
python3 - "$task_tmp/messages.jsonl" <<'PY'
import json
import sys

messages = [json.loads(line) for line in open(sys.argv[1])]
fields = {
    "v", "t", "id", "ts", "round_id", "host_boot", "fw", "seq", "uptime_ms",
    "reconnects", "failures", "dropped_messages", "send_failures", "heap_free_bytes",
    "heap_min_free_bytes", "heap_largest_free_bytes", "gateway_stack_free_bytes",
    "websocket_stack_free_bytes", "last_error",
}
assert len(messages) == 2
for message in messages:
    assert set(message) == fields, "Only the explicit privacy-safe schema is serialized"
    assert message["t"] == "diagnostics" and message["v"] == 1
    assert message["host_boot"] == "ffffffffffffffff" and message["fw"] == "LIVEG010"
    assert message["uptime_ms"] == 2**53 - 1 and message["last_error"] == 17
    for field in fields - {"v", "t", "ts", "round_id", "host_boot", "fw", "uptime_ms", "last_error"}:
        assert message[field] == 2**32 - 1, field
assert messages[0]["round_id"] == "ffffffffffffffff"
assert messages[1]["round_id"] is None
print("Gateway diagnostic codec: bounds, privacy allowlist, counters, capability compatibility passed")
PY
