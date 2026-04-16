set(FTP_SERVER_VENDOR_SOURCE "${CMAKE_SOURCE_DIR}/components/esp-idf-ftpServer/main/ftp.c")

idf_component_register(SRCS "main.c"
                       "audio_output_switch.c"
                       "bluetooth_service.c"
                       "cartridge_service.c"
                       "console_service.c"
                       "ftp_service.c"
                       "i2s_service.c"
                       "player_service.c"
                       "power_mgmt_service.c"
                       "qemu_openeth_service.c"
                       "qemu_pcm_service.c"
                       "ramdisk_service.c"
                       "runtime_env.c"
                       "script_socket_env.c"
                       "script_service.c"
                       "wifi_service.c"
                       "${FTP_SERVER_VENDOR_SOURCE}"
                   PRIV_REQUIRES spi_flash bt esp_psram nvs_flash esp_hw_support esp_audio_codec esp_ringbuf freertos esp_event esp_wifi esp_netif esp_eth esp_pm lwip log console fatfs sdmmc heap esp_system esp_timer esp_vfs_console esp_driver_i2s esp_driver_gpio joltwallet__littlefs espressif__wasmachine_core
                   INCLUDE_DIRS "."
                   PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/components/esp-idf-ftpServer/main")

set(FLASHFS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/flashfs")
set(FLASHFS_STAGE_DIR "${CMAKE_BINARY_DIR}/flashfs_image")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(stage_flashfs ALL
                  COMMAND ${CMAKE_COMMAND} -E remove_directory "${FLASHFS_STAGE_DIR}"
                  COMMAND ${CMAKE_COMMAND} -E make_directory "${FLASHFS_STAGE_DIR}"
                  COMMAND ${CMAKE_COMMAND} -E copy_directory "${FLASHFS_SOURCE_DIR}" "${FLASHFS_STAGE_DIR}"
                  VERBATIM)

littlefs_create_partition_image(littlefs "${FLASHFS_STAGE_DIR}" FLASH_IN_PROJECT DEPENDS stage_flashfs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           CONFIG_FTP_USER=\"${CONFIG_FTP_SVC_USER}\"
                           CONFIG_FTP_PASSWORD=\"${CONFIG_FTP_SVC_PASSWORD}\")
