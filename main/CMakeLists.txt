idf_component_register(
    SRCS "truma_sim.c" "openair_sim.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer mbedtls esp_driver_usb_serial_jtag esp_driver_uart
)

# Build the OpenAir PLUS capture personality instead of the Victron/BMS sim:
#   idf.py -DOPENAIR_SIM=1 build      (or `make openair` / `make openair-flash`)
# Default (unset) keeps the original Victron/Ultimatron/tank/Multiplus sim.
if(OPENAIR_SIM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE OPENAIR_SIM=1)
    message(STATUS "Building OpenAir PLUS simulator personality")
endif()
