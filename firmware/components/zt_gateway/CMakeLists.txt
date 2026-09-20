idf_component_register(
    SRCS "http.c" "ws.c" "codec.c" "bridge.c" "diagnostics.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES esp_http_client esp_websocket_client esp-tls esp_netif esp_timer freertos tcp_transport mbedtls esp_hw_support heap zt_game zt_radio zt_store
)
# The strict-upgrade TLS parent must preserve the socket/error hooks used by
# ESP-IDF 5.5.3's WebSocket close handling; the SDK exposes no public setters.
idf_component_get_property(tcp_transport_dir tcp_transport COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${tcp_transport_dir}/private_include")
