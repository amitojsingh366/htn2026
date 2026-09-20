idf_component_register(
    SRCS "buttons.c" "lcd.c" "leds.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd led_strip esp_timer freertos
)
