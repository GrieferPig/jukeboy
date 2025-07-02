#include "serial_interface.h"
#include "audio/audio_player.h"
#include "macros.h"

// ESP-IDF specific headers
#include "driver/uart.h"
#include "esp_log.h"

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Define constants for UART
#define UART_PORT_NUM UART_NUM_0 // Default console UART
#define UART_RX_BUFFER_SIZE (1024)
#define TASK_STACK_SIZE (4096)
#define TASK_PRIORITY (2)

static const char *SERIAL_TAG = "serial";

static void print_help()
{
    printf("\n=== Audio Player Commands ===\n");
    printf("space  - Toggle pause\n");
    printf("n      - Next track\n");
    printf("b      - Previous track\n");
    printf("s      - Toggle shuffle\n");
    printf("f      - Fast forward 10 seconds\n");
    printf("r      - Rewind 5 seconds\n");
    printf("+      - Volume up\n");
    printf("-      - Volume down\n");
    printf("h      - Show this help\n");
    printf("==============================\n\n");
}

static void serial_interface_task(void *parameter)
{
    // Buffer to hold incoming data from UART
    uint8_t *data = (uint8_t *)malloc(UART_RX_BUFFER_SIZE);
    if (!data)
    {
        ESP_LOGE(SERIAL_TAG, "Failed to allocate memory for UART buffer");
        vTaskDelete(NULL);
        return;
    }

    print_help();

    while (true)
    {
        // Read data from the UART
        int len = uart_read_bytes(UART_PORT_NUM, data, UART_RX_BUFFER_SIZE, 20 / portTICK_PERIOD_MS);

        if (len > 0)
        {
            // We only care about the first character for single-key commands
            char input = data[0];

            AudioCommand cmd;
            bool valid_command = true;

            switch (input)
            {
            case ' ': // Toggle pause
                cmd.type = CMD_TOGGLE_PAUSE;
                ESP_LOGI(SERIAL_TAG, "Toggling pause");
                break;

            case 'n': // Next track
                cmd.type = CMD_NEXT_TRACK;
                ESP_LOGI(SERIAL_TAG, "Next track");
                break;

            case 'b': // Previous track
                cmd.type = CMD_PREV_TRACK;
                ESP_LOGI(SERIAL_TAG, "Previous track");
                break;

            case 's': // Toggle shuffle
                cmd.type = CMD_TOGGLE_SHUFFLE;
                ESP_LOGI(SERIAL_TAG, "Toggling shuffle");
                break;

            case 'f': // Fast forward
                cmd.type = CMD_FFWD_10SEC;
                ESP_LOGI(SERIAL_TAG, "Fast forward 10 seconds");
                break;

            case 'r': // Rewind
                cmd.type = CMD_REWIND_5SEC;
                ESP_LOGI(SERIAL_TAG, "Rewind 5 seconds");
                break;

            case '+': // Volume up
                cmd.type = CMD_VOLUME_INC;
                ESP_LOGI(SERIAL_TAG, "Volume up");
                break;

            case '-': // Volume down
                cmd.type = CMD_VOLUME_DEC;
                ESP_LOGI(SERIAL_TAG, "Volume down");
                break;

            case 'h': // Help
                print_help();
                valid_command = false;
                break;

            default:
                // Ignore newline/carriage return characters
                if (input != '\n' && input != '\r')
                {
                    printf("Unknown command: %c. Type 'h' for help.\n", input);
                }
                valid_command = false;
                break;
            }

            if (valid_command)
            {
                audio_player_send_command(&cmd);
            }
        }
        // No vTaskDelay is needed here because uart_read_bytes blocks,
        // preventing this task from starving other tasks.
    }

    free(data);
    vTaskDelete(NULL);
}

void serial_interface_init()
{
    ESP_LOGI(SERIAL_TAG, "Starting serial interface task...");

    /*
     * Configure UART. Note that `esp_log` functions already use this UART,
     * so it's often pre-configured. However, explicit configuration is good practice.
     * If you are using the default USB-to-serial chip, you can often skip this
     * and just call uart_driver_install.
     */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUFFER_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));

    // Set UART pins (using default pins for UART0)
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    BaseType_t result = xTaskCreate(
        serial_interface_task,
        "serial_interface",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        NULL);

    if (result != pdPASS)
    {
        ESP_LOGE(SERIAL_TAG, "Failed to create serial interface task");
    }
}
