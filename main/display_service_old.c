#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pin_defs.h"

#define DISPLAY_SVC_SPI_HOST SPI2_HOST
#define DISPLAY_SVC_WIDTH 128
#define DISPLAY_SVC_HEIGHT 64
#define DISPLAY_SVC_PAGE_COUNT (DISPLAY_SVC_HEIGHT / 8)
#define DISPLAY_SVC_FRAMEBUFFER_SIZE (DISPLAY_SVC_WIDTH * DISPLAY_SVC_HEIGHT / 8)
#define DISPLAY_SVC_PIXEL_CLOCK_HZ (1 * 1000 * 1000)
#define DISPLAY_SVC_TRANS_QUEUE_DEPTH 4
#define DISPLAY_SVC_RESET_ACTIVE_LEVEL 0
#define DISPLAY_SVC_TEST_TASK_STACK_SIZE 4096
#define DISPLAY_SVC_TEST_TASK_PRIORITY 3
#define DISPLAY_SVC_TEST_STEP_MS 1000

static const char *TAG = "display_svc";

typedef struct
{
    uint8_t column_offset;
    bool mirror_x;
    bool mirror_y;
} display_service_variant_t;

static esp_lcd_panel_io_handle_t s_panel_io;
static TaskHandle_t s_test_task_handle;
static bool s_spi_bus_initialized;
static bool s_initialized;
static uint8_t s_active_column_offset = 2;
static uint8_t s_framebuffer[DISPLAY_SVC_FRAMEBUFFER_SIZE];

static const uint8_t GLYPH_SPACE[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t GLYPH_D[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
static const uint8_t GLYPH_E[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
static const uint8_t GLYPH_G[5] = {0x3E, 0x41, 0x49, 0x49, 0x3A};
static const uint8_t GLYPH_I[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
static const uint8_t GLYPH_K[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
static const uint8_t GLYPH_L[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
static const uint8_t GLYPH_O[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
static const uint8_t GLYPH_P[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
static const uint8_t GLYPH_S[5] = {0x26, 0x49, 0x49, 0x49, 0x32};
static const uint8_t GLYPH_T[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
static const uint8_t GLYPH_5[5] = {0x27, 0x45, 0x45, 0x45, 0x39};

static TickType_t display_service_test_step_ticks(void)
{
    TickType_t step_ticks = pdMS_TO_TICKS(DISPLAY_SVC_TEST_STEP_MS);
    return step_ticks == 0 ? 1 : step_ticks;
}

static const uint8_t *display_service_glyph_for_char(char ch)
{
    switch (ch)
    {
    case 'D':
        return GLYPH_D;
    case 'E':
        return GLYPH_E;
    case 'G':
        return GLYPH_G;
    case 'I':
        return GLYPH_I;
    case 'K':
        return GLYPH_K;
    case 'L':
        return GLYPH_L;
    case 'O':
        return GLYPH_O;
    case 'P':
        return GLYPH_P;
    case 'S':
        return GLYPH_S;
    case 'T':
        return GLYPH_T;
    case '5':
        return GLYPH_5;
    case ' ':
    default:
        return GLYPH_SPACE;
    }
}

static size_t display_service_text_width(const char *text)
{
    size_t length = 0;

    while (text != NULL && text[length] != '\0')
    {
        length++;
    }

    if (length == 0)
    {
        return 0;
    }

    return (length * 6U) - 1U;
}

static void display_service_draw_text(uint8_t page, uint8_t x_start, const char *text)
{
    uint8_t x = x_start;

    if (page >= DISPLAY_SVC_PAGE_COUNT || text == NULL)
    {
        return;
    }

    while (*text != '\0' && x < DISPLAY_SVC_WIDTH)
    {
        const uint8_t *glyph = display_service_glyph_for_char(*text++);

        for (size_t column = 0; column < 5U && x < DISPLAY_SVC_WIDTH; column++)
        {
            s_framebuffer[(page * DISPLAY_SVC_WIDTH) + x] = glyph[column];
            x++;
        }

        if (x < DISPLAY_SVC_WIDTH)
        {
            s_framebuffer[(page * DISPLAY_SVC_WIDTH) + x] = 0x00;
            x++;
        }
    }
}

static void display_service_draw_centered_text(uint8_t page, const char *text)
{
    size_t text_width = display_service_text_width(text);
    uint8_t x = 0;

    if (text_width < DISPLAY_SVC_WIDTH)
    {
        x = (uint8_t)((DISPLAY_SVC_WIDTH - text_width) / 2U);
    }

    display_service_draw_text(page, x, text);
}

static esp_err_t display_service_send_command(uint8_t command, const uint8_t *params, size_t param_size)
{
    ESP_RETURN_ON_FALSE(s_panel_io != NULL, ESP_ERR_INVALID_STATE, TAG, "panel IO is not initialized");
    return esp_lcd_panel_io_tx_param(s_panel_io, command, params, param_size);
}

static esp_err_t display_service_set_entire_display_on(bool enabled)
{
    return display_service_send_command(enabled ? 0xA5 : 0xA4, NULL, 0);
}

static esp_err_t display_service_set_inverted(bool enabled)
{
    return display_service_send_command(enabled ? 0xA7 : 0xA6, NULL, 0);
}

static void display_service_reset_panel(void)
{
    gpio_set_level(HAL_DISPLAY_RESET_PIN, DISPLAY_SVC_RESET_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(HAL_DISPLAY_RESET_PIN, !DISPLAY_SVC_RESET_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void display_service_fill(uint8_t byte)
{
    memset(s_framebuffer, byte, sizeof(s_framebuffer));
}

static void display_service_render_checkerboard(void)
{
    for (uint8_t page = 0; page < DISPLAY_SVC_PAGE_COUNT; page++)
    {
        for (uint8_t column = 0; column < DISPLAY_SVC_WIDTH; column++)
        {
            s_framebuffer[(page * DISPLAY_SVC_WIDTH) + column] = ((page + column) & 1U) == 0 ? 0xAA : 0x55;
        }
    }
}

static void display_service_render_vertical_bars(void)
{
    for (uint8_t page = 0; page < DISPLAY_SVC_PAGE_COUNT; page++)
    {
        for (uint8_t column = 0; column < DISPLAY_SVC_WIDTH; column++)
        {
            s_framebuffer[(page * DISPLAY_SVC_WIDTH) + column] = ((column / 8U) & 1U) == 0 ? 0xFF : 0x00;
        }
    }
}

static void display_service_render_horizontal_bars(void)
{
    for (uint8_t page = 0; page < DISPLAY_SVC_PAGE_COUNT; page++)
    {
        memset(&s_framebuffer[page * DISPLAY_SVC_WIDTH],
               (page & 1U) == 0 ? 0xFF : 0x00,
               DISPLAY_SVC_WIDTH);
    }
}

static void display_service_render_border(void)
{
    display_service_fill(0x00);

    memset(&s_framebuffer[0], 0xFF, DISPLAY_SVC_WIDTH);
    memset(&s_framebuffer[(DISPLAY_SVC_PAGE_COUNT - 1U) * DISPLAY_SVC_WIDTH], 0xFF, DISPLAY_SVC_WIDTH);

    for (uint8_t page = 0; page < DISPLAY_SVC_PAGE_COUNT; page++)
    {
        s_framebuffer[(page * DISPLAY_SVC_WIDTH)] = 0xFF;
        s_framebuffer[(page * DISPLAY_SVC_WIDTH) + (DISPLAY_SVC_WIDTH - 1U)] = 0xFF;
    }
}

static esp_err_t display_service_init_panel_variant(const display_service_variant_t *variant)
{
    static const struct
    {
        uint8_t command;
        uint8_t params[2];
        uint8_t param_size;
    } base_init_sequence[] = {
        {0xAE, {0x00, 0x00}, 0},
        {0xD5, {0x80, 0x00}, 1},
        {0xA8, {DISPLAY_SVC_HEIGHT - 1U, 0x00}, 1},
        {0xD3, {0x00, 0x00}, 1},
        {0x40, {0x00, 0x00}, 0},
        {0xAD, {0x8B, 0x00}, 1},
        {0xDA, {0x12, 0x00}, 1},
        {0x81, {0x80, 0x00}, 1},
        {0xD9, {0x22, 0x00}, 1},
        {0xDB, {0x35, 0x00}, 1},
        {0xA4, {0x00, 0x00}, 0},
        {0xA6, {0x00, 0x00}, 0},
        {0x2E, {0x00, 0x00}, 0},
    };

    ESP_RETURN_ON_FALSE(variant != NULL, ESP_ERR_INVALID_ARG, TAG, "display variant is null");

    s_active_column_offset = variant->column_offset;
    display_service_reset_panel();

    for (size_t index = 0; index < sizeof(base_init_sequence) / sizeof(base_init_sequence[0]); index++)
    {
        esp_err_t err = display_service_send_command(base_init_sequence[index].command,
                                                     base_init_sequence[index].param_size > 0 ? base_init_sequence[index].params : NULL,
                                                     base_init_sequence[index].param_size);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    esp_err_t err = display_service_send_command(variant->mirror_x ? 0xA1 : 0xA0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_service_send_command(variant->mirror_y ? 0xC8 : 0xC0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = display_service_send_command(0xAF, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t display_service_flush(void)
{
    ESP_RETURN_ON_FALSE(s_panel_io != NULL, ESP_ERR_INVALID_STATE, TAG, "panel IO is not initialized");

    for (uint8_t page = 0; page < DISPLAY_SVC_PAGE_COUNT; page++)
    {
        uint8_t lower_column = (uint8_t)(s_active_column_offset & 0x0F);
        uint8_t upper_column = (uint8_t)(0x10 | ((s_active_column_offset >> 4) & 0x0F));
        esp_err_t err = display_service_send_command((uint8_t)(0xB0 | page), NULL, 0);
        if (err != ESP_OK)
        {
            return err;
        }

        err = display_service_send_command(lower_column, NULL, 0);
        if (err != ESP_OK)
        {
            return err;
        }

        err = display_service_send_command(upper_column, NULL, 0);
        if (err != ESP_OK)
        {
            return err;
        }

        err = esp_lcd_panel_io_tx_color(s_panel_io,
                                        -1,
                                        &s_framebuffer[page * DISPLAY_SVC_WIDTH],
                                        DISPLAY_SVC_WIDTH);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t display_service_render_test_message(void)
{
    display_service_render_border();
    display_service_draw_centered_text(1, "OLED TEST");
    display_service_draw_centered_text(4, "GPIO5 OK");
    return display_service_flush();
}

static void display_service_log_step_error(const char *step_name, size_t variant_index, esp_err_t err)
{
    ESP_LOGW(TAG,
             "display test step failed (variant=%u step=%s err=%s)",
             (unsigned)variant_index,
             step_name,
             esp_err_to_name(err));
}

static void display_service_test_task(void *param)
{
    static const display_service_variant_t variants[] = {
        {.column_offset = 0, .mirror_x = false, .mirror_y = false},
        {.column_offset = 0, .mirror_x = true, .mirror_y = false},
        {.column_offset = 0, .mirror_x = false, .mirror_y = true},
        {.column_offset = 0, .mirror_x = true, .mirror_y = true},
        {.column_offset = 2, .mirror_x = false, .mirror_y = false},
        {.column_offset = 2, .mirror_x = true, .mirror_y = false},
        {.column_offset = 2, .mirror_x = false, .mirror_y = true},
        {.column_offset = 2, .mirror_x = true, .mirror_y = true},
    };
    TickType_t step_ticks = display_service_test_step_ticks();

    (void)param;

    for (;;)
    {
        for (size_t index = 0; index < sizeof(variants) / sizeof(variants[0]); index++)
        {
            const display_service_variant_t *variant = &variants[index];
            esp_err_t err = display_service_init_panel_variant(variant);
            if (err != ESP_OK)
            {
                display_service_log_step_error("init", index, err);
                continue;
            }

            ESP_LOGI(TAG,
                     "display variant %u offset=%u mirror_x=%d mirror_y=%d",
                     (unsigned)index,
                     (unsigned)variant->column_offset,
                     variant->mirror_x ? 1 : 0,
                     variant->mirror_y ? 1 : 0);

            err = display_service_set_entire_display_on(true);
            if (err != ESP_OK)
            {
                display_service_log_step_error("all_on", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            err = display_service_set_entire_display_on(false);
            if (err != ESP_OK)
            {
                display_service_log_step_error("resume_ram", index, err);
                continue;
            }

            display_service_fill(0x00);
            err = display_service_flush();
            if (err != ESP_OK)
            {
                display_service_log_step_error("black", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            display_service_fill(0xFF);
            err = display_service_flush();
            if (err != ESP_OK)
            {
                display_service_log_step_error("white", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            display_service_render_checkerboard();
            err = display_service_flush();
            if (err != ESP_OK)
            {
                display_service_log_step_error("checkerboard", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            display_service_render_vertical_bars();
            err = display_service_flush();
            if (err != ESP_OK)
            {
                display_service_log_step_error("vertical_bars", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            display_service_render_horizontal_bars();
            err = display_service_flush();
            if (err != ESP_OK)
            {
                display_service_log_step_error("horizontal_bars", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            err = display_service_set_inverted(true);
            if (err != ESP_OK)
            {
                display_service_log_step_error("invert_on", index, err);
                continue;
            }
            vTaskDelay(step_ticks);

            err = display_service_set_inverted(false);
            if (err != ESP_OK)
            {
                display_service_log_step_error("invert_off", index, err);
                continue;
            }

            err = display_service_render_test_message();
            if (err != ESP_OK)
            {
                display_service_log_step_error("text", index, err);
                continue;
            }
            vTaskDelay(step_ticks);
        }
    }
}

static void display_service_deinit(void)
{
    if (s_panel_io != NULL)
    {
        (void)esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }

    if (s_spi_bus_initialized)
    {
        (void)spi_bus_free(DISPLAY_SVC_SPI_HOST);
        s_spi_bus_initialized = false;
    }

    gpio_reset_pin(HAL_DISPLAY_RESET_PIN);

    s_test_task_handle = NULL;
    s_initialized = false;
}

esp_err_t display_service_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = HAL_DISPLAY_SPI_CLK_PIN,
        .mosi_io_num = HAL_DISPLAY_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_SVC_FRAMEBUFFER_SIZE,
    };
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = HAL_DISPLAY_CS_PIN,
        .dc_gpio_num = HAL_DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SVC_PIXEL_CLOCK_HZ,
        .trans_queue_depth = DISPLAY_SVC_TRANS_QUEUE_DEPTH,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
        },
    };
    gpio_config_t reset_gpio_config = {
        .pin_bit_mask = 1ULL << HAL_DISPLAY_RESET_PIN,
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

    err = spi_bus_initialize(DISPLAY_SVC_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        return err;
    }
    s_spi_bus_initialized = true;

    err = gpio_config(&reset_gpio_config);
    if (err != ESP_OK)
    {
        display_service_deinit();
        return err;
    }

    display_service_reset_panel();

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SVC_SPI_HOST,
                                   &io_config,
                                   &s_panel_io);
    if (err != ESP_OK)
    {
        display_service_deinit();
        return err;
    }

    if (xTaskCreatePinnedToCore(display_service_test_task,
                                "display_svc",
                                DISPLAY_SVC_TEST_TASK_STACK_SIZE,
                                NULL,
                                DISPLAY_SVC_TEST_TASK_PRIORITY,
                                &s_test_task_handle,
                                0) != pdPASS)
    {
        display_service_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "OLED brute-force test running (clk=%d mosi=%d rst=%d dc=%d cs=%d)",
             HAL_DISPLAY_SPI_CLK_PIN,
             HAL_DISPLAY_SPI_MOSI_PIN,
             HAL_DISPLAY_RESET_PIN,
             HAL_DISPLAY_DC_PIN,
             HAL_DISPLAY_CS_PIN);
    return ESP_OK;
}