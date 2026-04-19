#include <string.h>

#include "diskio_impl.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "ramdisk_service.h"

#define RAMDISK_SECTOR_SIZE 512
#define RAMDISK_CLUSTER_SIZE 4096

static const char *TAG = "ramdisk_svc";

typedef struct
{
    uint8_t *storage;
    size_t storage_size;
    size_t sector_count;
    BYTE pdrv;
    FATFS *fs;
    char fat_drive[3];
    bool initialized;
} ramdisk_service_state_t;

static ramdisk_service_state_t s_state = {
    .pdrv = FF_DRV_NOT_USED,
};
EXT_RAM_BSS_ATTR static uint8_t s_ramdisk_storage[RAMDISK_SERVICE_SIZE_BYTES];

static bool ramdisk_service_bounds_valid(LBA_t sector, UINT count)
{
    if (count == 0)
    {
        return false;
    }

    if (sector >= s_state.sector_count)
    {
        return false;
    }

    if (count > (s_state.sector_count - sector))
    {
        return false;
    }

    return true;
}

static DSTATUS ramdisk_init(BYTE pdrv)
{
    if (pdrv != s_state.pdrv || s_state.storage == NULL)
    {
        return STA_NOINIT;
    }

    return 0;
}

static DSTATUS ramdisk_status(BYTE pdrv)
{
    return ramdisk_init(pdrv);
}

static DRESULT ramdisk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    size_t offset;
    size_t size;

    if (ramdisk_init(pdrv) != 0 || buff == NULL)
    {
        return RES_NOTRDY;
    }

    if (!ramdisk_service_bounds_valid(sector, count))
    {
        return RES_PARERR;
    }

    offset = ((size_t)sector) * RAMDISK_SECTOR_SIZE;
    size = ((size_t)count) * RAMDISK_SECTOR_SIZE;
    memcpy(buff, s_state.storage + offset, size);
    return RES_OK;
}

static DRESULT ramdisk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    size_t offset;
    size_t size;

    if (ramdisk_init(pdrv) != 0 || buff == NULL)
    {
        return RES_NOTRDY;
    }

    if (!ramdisk_service_bounds_valid(sector, count))
    {
        return RES_PARERR;
    }

    offset = ((size_t)sector) * RAMDISK_SECTOR_SIZE;
    size = ((size_t)count) * RAMDISK_SECTOR_SIZE;
    memcpy(s_state.storage + offset, buff, size);
    return RES_OK;
}

static DRESULT ramdisk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (ramdisk_init(pdrv) != 0)
    {
        return RES_NOTRDY;
    }

    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (buff == NULL)
        {
            return RES_PARERR;
        }
        *((LBA_t *)buff) = (LBA_t)s_state.sector_count;
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == NULL)
        {
            return RES_PARERR;
        }
        *((WORD *)buff) = RAMDISK_SECTOR_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (buff == NULL)
        {
            return RES_PARERR;
        }
        *((DWORD *)buff) = 1;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

static esp_err_t ramdisk_service_format_and_mount(void)
{
    const ff_diskio_impl_t ramdisk_impl = {
        .init = ramdisk_init,
        .status = ramdisk_status,
        .read = ramdisk_read,
        .write = ramdisk_write,
        .ioctl = ramdisk_ioctl,
    };
    const esp_vfs_fat_conf_t conf = {
        .base_path = RAMDISK_SERVICE_MOUNT_PATH,
        .fat_drive = s_state.fat_drive,
        .max_files = 8,
    };
    FRESULT fresult;
    esp_err_t err;
    void *workbuf;
    const MKFS_PARM mkfs_params = {
        .fmt = (BYTE)(FM_ANY | FM_SFD),
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = RAMDISK_CLUSTER_SIZE,
    };

    ff_diskio_register(s_state.pdrv, &ramdisk_impl);

    err = esp_vfs_fat_register_cfg(&conf, &s_state.fs);
    if (err != ESP_OK)
    {
        ff_diskio_unregister(s_state.pdrv);
        return err;
    }

    fresult = f_mount(s_state.fs, s_state.fat_drive, 1);
    if (fresult == FR_OK)
    {
        return ESP_OK;
    }

    if (fresult != FR_NO_FILESYSTEM)
    {
        ESP_LOGE(TAG, "f_mount(%s) failed: %d", s_state.fat_drive, (int)fresult);
        esp_vfs_fat_unregister_path(RAMDISK_SERVICE_MOUNT_PATH);
        ff_diskio_unregister(s_state.pdrv);
        return ESP_FAIL;
    }

    workbuf = ff_memalloc(FF_MAX_SS);
    if (workbuf == NULL)
    {
        esp_vfs_fat_unregister_path(RAMDISK_SERVICE_MOUNT_PATH);
        ff_diskio_unregister(s_state.pdrv);
        return ESP_ERR_NO_MEM;
    }

    fresult = f_mkfs(s_state.fat_drive, &mkfs_params, workbuf, FF_MAX_SS);
    ff_memfree(workbuf);
    if (fresult != FR_OK)
    {
        ESP_LOGE(TAG, "f_mkfs(%s) failed: %d", s_state.fat_drive, (int)fresult);
        esp_vfs_fat_unregister_path(RAMDISK_SERVICE_MOUNT_PATH);
        ff_diskio_unregister(s_state.pdrv);
        return ESP_FAIL;
    }

    fresult = f_mount(s_state.fs, s_state.fat_drive, 1);
    if (fresult != FR_OK)
    {
        ESP_LOGE(TAG, "f_mount(%s) after format failed: %d", s_state.fat_drive, (int)fresult);
        esp_vfs_fat_unregister_path(RAMDISK_SERVICE_MOUNT_PATH);
        ff_diskio_unregister(s_state.pdrv);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ramdisk_service_init(void)
{
    esp_err_t err;

    if (s_state.initialized)
    {
        return ESP_OK;
    }

    s_state.storage = s_ramdisk_storage;
    memset(s_state.storage, 0, RAMDISK_SERVICE_SIZE_BYTES);
    s_state.storage_size = RAMDISK_SERVICE_SIZE_BYTES;
    s_state.sector_count = RAMDISK_SERVICE_SIZE_BYTES / RAMDISK_SECTOR_SIZE;

    err = ff_diskio_get_drive(&s_state.pdrv);
    if (err != ESP_OK)
    {
        s_state.storage = NULL;
        return err;
    }

    s_state.fat_drive[0] = (char)('0' + s_state.pdrv);
    s_state.fat_drive[1] = ':';
    s_state.fat_drive[2] = '\0';

    err = ramdisk_service_format_and_mount();
    if (err != ESP_OK)
    {
        s_state.storage = NULL;
        s_state.pdrv = FF_DRV_NOT_USED;
        s_state.storage_size = 0;
        s_state.sector_count = 0;
        s_state.fs = NULL;
        s_state.fat_drive[0] = '\0';
        return err;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG,
             "mounted %u KiB PSRAM ramdisk at %s using drive %s",
             (unsigned)(s_state.storage_size / 1024),
             RAMDISK_SERVICE_MOUNT_PATH,
             s_state.fat_drive);
    return ESP_OK;
}