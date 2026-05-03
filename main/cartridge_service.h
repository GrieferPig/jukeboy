#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "jukeboy_formats.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(CARTRIDGE_SERVICE_EVENT);

    typedef enum
    {
        CARTRIDGE_SVC_EVENT_STARTED,
        CARTRIDGE_SVC_EVENT_INSERTED,
        CARTRIDGE_SVC_EVENT_MOUNTED,
        CARTRIDGE_SVC_EVENT_UNMOUNTED,
    } cartridge_service_event_id_t;

    typedef enum
    {
        CARTRIDGE_STATUS_EMPTY,
        CARTRIDGE_STATUS_READY,
        CARTRIDGE_STATUS_INVALID,
    } cartridge_status_t;

    /**
     * Hardware configuration for the SD card SDMMC peripheral.
     * All three signals must be specified; the SDMMC host is operated in
     * 1-bit SDIO mode using the GPIO matrix so any GPIO numbers are accepted.
     */
    typedef struct
    {
        int clk_gpio;            /**< Clock signal GPIO (e.g. 14) */
        int cmd_gpio;            /**< Command signal GPIO (e.g. 15) */
        int d0_gpio;             /**< Data-0 signal GPIO (e.g. 2) */
        const char *mount_point; /**< VFS mount path, e.g. "/sdcard" */
    } cartridge_service_config_t;

#define CARTRIDGE_SERVICE_CONFIG_DEFAULT() \
    {                                      \
        .clk_gpio = 14,                    \
        .cmd_gpio = 15,                    \
        .d0_gpio = 2,                      \
        .mount_point = "/sdcard",          \
    }

#define CARTRIDGE_SERVICE_READ_BUF_SIZE (64 * 1024)

    /**
     * Initialise the cartridge service: stores the hardware configuration so
     * that subsequent mount/unmount calls know which pins and path to use.
     * Does NOT initialise the SDMMC peripheral or mount the filesystem.
     * Call once before cartridge_service_mount().
     */
    esp_err_t cartridge_service_init(const cartridge_service_config_t *config);

    /**
     * Mount the SD card FAT filesystem at the configured mount point.
     * Logs card information on success.
     * Returns ESP_OK if the card was mounted successfully.
     */
    esp_err_t cartridge_service_mount(void);

    /**
     * Unmount the SD card FAT filesystem.
     * Safe to call even if the card is not currently mounted (returns ESP_OK).
     */
    esp_err_t cartridge_service_unmount(void);

    /**
     * Return the configured VFS mount point for the cartridge filesystem.
     */
    const char *cartridge_service_get_mount_point(void);

    /**
     * Returns whether a cartridge (SD card) is physically present.
     * This is currently a stub that always returns true; replace with a
     * card-detect GPIO read when hardware support is available.
     */
    bool cartridge_service_is_inserted(void);

    /**
     * Returns whether the SD card FAT filesystem is currently mounted.
     */
    bool cartridge_service_is_mounted(void);

    /**
     * Return the current cartridge metadata status.
     */
    cartridge_status_t cartridge_service_get_status(void);

    /**
     * Return a human-readable name for the cartridge status.
     */
    const char *cartridge_service_status_name(cartridge_status_t status);

    /**
     * Return the loaded metadata header, or NULL if metadata is unavailable.
     * The returned pointer remains valid until the cartridge is reloaded or unmounted.
     */
    const jukeboy_jbm_header_t *cartridge_service_get_metadata_header(void);
    uint32_t cartridge_service_get_metadata_version(void);
    uint32_t cartridge_service_get_metadata_checksum(void);

    /**
     * Return the number of loaded metadata tracks.
     */
    size_t cartridge_service_get_metadata_track_count(void);

    /**
     * Return the metadata track at the given index, or NULL if unavailable.
     */
    const jukeboy_jbm_track_t *cartridge_service_get_metadata_track(size_t index);

    const char *cartridge_service_get_album_name(void);
    const char *cartridge_service_get_album_description(void);
    const char *cartridge_service_get_album_artist(void);
    uint32_t cartridge_service_get_album_year(void);
    uint32_t cartridge_service_get_album_duration_sec(void);
    const char *cartridge_service_get_album_genre(void);
    const char *cartridge_service_get_album_tag(size_t index);
    const char *cartridge_service_get_track_name(size_t index);
    const char *cartridge_service_get_track_artists(size_t index);
    uint32_t cartridge_service_get_track_duration_sec(size_t index);
    uint32_t cartridge_service_get_track_file_num(size_t index);

    /**
    * Start an asynchronous 64KB read from the given file at the specified
     * byte offset. The service manages file
     * open/close internally: if filename differs from the currently open file,
     * the old file is closed and the new one opened.
     *
    * Data is read into one of two 64KB PSRAM double-buffers. When the read
     * completes, the task identified by notify_task receives a task
     * notification (xTaskNotifyGive). Call cartridge_service_get_read_result()
     * after the notification to obtain the data pointer and length.
     *
     * Only one async read may be in flight at a time.
     *
     * @param filename     File path (relative to mount point root, e.g. "000.jba")
     * @param offset       byte offset into the file
     * @param notify_task  Task handle to notify on completion
     * @return ESP_OK if the request was accepted
     */
    esp_err_t cartridge_service_read_chunk_async(const char *filename,
                                                 size_t offset,
                                                 TaskHandle_t notify_task);

    /**
     * Retrieve the result of the last completed async read.
     *
     * @param[out] out_data  Pointer to the read buffer (valid until next read)
     * @param[out] out_len   Number of bytes actually read (0 on error / EOF)
     * @return ESP_OK if a valid result is available, ESP_ERR_NOT_FOUND if the
     *         file open failed, or ESP_FAIL on read error.
     */
    esp_err_t cartridge_service_get_read_result(const uint8_t **out_data,
                                                size_t *out_len);

    /**
     * Close the currently open file (if any). Safe to call at any time.
     */
    void cartridge_service_close_file(void);

#ifdef __cplusplus
}
#endif
