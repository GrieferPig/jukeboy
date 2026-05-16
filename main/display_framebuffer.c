#include "display_framebuffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pin_defs.h"

#define DISPLAY_FRAMEBUFFER_SPI_HOST SPI2_HOST
#define DISPLAY_FRAMEBUFFER_SPI_CLOCK_HZ (8 * 1000 * 1000)
#define DISPLAY_FRAMEBUFFER_RESET_ACTIVE_LEVEL 0

static const char *TAG = "disp_fb";

typedef struct
{
    bool dirty;
    uint8_t min_col;
    uint8_t max_col;
} display_framebuffer_dirty_page_t;

static spi_device_handle_t s_spi_device;
static SemaphoreHandle_t s_mutex;
static bool s_spi_bus_initialized;
static bool s_initialized;
static bool s_panel_ready;
static DMA_ATTR uint8_t s_framebuffer[DISPLAY_FRAMEBUFFER_SIZE];
static display_framebuffer_dirty_page_t s_dirty_pages[DISPLAY_FRAMEBUFFER_PAGE_COUNT];

static void display_framebuffer_delay_ms(uint32_t delay_ms)
{
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);

    if (delay_ms > 0 && delay_ticks == 0)
    {
        delay_ticks = 1;
    }

    vTaskDelay(delay_ticks);
}

static void display_framebuffer_clear_dirty_locked(void)
{
    memset(s_dirty_pages, 0, sizeof(s_dirty_pages));
}

static void display_framebuffer_mark_page_span_dirty_locked(uint8_t page, uint8_t start_col, uint8_t end_col)
{
    display_framebuffer_dirty_page_t *dirty_page;

    if (page >= DISPLAY_FRAMEBUFFER_PAGE_COUNT || start_col >= DISPLAY_FRAMEBUFFER_WIDTH)
    {
        return;
    }

    if (end_col >= DISPLAY_FRAMEBUFFER_WIDTH)
    {
        end_col = DISPLAY_FRAMEBUFFER_WIDTH - 1U;
    }

    if (start_col > end_col)
    {
        return;
    }

    dirty_page = &s_dirty_pages[page];
    if (!dirty_page->dirty)
    {
        dirty_page->dirty = true;
        dirty_page->min_col = start_col;
        dirty_page->max_col = end_col;
        return;
    }

    if (start_col < dirty_page->min_col)
    {
        dirty_page->min_col = start_col;
    }

    if (end_col > dirty_page->max_col)
    {
        dirty_page->max_col = end_col;
    }
}

static void display_framebuffer_mark_all_dirty_locked(void)
{
    for (uint8_t page = 0; page < DISPLAY_FRAMEBUFFER_PAGE_COUNT; page++)
    {
        s_dirty_pages[page].dirty = true;
        s_dirty_pages[page].min_col = 0;
        s_dirty_pages[page].max_col = DISPLAY_FRAMEBUFFER_WIDTH - 1U;
    }
}

static esp_err_t display_framebuffer_transfer_bytes(bool is_data, const uint8_t *bytes, size_t byte_count)
{
    spi_transaction_t transaction = {0};

    ESP_RETURN_ON_FALSE(s_spi_device != NULL, ESP_ERR_INVALID_STATE, TAG, "SPI device is not initialized");

    if (byte_count == 0)
    {
        return ESP_OK;
    }

    transaction.length = byte_count * 8U;
    if (byte_count <= sizeof(transaction.tx_data))
    {
        transaction.flags = SPI_TRANS_USE_TXDATA;
        memcpy(transaction.tx_data, bytes, byte_count);
    }
    else
    {
        transaction.tx_buffer = bytes;
    }

    gpio_set_level(HAL_DISPLAY_DC_PIN, is_data ? 1 : 0);
    gpio_set_level(HAL_DISPLAY_CS_PIN, 0);

    esp_err_t err = spi_device_polling_transmit(s_spi_device, &transaction);

    gpio_set_level(HAL_DISPLAY_CS_PIN, 1);
    return err;
}

static esp_err_t display_framebuffer_send_command(uint8_t command)
{
    return display_framebuffer_transfer_bytes(false, &command, 1);
}

static esp_err_t display_framebuffer_send_data_block(const uint8_t *data, size_t data_size)
{
    return display_framebuffer_transfer_bytes(true, data, data_size);
}

static void display_framebuffer_reset_panel(void)
{
    gpio_set_level(HAL_DISPLAY_RESET_PIN, 1);
    display_framebuffer_delay_ms(1);
    gpio_set_level(HAL_DISPLAY_RESET_PIN, DISPLAY_FRAMEBUFFER_RESET_ACTIVE_LEVEL);
    display_framebuffer_delay_ms(10);
    gpio_set_level(HAL_DISPLAY_RESET_PIN, !DISPLAY_FRAMEBUFFER_RESET_ACTIVE_LEVEL);
    display_framebuffer_delay_ms(50);
}

static esp_err_t display_framebuffer_set_window(uint8_t start_col,
                                                uint8_t end_col,
                                                uint8_t start_page,
                                                uint8_t end_page)
{
    esp_err_t err = display_framebuffer_send_command(0x21);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_command(start_col);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_command(end_col);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_command(0x22);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_command(start_page);
    if (err != ESP_OK)
    {
        return err;
    }

    return display_framebuffer_send_command(end_page);
}

static esp_err_t display_framebuffer_flush_page_locked(uint8_t page)
{
    display_framebuffer_dirty_page_t *dirty_page = &s_dirty_pages[page];
    size_t offset;
    size_t length;
    esp_err_t err;

    if (!dirty_page->dirty)
    {
        return ESP_OK;
    }

    offset = ((size_t)page * DISPLAY_FRAMEBUFFER_WIDTH) + dirty_page->min_col;
    length = (size_t)(dirty_page->max_col - dirty_page->min_col + 1U);

    err = display_framebuffer_set_window(dirty_page->min_col,
                                         dirty_page->max_col,
                                         page,
                                         page);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_data_block(&s_framebuffer[offset], length);
    if (err != ESP_OK)
    {
        return err;
    }

    dirty_page->dirty = false;
    return ESP_OK;
}

static esp_err_t display_framebuffer_flush_full_width_run_locked(uint8_t start_page, uint8_t end_page)
{
    size_t offset = (size_t)start_page * DISPLAY_FRAMEBUFFER_WIDTH;
    size_t length = (size_t)(end_page - start_page + 1U) * DISPLAY_FRAMEBUFFER_WIDTH;
    esp_err_t err = display_framebuffer_set_window(0,
                                                   DISPLAY_FRAMEBUFFER_WIDTH - 1U,
                                                   start_page,
                                                   end_page);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_data_block(&s_framebuffer[offset], length);
    if (err != ESP_OK)
    {
        return err;
    }

    for (uint8_t page = start_page; page <= end_page; page++)
    {
        s_dirty_pages[page].dirty = false;
    }

    return ESP_OK;
}

static esp_err_t display_framebuffer_init_panel(void)
{
    esp_err_t err;

    display_framebuffer_reset_panel();

    err = spi_device_acquire_bus(s_spi_device, portMAX_DELAY);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_framebuffer_send_command(0xAE);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xD5);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x80);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xA8);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x3F);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xD3);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x00);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x40);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x8D);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x14);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x20);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x00);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xA1);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xC8);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xDA);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x12);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x81);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xCF);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xD9);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xF1);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xDB);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0x40);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xA4);
    if (err != ESP_OK)
    {
        goto done;
    }
    err = display_framebuffer_send_command(0xA6);
    if (err != ESP_OK)
    {
        goto done;
    }

    err = display_framebuffer_send_command(0xAF);

done:
    spi_device_release_bus(s_spi_device);
    return err;
}

static void display_framebuffer_deinit(void)
{
    if (s_spi_device != NULL)
    {
        (void)spi_bus_remove_device(s_spi_device);
        s_spi_device = NULL;
    }

    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    if (s_spi_bus_initialized)
    {
        (void)spi_bus_free(DISPLAY_FRAMEBUFFER_SPI_HOST);
        s_spi_bus_initialized = false;
    }

    gpio_reset_pin(HAL_DISPLAY_RESET_PIN);
    gpio_reset_pin(HAL_DISPLAY_DC_PIN);
    gpio_reset_pin(HAL_DISPLAY_CS_PIN);

    s_panel_ready = false;
    s_initialized = false;
}

esp_err_t display_framebuffer_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = HAL_DISPLAY_SPI_CLK_PIN,
        .mosi_io_num = HAL_DISPLAY_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_FRAMEBUFFER_SIZE,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = DISPLAY_FRAMEBUFFER_SPI_CLOCK_HZ,
        .mode = 0,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    gpio_config_t control_gpio_config = {
        .pin_bit_mask = (1ULL << HAL_DISPLAY_CS_PIN) |
                        (1ULL << HAL_DISPLAY_DC_PIN) |
                        (1ULL << HAL_DISPLAY_RESET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err;

    if (s_initialized)
    {
        return ESP_OK;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    display_framebuffer_clear_dirty_locked();
    s_panel_ready = false;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    err = spi_bus_initialize(DISPLAY_FRAMEBUFFER_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }
    s_spi_bus_initialized = true;

    err = gpio_config(&control_gpio_config);
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }

    gpio_set_level(HAL_DISPLAY_CS_PIN, 1);
    gpio_set_level(HAL_DISPLAY_DC_PIN, 0);
    gpio_set_level(HAL_DISPLAY_RESET_PIN, 1);

    err = spi_bus_add_device(DISPLAY_FRAMEBUFFER_SPI_HOST, &device_config, &s_spi_device);
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }

    err = display_framebuffer_init_panel();
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }

    s_panel_ready = true;
    display_framebuffer_mark_all_dirty_locked();
    err = display_framebuffer_lock(NULL);
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }
    err = display_framebuffer_flush_dirty_locked();
    display_framebuffer_unlock();
    if (err != ESP_OK)
    {
        display_framebuffer_deinit();
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OLED framebuffer ready (clk=%d mosi=%d rst=%d dc=%d cs=%d)",
             HAL_DISPLAY_SPI_CLK_PIN,
             HAL_DISPLAY_SPI_MOSI_PIN,
             HAL_DISPLAY_RESET_PIN,
             HAL_DISPLAY_DC_PIN,
             HAL_DISPLAY_CS_PIN);
    return ESP_OK;
}

esp_err_t display_framebuffer_lock(uint8_t **framebuffer_out)
{
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "framebuffer mutex is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "failed to take framebuffer mutex");

    if (framebuffer_out != NULL)
    {
        *framebuffer_out = s_framebuffer;
    }

    return ESP_OK;
}

void display_framebuffer_unlock(void)
{
    if (s_mutex != NULL)
    {
        xSemaphoreGive(s_mutex);
    }
}

void display_framebuffer_mark_dirty_area_locked(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t start_page;
    uint8_t end_page;

    if (x1 >= DISPLAY_FRAMEBUFFER_WIDTH || y1 >= DISPLAY_FRAMEBUFFER_HEIGHT)
    {
        return;
    }

    if (x2 >= DISPLAY_FRAMEBUFFER_WIDTH)
    {
        x2 = DISPLAY_FRAMEBUFFER_WIDTH - 1U;
    }
    if (y2 >= DISPLAY_FRAMEBUFFER_HEIGHT)
    {
        y2 = DISPLAY_FRAMEBUFFER_HEIGHT - 1U;
    }
    if (x1 > x2 || y1 > y2)
    {
        return;
    }

    start_page = (uint8_t)(y1 >> 3);
    end_page = (uint8_t)(y2 >> 3);
    for (uint8_t page = start_page; page <= end_page; page++)
    {
        display_framebuffer_mark_page_span_dirty_locked(page, (uint8_t)x1, (uint8_t)x2);
    }
}

esp_err_t display_framebuffer_flush_dirty_locked(void)
{
    bool have_dirty_pages = false;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s_panel_ready, ESP_ERR_INVALID_STATE, TAG, "panel is not ready");

    for (uint8_t page = 0; page < DISPLAY_FRAMEBUFFER_PAGE_COUNT; page++)
    {
        if (s_dirty_pages[page].dirty)
        {
            have_dirty_pages = true;
            break;
        }
    }

    if (!have_dirty_pages)
    {
        return ESP_OK;
    }

    err = spi_device_acquire_bus(s_spi_device, portMAX_DELAY);
    if (err != ESP_OK)
    {
        return err;
    }

    for (uint8_t page = 0; page < DISPLAY_FRAMEBUFFER_PAGE_COUNT; page++)
    {
        if (!s_dirty_pages[page].dirty)
        {
            continue;
        }

        if (s_dirty_pages[page].min_col == 0 &&
            s_dirty_pages[page].max_col == (DISPLAY_FRAMEBUFFER_WIDTH - 1U))
        {
            uint8_t end_page = page;

            while ((end_page + 1U) < DISPLAY_FRAMEBUFFER_PAGE_COUNT &&
                   s_dirty_pages[end_page + 1U].dirty &&
                   s_dirty_pages[end_page + 1U].min_col == 0 &&
                   s_dirty_pages[end_page + 1U].max_col == (DISPLAY_FRAMEBUFFER_WIDTH - 1U))
            {
                end_page++;
            }

            err = display_framebuffer_flush_full_width_run_locked(page, end_page);
            if (err != ESP_OK)
            {
                spi_device_release_bus(s_spi_device);
                return err;
            }

            page = end_page;
            continue;
        }

        err = display_framebuffer_flush_page_locked(page);
        if (err != ESP_OK)
        {
            spi_device_release_bus(s_spi_device);
            return err;
        }
    }

    spi_device_release_bus(s_spi_device);
    return ESP_OK;
}