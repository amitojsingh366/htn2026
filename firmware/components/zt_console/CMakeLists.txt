idf_component_register(
    SRCS "console.c"
    INCLUDE_DIRS "."
    REQUIRES zt_contract
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_timer log freertos
)
