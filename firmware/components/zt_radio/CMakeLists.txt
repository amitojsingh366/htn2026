idf_component_register(
    SRCS "radio.c" "channel.c" "mesh.c" "wire.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES esp_wifi esp_event esp_netif esp_timer esp_hw_support mbedtls freertos zt_game zt_store
)
