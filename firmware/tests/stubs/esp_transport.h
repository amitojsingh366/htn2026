#pragma once
typedef void *esp_transport_handle_t;
int esp_transport_poll_write(esp_transport_handle_t transport, int timeout_ms);
