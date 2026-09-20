#include "zt_gateway.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static zt_gateway_message_t message;
static zt_json_workspace_t workspace;

static zt_err_t decode(const char *json_text, unsigned target, unsigned expiry)
{
    char frame[2048];
    int len = snprintf(frame, sizeof(frame),
        "{\"v\":1,\"t\":\"commands\",\"id\":7,\"ts\":1000,\"state_rev\":3,"
        "\"commands\":[{\"seq\":12,\"round_id\":\"0123456789abcdef\","
        "\"type\":\"ANNOUNCE\",\"target\":%u,\"valid_until_elapsed_ms\":%u,\"text\":%s}]}",
        target, expiry, json_text);
    assert(len > 0 && (size_t)len < sizeof(frame));
    return zt_gateway_decode(frame, (size_t)len, &workspace, &message);
}

int main(void)
{
    assert(decode("\"Stay alert!\"", 255, 30000) == ZT_OK);
    const zt_gateway_command_t *command = &message.body.commands.entries[0];
    assert(command->type == ZT_CMD_ANNOUNCE && command->seq == 12);
    assert(command->round_id == UINT64_C(0x0123456789abcdef));
    assert(command->target == 255 && command->valid_until_elapsed_ms == 30000);
    assert(command->args.announce.text_len == 11);
    assert(!memcmp(command->args.announce.text, "Stay alert!", 11));

    char maximum[ZT_ANNOUNCE_MAX_LEN + 3];
    maximum[0] = '"';
    memset(maximum + 1, 'A', ZT_ANNOUNCE_MAX_LEN);
    maximum[ZT_ANNOUNCE_MAX_LEN + 1] = '"';
    maximum[ZT_ANNOUNCE_MAX_LEN + 2] = 0;
    assert(decode(maximum, 19, 600000) == ZT_OK);
    assert(command->args.announce.text_len == ZT_ANNOUNCE_MAX_LEN);
    char oversized[ZT_ANNOUNCE_MAX_LEN + 4];
    oversized[0] = '"';
    memset(oversized + 1, 'A', ZT_ANNOUNCE_MAX_LEN + 1);
    oversized[ZT_ANNOUNCE_MAX_LEN + 2] = '"';
    oversized[ZT_ANNOUNCE_MAX_LEN + 3] = 0;
    assert(decode(oversized, 255, 30000) != ZT_OK);
    assert(decode("\"\"", 255, 30000) != ZT_OK);
    assert(decode("null", 255, 30000) != ZT_OK);
    assert(decode("42", 255, 30000) != ZT_OK);
    assert(decode("\"hello\\nworld\"", 255, 30000) != ZT_OK);
    assert(decode("\"hello\\u0000world\"", 255, 30000) != ZT_OK);
    assert(decode("\"hello\\u007fworld\"", 255, 30000) != ZT_OK);
    assert(decode("\"hello\\u00e9world\"", 255, 30000) != ZT_OK);
    assert(decode("\"alert\"", 20, 30000) != ZT_OK);
    assert(decode("\"alert\"", 255, UINT32_MAX) != ZT_OK);
    assert(decode("\"say \\\"go\\\"!\"", 0, 30000) == ZT_OK);
    assert(command->args.announce.text_len == 9);
    assert(!memcmp(command->args.announce.text, "say \"go\"!", 9));
    puts("announcement codec: bounded ASCII, targeting and expiry checks passed");
}
