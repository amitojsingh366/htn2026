idf_component_register(
    SRCS "game.c" "clock.c" "peers.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES esp_timer esp_hw_support freertos zt_store zt_radio zt_hal zt_gateway
)
