# The board in use is chosen in the top-level CMakeLists.txt
# (main/boards/board_select.cmake). Only its board file is built, so each hook
# in boards/board_hooks.h has one definition.
idf_component_register(SRCS main.c
                            utility.c
                            elm327_at.c
                            timer.c
                            ws2812_led.c
                            shell.c
                            board.c
                            ${OPENDIAG_BOARD_IMPL}
                            button.c
                            slcan.c
                            j2534.c
                            j2534_scheduler.c
                            j2534_wire.c
                            j2534.pb.c
                            usb_cdc.c
                            ble_uart.c
                            can_bus.c
                            isotp.c
                            kline.c
                            kline_codec.c
                            bus_tx.c
                            bus_tx_worker.c
                            bus.c
                            j1850_pwm.c
                            j1850_pwm_codec.c
                            j1850_vpw.c
                            j1850_vpw_codec.c
                            j1850_common.c
                            j1850_lifecycle.c
                            comm_iface.c
                            vif.c
                            vif_ctrl.c
                       PRIV_REQUIRES
                            nanopb
                            esp_hal_usb
                            spi_flash
                            bt
                            nvs_flash
                            esp_adc
                            esp_ringbuf
                            esp_app_format
                            esp_driver_ledc
                            esp_driver_gpio
                            esp_driver_mcpwm
                            esp_driver_uart
                            esp_driver_rmt
                            esp_driver_gptimer
                            esp_driver_twai
                       REQUIRES driver freertos
                       INCLUDE_DIRS "")

# main/ for the shared headers (board.h, pinout.h) that the board files under
# boards/ include, and the generated board_profile.h for the board in use (see
# board_select.cmake). INCLUDE_DIRS is "", so the component adds neither itself.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${OPENDIAG_BOARD_INCLUDE}")
