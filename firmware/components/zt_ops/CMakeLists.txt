idf_component_register(
    SRCS "ops.c"
    REQUIRES zt_contract
    PRIV_REQUIRES zt_console zt_store zt_game zt_radio zt_ui zt_demo
                  esp_hw_support esp_system heap spi_flash nvs_flash freertos
)
