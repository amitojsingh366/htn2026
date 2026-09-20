idf_component_register(
    SRCS "store.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES nvs_flash esp_partition esp_hw_support spi_flash esp_rom freertos mbedtls zt_radio
)
