#include "zt_gateway.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static zt_gateway_message_t message = {
    .envelope = {ZT_GATEWAY_SCHEMA_VERSION, ZT_GATEWAY_DIAGNOSTICS, UINT32_MAX, 1700000000000ULL},
    .body.diagnostics = {
        .round_id = UINT64_MAX, .host_boot = UINT64_MAX, .fw = "LIVEG010", .seq = UINT32_MAX,
        .uptime_ms = 9007199254740991ULL, .reconnects = UINT32_MAX, .failures = UINT32_MAX,
        .dropped_messages = UINT32_MAX, .send_failures = UINT32_MAX,
        .heap_free_bytes = UINT32_MAX, .heap_min_free_bytes = UINT32_MAX,
        .heap_largest_free_bytes = UINT32_MAX, .gateway_stack_free_bytes = UINT32_MAX,
        .websocket_stack_free_bytes = UINT32_MAX, .last_error = ZT_ERR_NETWORK,
    },
};

static void rejected(zt_gateway_message_t *m)
{
    char out[ZT_GATEWAY_TX_BUFFER_BYTES];
    size_t written = 123;
    assert(zt_gateway_encode(m, out, sizeof(out), &written) != ZT_OK);
    assert(written == 0 && out[0] == 0);
}

static void welcome(const char *capability, zt_err_t expected, unsigned enabled)
{
    char text[512];
    snprintf(text, sizeof(text), "{\"v\":1,\"t\":\"welcome\",\"id\":1,\"ts\":1700000000000,"
        "\"server_time_ms\":1700000000000,\"resume\":\"ok\",\"phase\":\"lobby\","
        "\"round_id\":null,\"state_rev\":1,\"resume_from\":0,\"snapshot_id\":0,\"snapshot_pages\":0%s}", capability);
    zt_json_workspace_t workspace;
    zt_gateway_message_t decoded;
    assert(zt_gateway_decode(text, strlen(text), &workspace, &decoded) == expected);
    if (expected == ZT_OK) assert(decoded.body.welcome.diagnostics == enabled);
}

int main(void)
{
    char out[ZT_GATEWAY_TX_BUFFER_BYTES];
    size_t written = 0;
    assert(zt_gateway_encode(&message, out, sizeof(out), &written) == ZT_OK);
    assert(written == strlen(out) && written <= ZT_GATEWAY_DIAGNOSTICS_MAX_BYTES);
    puts(out); /* Independent JSON parser checks the entire serialized schema. */
    size_t needed = written;
    for (size_t capacity = 1; capacity <= needed; ++capacity) {
        unsigned char bounded[ZT_GATEWAY_TX_BUFFER_BYTES + 2];
        memset(bounded, 0xa5, sizeof(bounded));
        written = 123;
        assert(zt_gateway_encode(&message, (char *)bounded + 1, capacity, &written) == ZT_ERR_NO_SPACE);
        assert(!written && !bounded[1]);
        assert(bounded[0] == 0xa5 && bounded[capacity + 1] == 0xa5);
    }
    assert(zt_gateway_encode(&message, out, needed + 1, &written) == ZT_OK && written == needed);
    message.body.diagnostics.round_id = 0;
    assert(zt_gateway_encode(&message, out, sizeof(out), &written) == ZT_OK);
    puts(out);
    /* Control rejection errors added by sync recovery remain reportable. */
    const zt_err_t control_errors[] = {ZT_ERR_HOST_REGISTRATION, ZT_ERR_ROSTER_SIZE, ZT_ERR_ROUND_ACTIVE};
    for (unsigned i = 0; i < sizeof(control_errors) / sizeof(control_errors[0]); ++i) {
        zt_gateway_message_t control = message;
        control.body.diagnostics.last_error = control_errors[i];
        assert(zt_gateway_encode(&control, out, sizeof(out), &written) == ZT_OK);
        char expected[32];
        snprintf(expected, sizeof(expected), "\"last_error\":%u}", (unsigned)control_errors[i]);
        assert(strstr(out, expected));
    }
    zt_gateway_message_t invalid = message;
    invalid.body.diagnostics.host_boot = 0; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.seq = 0; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.uptime_ms++; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.last_error = (zt_err_t)(ZT_ERR_MAX + 1); rejected(&invalid);
    invalid = message; invalid.body.diagnostics.last_error = (zt_err_t)-1; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.fw[0] = '"'; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.fw[0] = '\n'; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.fw[1] = '\0'; rejected(&invalid);
    invalid = message; invalid.body.diagnostics.fw[ZT_BUILD_ID_LEN] = 'x'; rejected(&invalid);
    invalid = message; invalid.envelope.id = 0; rejected(&invalid);
    invalid = message; invalid.envelope.v = 2; rejected(&invalid);
    welcome("", ZT_OK, 0); /* Existing backend cannot enable diagnostics by accident. */
    welcome(",\"diagnostics\":true", ZT_OK, 1);
    welcome(",\"diagnostics\":false", ZT_OK, 0);
    welcome(",\"diagnostics\":1", ZT_ERR_PROTOCOL, 0);
    welcome(",\"diagnostics\":\"true\"", ZT_ERR_PROTOCOL, 0);
    return 0;
}
