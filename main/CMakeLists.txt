set(FTP_SERVER_VENDOR_SOURCE "${CMAKE_SOURCE_DIR}/components/esp-idf-ftpServer/main/ftp.c")

idf_component_register(SRCS "main.c"
                       "audio_output_switch.c"
                       "bluetooth_service.c"
                       "cartridge_service.c"
                       "console_service.c"
                       "esp_flash_dispatcher.c"
                       "ftp_service.c"
                       "i2s_service.c"
                       "player_service.c"
                       "power_mgmt_service.c"
                       "ramdisk_service.c"
                       "runtime_env.c"
                       "script_service.c"
                       "wifi_service.c"
                       "${FTP_SERVER_VENDOR_SOURCE}"
                   PRIV_REQUIRES spi_flash bt esp_psram nvs_flash esp_hw_support esp_audio_codec esp_ringbuf freertos esp_event esp_wifi esp_netif esp_eth esp_pm lwip log console fatfs sdmmc heap esp_system esp_timer esp_vfs_console esp_driver_i2s esp_driver_gpio joltwallet__littlefs espressif__wasmachine_core
                   INCLUDE_DIRS "."
                   PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/components/esp-idf-ftpServer/main")

set(FLASHFS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/flashfs")
set(FLASHFS_STAGE_DIR "${CMAKE_BINARY_DIR}/flashfs_image")
set(QEMU_SD_IMAGE "${CMAKE_BINARY_DIR}/sd_image.bin")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(stage_flashfs ALL
                  COMMAND ${CMAKE_COMMAND} -E remove_directory "${FLASHFS_STAGE_DIR}"
                  COMMAND ${CMAKE_COMMAND} -E make_directory "${FLASHFS_STAGE_DIR}"
                  COMMAND ${CMAKE_COMMAND} -E copy_directory "${FLASHFS_SOURCE_DIR}" "${FLASHFS_STAGE_DIR}"
                  VERBATIM)

add_custom_command(OUTPUT "${QEMU_SD_IMAGE}"
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/ensure_sd_image.py"
              --image-path "${QEMU_SD_IMAGE}"
              --size-bytes "1073741824"
              --source-dir "${CMAKE_SOURCE_DIR}/tools/out"
          COMMENT "Generating QEMU SD image from tools/out"
                  VERBATIM)

add_custom_target(qemu_sd_image ALL
                  DEPENDS "${QEMU_SD_IMAGE}")

littlefs_create_partition_image(littlefs "${FLASHFS_STAGE_DIR}" FLASH_IN_PROJECT DEPENDS stage_flashfs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           CONFIG_FTP_USER=\"${CONFIG_FTP_SVC_USER}\"
                           CONFIG_FTP_PASSWORD=\"${CONFIG_FTP_SVC_PASSWORD}\")

target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=esp_flash_read"
                      "-Wl,--wrap=esp_flash_write"
                      "-Wl,--wrap=esp_flash_write_encrypted"
                      "-Wl,--wrap=esp_flash_erase_region"
                      "-Wl,--wrap=esp_flash_erase_chip")
