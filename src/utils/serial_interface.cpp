#include "serial_interface.h"
#include "audio/audio_player.h"
#include "macros.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

const char *SERIAL_TAG = "serial";

static void print_help()
{
    Serial.println("\n=== Audio Player Commands ===");
    // Serial.println("p<num> - Play track (e.g., p0, p1, p2)");
    Serial.println("space  - Toggle pause");
    Serial.println("n      - Next track");
    Serial.println("b      - Previous track");
    // Serial.println("v<num> - Set volume shift (e.g., v1, v5, v10)");
    Serial.println("s      - Toggle shuffle");
    Serial.println("f      - Fast forward 10 seconds");
    Serial.println("r      - Rewind 5 seconds");
    Serial.println("+      - Volume up");
    Serial.println("-      - Volume down");
    Serial.println("h      - Show this help");
    Serial.println("==============================\n");
}

static void serial_interface_task(void *parameter)
{
    print_help();

    while (true)
    {
        if (Serial.available())
        {
            char input = Serial.read();

            // Check for space before trimming
            // bool is_space_command = (input.length() > 0 && input.charAt(0) == ' ');

            // input.trim();

            // if (input.length() == 0 && !is_space_command)
            // {
            //     vTaskDelay(pdMS_TO_TICKS(10));
            //     continue;
            // }

            AudioCommand cmd;
            bool valid_command = true;

            // char first_char = is_space_command ? ' ' : input.charAt(0);

            // switch (first_char)
            switch (input)
            {
                // case 'p': // Play track
                //     if (input.length() > 1)
                //     {
                //         cmd.type = CMD_PLAY_TRACK;
                //         cmd.params.track_number = input.substring(1).toInt();
                //         ESP_LOGI(SERIAL_TAG, "Playing track %d", cmd.params.track_number);
                //     }
                //     else
                //     {
                //         valid_command = false;
                //     }
                //     break;

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

                // case 'v': // Set volume shift
                //     if (input.length() > 1)
                //     {
                //         cmd.type = CMD_SET_VOLUME_SHIFT;
                //         cmd.params.volume_shift = input.substring(1).toInt();
                //         ESP_LOGI(SERIAL_TAG, "Setting volume shift to %d", cmd.params.volume_shift);
                //     }
                //     else
                //     {
                //         valid_command = false;
                //     }
                //     break;

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
                Serial.println("Unknown command. Type 'h' for help.");
                valid_command = false;
                break;
            }

            if (valid_command)
            {
                audio_player_send_command(cmd);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void serial_interface_init()
{
    ESP_LOGI(SERIAL_TAG, "Starting serial interface task...");

    BaseType_t result = xTaskCreate(
        serial_interface_task,
        "serial_interface",
        4096, // Stack size
        NULL, // Parameters
        2,    // Priority
        NULL  // Task handle
    );

    if (result != pdPASS)
    {
        ESP_LOGE(SERIAL_TAG, "Failed to create serial interface task");
    }
}
