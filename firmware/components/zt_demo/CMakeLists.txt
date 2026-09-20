idf_component_register(
    SRCS "demo.c"
    REQUIRES zt_contract
    PRIV_REQUIRES zt_game zt_radio zt_ui esp_timer freertos
)
