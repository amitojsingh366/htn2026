idf_component_register(
    SRCS "ui.c" "font.c" "render.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES zt_hal esp_timer freertos
)
