#!/bin/sh
set -eu
firmware_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
test_output=$(mktemp -d)
trap 'rm -rf "$test_output"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I "$firmware_dir/components/zt_contract/include" \
    "$firmware_dir/tests/announcement_codec_test.c" \
    "$firmware_dir/components/zt_gateway/codec.c" \
    "$firmware_dir/components/zt_contract/ids.c" \
    -o "$test_output/announcement_codec_test"
"$test_output/announcement_codec_test"
