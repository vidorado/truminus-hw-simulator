idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer mbedtls esp_driver_usb_serial_jtag
)
