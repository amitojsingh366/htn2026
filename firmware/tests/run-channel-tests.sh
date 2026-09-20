#!/bin/sh
set -eu
firmware_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_output=$(mktemp -d)
trap 'rm -rf "$test_output"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -I "$firmware_dir/tests/channel_stubs" \
    -I "$firmware_dir/components/zt_contract/include" \
    "$firmware_dir/tests/channel_discovery_test.c" \
    "$firmware_dir/components/zt_radio/channel.c" \
    -o "$test_output/channel_discovery_test"
# Each process starts the production owner's static state from a clean boot.
for scenario in retune queued-home-event retune-failure stale future locked lease-expiry lease-refresh newest-observation; do
    "$test_output/channel_discovery_test" "$scenario"
done
