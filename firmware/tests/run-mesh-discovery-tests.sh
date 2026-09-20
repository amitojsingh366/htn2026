#!/bin/sh
set -eu
firmware_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_output=$(mktemp -d)
trap 'rm -rf "$test_output"' EXIT HUP INT TERM
case $(uname -s) in
    Darwin) set -- -Wl,-dead_strip ;;
    *) set -- -Wl,--gc-sections -lcrypto ;;
esac
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$firmware_dir/tests/mesh_stubs" \
    -I "$firmware_dir/components/zt_contract/include" \
    "$firmware_dir/tests/mesh_discovery_test.c" \
    "$firmware_dir/components/zt_radio/mesh.c" \
    "$firmware_dir/components/zt_radio/wire.c" \
    "$@" -o "$test_output/mesh_discovery_test"
"$test_output/mesh_discovery_test"
