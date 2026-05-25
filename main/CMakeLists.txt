idf_component_register(
    SRCS "truma_sim.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer mbedtls esp_driver_usb_serial_jtag esp_driver_uart
)
