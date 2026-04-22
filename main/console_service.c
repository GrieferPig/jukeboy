#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"

// #include "esp_flash_dispatcher.h"
#include "bluetooth_service.h"
#include "wifi_service.h"
#include "console_service.h"
#include "cartridge_service.h"
#include "audio_output_switch.h"
#include "player_service.h"
#include "power_mgmt_service.h"
#include "script_service.h"

static const char *TAG = "console_svc";

#define TELEMETRY_INTERVAL_MS 5000
#define TELEMETRY_TASK_STACK_SIZE 2048
#define TELEMETRY_MAX_TASKS 32
#define CONSOLE_REPL_TASK_STACK_SIZE (8 * 1024)

static void script_print_output(const char *output);
static const char *auth_mode_str(wifi_auth_mode_t mode);

typedef struct
{
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime_counter;
    char name[configMAX_TASK_NAME_LEN];
} telemetry_task_snapshot_t;

static volatile bool s_memory_telemetry_enabled = false;
static UBaseType_t s_prev_snapshot_count;
static configRUN_TIME_COUNTER_TYPE s_prev_total_runtime;
static uint8_t s_snapshot_idx;
static StaticSemaphore_t s_telemetry_mutex_storage;
static SemaphoreHandle_t s_telemetry_mutex;
static esp_event_handler_instance_t s_wifi_scan_done_handler;

EXT_RAM_BSS_ATTR static TaskStatus_t s_task_state_buf[TELEMETRY_MAX_TASKS];
EXT_RAM_BSS_ATTR static telemetry_task_snapshot_t s_snapshot_bufs[2][TELEMETRY_MAX_TASKS];

static StaticTask_t s_telemetry_task_tcb;
EXT_RAM_BSS_ATTR static StackType_t s_telemetry_task_stack[TELEMETRY_TASK_STACK_SIZE];

static telemetry_task_snapshot_t *find_previous_snapshot(TaskHandle_t handle)
{
    for (UBaseType_t index = 0; index < s_prev_snapshot_count; index++)
    {
        if (s_snapshot_bufs[s_snapshot_idx][index].handle == handle)
        {
            return &s_snapshot_bufs[s_snapshot_idx][index];
        }
    }
    return NULL;
}

static void capture_runtime_snapshot(void)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;

    if (s_telemetry_mutex)
    {
        xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    }

    if (task_count > TELEMETRY_MAX_TASKS)
    {
        task_count = TELEMETRY_MAX_TASKS;
    }

    uint8_t next_idx = s_snapshot_idx ^ 1;
    task_count = uxTaskGetSystemState(s_task_state_buf, task_count, &total_runtime);
    for (UBaseType_t index = 0; index < task_count; index++)
    {
        s_snapshot_bufs[next_idx][index].handle = s_task_state_buf[index].xHandle;
        s_snapshot_bufs[next_idx][index].runtime_counter = s_task_state_buf[index].ulRunTimeCounter;
        strncpy(s_snapshot_bufs[next_idx][index].name, s_task_state_buf[index].pcTaskName,
                sizeof(s_snapshot_bufs[next_idx][index].name) - 1);
        s_snapshot_bufs[next_idx][index].name[sizeof(s_snapshot_bufs[next_idx][index].name) - 1] = '\0';
    }

    s_snapshot_idx = next_idx;
    s_prev_snapshot_count = task_count;
    s_prev_total_runtime = total_runtime;

    if (s_telemetry_mutex)
    {
        xSemaphoreGive(s_telemetry_mutex);
    }
}

static void print_memory_stats(void)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t max_block_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t max_block_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;

    if (s_telemetry_mutex)
    {
        xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    }

    if (task_count > TELEMETRY_MAX_TASKS)
    {
        task_count = TELEMETRY_MAX_TASKS;
    }

    ESP_LOGI(TAG, "--- MEMORY TELEMETRY ---");
    ESP_LOGI(TAG, "Internal DRAM: Free: %u Bytes / Total: %u Bytes (%.1f%%)",
             (unsigned int)free_internal,
             (unsigned int)total_internal,
             total_internal ? (float)free_internal / total_internal * 100.0f : 0.0f);
    ESP_LOGI(TAG, "Internal DRAM Largest Free Block: %u Bytes", (unsigned int)max_block_internal);
    ESP_LOGI(TAG, "External PSRAM: Free: %u Bytes / Total: %u Bytes (%.1f%%)",
             (unsigned int)free_psram,
             (unsigned int)total_psram,
             total_psram ? (float)free_psram / total_psram * 100.0f : 0.0f);
    ESP_LOGI(TAG, "External PSRAM Largest Free Block: %u Bytes", (unsigned int)max_block_psram);
    ESP_LOGI(TAG, "------------------------");

    task_count = uxTaskGetSystemState(s_task_state_buf, task_count, &total_runtime);
    ESP_LOGI(TAG, "--- CPU TELEMETRY (INTERVAL) ---");
    ESP_LOGI(TAG, "Task Name\tDelta Time\t%% Interval");
    ESP_LOGI(TAG, "---------------------------------");

    if (s_prev_snapshot_count == 0 || total_runtime <= s_prev_total_runtime)
    {
        ESP_LOGI(TAG, "Baseline captured. Interval telemetry starts on next print.");
    }
    else
    {
        configRUN_TIME_COUNTER_TYPE runtime_delta = total_runtime - s_prev_total_runtime;
        for (UBaseType_t index = 0; index < task_count; index++)
        {
            telemetry_task_snapshot_t *previous = find_previous_snapshot(s_task_state_buf[index].xHandle);
            configRUN_TIME_COUNTER_TYPE task_delta = 0;
            if (previous && s_task_state_buf[index].ulRunTimeCounter >= previous->runtime_counter)
            {
                task_delta = s_task_state_buf[index].ulRunTimeCounter - previous->runtime_counter;
            }
            if (task_delta == 0)
            {
                continue;
            }

            float pct = runtime_delta ? ((float)task_delta * 100.0f) / (float)runtime_delta : 0.0f;
            printf("%s\t%lu\t%.1f%%\n",
                   s_task_state_buf[index].pcTaskName,
                   (unsigned long)task_delta,
                   pct);
        }
    }

    ESP_LOGI(TAG, "---------------------------------");

    uint8_t next_idx = s_snapshot_idx ^ 1;
    for (UBaseType_t index = 0; index < task_count; index++)
    {
        s_snapshot_bufs[next_idx][index].handle = s_task_state_buf[index].xHandle;
        s_snapshot_bufs[next_idx][index].runtime_counter = s_task_state_buf[index].ulRunTimeCounter;
        strncpy(s_snapshot_bufs[next_idx][index].name, s_task_state_buf[index].pcTaskName,
                sizeof(s_snapshot_bufs[next_idx][index].name) - 1);
        s_snapshot_bufs[next_idx][index].name[sizeof(s_snapshot_bufs[next_idx][index].name) - 1] = '\0';
    }

    s_snapshot_idx = next_idx;
    s_prev_snapshot_count = task_count;
    s_prev_total_runtime = total_runtime;

    if (s_telemetry_mutex)
    {
        xSemaphoreGive(s_telemetry_mutex);
    }
}

static void console_print_wifi_scan_results(const wifi_svc_scan_result_t *result)
{
    if (!result || result->count == 0)
    {
        printf("No access points found.\n");
        return;
    }

    printf("\n%-4s %-32s %6s %4s %-8s\n", "#", "SSID", "RSSI", "CH", "AUTH");
    printf("---- -------------------------------- ------ ---- --------\n");
    for (uint16_t index = 0; index < result->count; ++index)
    {
        printf("%-4u %-32s %4d   %2d   %-8s\n",
               (unsigned)(index + 1),
               (char *)result->records[index].ssid,
               result->records[index].rssi,
               result->records[index].primary,
               auth_mode_str(result->records[index].authmode));
    }
    printf("\n");
}

static void console_wifi_event_handler(void *handler_arg,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)handler_arg;
    (void)event_base;

    if (event_id != WIFI_SVC_EVENT_SCAN_DONE)
    {
        return;
    }

    printf("WiFi scan complete.\n");
    console_print_wifi_scan_results((const wifi_svc_scan_result_t *)event_data);
}

static void telemetry_task(void *param)
{
    capture_runtime_snapshot();

    for (;;)
    {
        if (s_memory_telemetry_enabled)
        {
            print_memory_stats();
        }
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

/* ════════════════════════════════════════════════════════════════════ *
 *  Bluetooth commands                                                 *
 * ════════════════════════════════════════════════════════════════════ */

static const char *bt_media_key_name(uint8_t key_code)
{
    switch (key_code)
    {
    case ESP_AVRC_PT_CMD_PLAY:
        return "play";
    case ESP_AVRC_PT_CMD_STOP:
        return "stop";
    case ESP_AVRC_PT_CMD_PAUSE:
        return "pause";
    case ESP_AVRC_PT_CMD_FORWARD:
        return "next";
    case ESP_AVRC_PT_CMD_BACKWARD:
        return "prev";
    case ESP_AVRC_PT_CMD_FAST_FORWARD:
        return "ff";
    case ESP_AVRC_PT_CMD_REWIND:
        return "rewind";
    case ESP_AVRC_PT_CMD_VOL_UP:
        return "vol_up";
    case ESP_AVRC_PT_CMD_VOL_DOWN:
        return "vol_down";
    case ESP_AVRC_PT_CMD_MUTE:
        return "mute";
    default:
        return "custom";
    }
}

static bool bt_parse_media_key(const char *value, uint8_t *key_code)
{
    char *end = NULL;
    unsigned long numeric_value;

    if (!value || !key_code)
    {
        return false;
    }

    if (strcmp(value, "play") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_PLAY;
    }
    else if (strcmp(value, "pause") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_PAUSE;
    }
    else if (strcmp(value, "stop") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_STOP;
    }
    else if (strcmp(value, "next") == 0 || strcmp(value, "forward") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_FORWARD;
    }
    else if (strcmp(value, "prev") == 0 || strcmp(value, "previous") == 0 || strcmp(value, "back") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_BACKWARD;
    }
    else if (strcmp(value, "ff") == 0 || strcmp(value, "fast_forward") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_FAST_FORWARD;
    }
    else if (strcmp(value, "rewind") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_REWIND;
    }
    else if (strcmp(value, "vol_up") == 0 || strcmp(value, "volume_up") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_VOL_UP;
    }
    else if (strcmp(value, "vol_down") == 0 || strcmp(value, "volume_down") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_VOL_DOWN;
    }
    else if (strcmp(value, "mute") == 0)
    {
        *key_code = ESP_AVRC_PT_CMD_MUTE;
    }
    else
    {
        numeric_value = strtoul(value, &end, 0);
        if (!end || *end != '\0' || numeric_value > UINT8_MAX)
        {
            return false;
        }
        *key_code = (uint8_t)numeric_value;
    }

    return true;
}

static void bt_print_bda(const esp_bd_addr_t bda)
{
    printf(ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
}

static int bt_pair_handler(int argc, char **argv)
{
    esp_err_t err = bluetooth_service_pair_best_a2dp_sink();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Starting Bluetooth discovery and best-sink pairing...\n");
    return 0;
}

static int bt_connect_handler(int argc, char **argv)
{
    esp_err_t err = bluetooth_service_connect_last_bonded_a2dp_device();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Connect request sent for the last bonded A2DP device.\n");
    return 0;
}

static int bt_disconnect_handler(int argc, char **argv)
{
    esp_err_t err = bluetooth_service_disconnect_a2dp();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Disconnect request sent.\n");
    return 0;
}

static int bt_confirm_handler(int argc, char **argv)
{
    bluetooth_service_pairing_confirm_t confirm = {0};
    const char *action = argc >= 2 ? argv[1] : NULL;

    esp_err_t err = bluetooth_service_get_pending_pairing_confirm(&confirm);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (!confirm.pending)
    {
        printf("No Bluetooth pairing confirmation is pending.\n");
        return 1;
    }

    if (!action)
    {
        printf("Pending pairing confirmation for ");
        bt_print_bda(confirm.remote_bda);
        printf(" passkey=%06lu\n", (unsigned long)confirm.numeric_value);
        printf("Usage: bt confirm <accept|reject>\n");
        return 0;
    }

    if (strcmp(action, "accept") != 0 && strcmp(action, "reject") != 0)
    {
        printf("Usage: bt confirm <accept|reject>\n");
        return 1;
    }

    err = bluetooth_service_reply_pairing_confirm(strcmp(action, "accept") == 0);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Bluetooth pairing confirmation %s for ", action);
    bt_print_bda(confirm.remote_bda);
    printf(" passkey=%06lu\n", (unsigned long)confirm.numeric_value);
    return 0;
}

static struct
{
    struct arg_str *action;
    struct arg_end *end;
} s_bt_audio_args;

static int bt_audio_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_bt_audio_args);
    esp_err_t err;

    if (nerrors)
    {
        arg_print_errors(stderr, s_bt_audio_args.end, argv[0]);
        return 1;
    }

    if (s_bt_audio_args.action->count == 0)
    {
        printf("Usage: bt audio <start|suspend>\n");
        return 1;
    }

    if (strcmp(s_bt_audio_args.action->sval[0], "start") == 0)
    {
        err = bluetooth_service_start_audio();
    }
    else if (strcmp(s_bt_audio_args.action->sval[0], "suspend") == 0)
    {
        err = bluetooth_service_suspend_audio();
    }
    else
    {
        printf("Usage: bt audio <start|suspend>\n");
        return 1;
    }

    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Audio command sent: %s\n", s_bt_audio_args.action->sval[0]);
    return 0;
}

static struct
{
    struct arg_str *key;
    struct arg_end *end;
} s_bt_media_key_args;

static int bt_media_key_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_bt_media_key_args);
    uint8_t key_code;
    esp_err_t err;

    if (nerrors)
    {
        arg_print_errors(stderr, s_bt_media_key_args.end, argv[0]);
        return 1;
    }

    if (!bt_parse_media_key(s_bt_media_key_args.key->sval[0], &key_code))
    {
        printf("Unknown media key. Use play, pause, stop, next, prev, ff, rewind, vol_up, vol_down, mute, or a numeric key code.\n");
        return 1;
    }

    err = bluetooth_service_send_media_key(key_code);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Sent media key %s (%u).\n", bt_media_key_name(key_code), (unsigned int)key_code);
    return 0;
}

static int bt_bonded_handler(int argc, char **argv)
{
    size_t count = bluetooth_service_get_bonded_device_count();
    esp_bd_addr_t *devices = NULL;
    esp_err_t err;

    printf("Bonded device count: %u\n", (unsigned int)count);
    if (count == 0)
    {
        return 0;
    }

    devices = calloc(count, sizeof(*devices));
    if (!devices)
    {
        printf("Error: out of memory\n");
        return 1;
    }

    err = bluetooth_service_get_bonded_devices(&count, devices);
    if (err != ESP_OK)
    {
        free(devices);
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    for (size_t index = 0; index < count; index++)
    {
        printf("%u. ", (unsigned int)(index + 1));
        bt_print_bda(devices[index]);
        printf("\n");
    }

    free(devices);
    return 0;
}

static const char *media_control_name(player_service_control_t control)
{
    switch (control)
    {
    case PLAYER_SVC_CONTROL_NEXT:
        return "next";
    case PLAYER_SVC_CONTROL_PREVIOUS:
        return "prev";
    case PLAYER_SVC_CONTROL_FAST_FORWARD:
        return "ff";
    case PLAYER_SVC_CONTROL_FAST_BACKWARD:
        return "rewind";
    case PLAYER_SVC_CONTROL_VOLUME_UP:
        return "vol_up";
    case PLAYER_SVC_CONTROL_VOLUME_DOWN:
        return "vol_down";
    case PLAYER_SVC_CONTROL_PAUSE:
        return "pause";
    default:
        return "unknown";
    }
}

static bool media_parse_control(const char *value, player_service_control_t *control)
{
    if (!value || !control)
    {
        return false;
    }

    if (strcmp(value, "next") == 0)
    {
        *control = PLAYER_SVC_CONTROL_NEXT;
    }
    else if (strcmp(value, "prev") == 0 || strcmp(value, "previous") == 0)
    {
        *control = PLAYER_SVC_CONTROL_PREVIOUS;
    }
    else if (strcmp(value, "ff") == 0 || strcmp(value, "fast_forward") == 0)
    {
        *control = PLAYER_SVC_CONTROL_FAST_FORWARD;
    }
    else if (strcmp(value, "rewind") == 0 || strcmp(value, "backward") == 0)
    {
        *control = PLAYER_SVC_CONTROL_FAST_BACKWARD;
    }
    else if (strcmp(value, "vol_up") == 0 || strcmp(value, "volume_up") == 0)
    {
        *control = PLAYER_SVC_CONTROL_VOLUME_UP;
    }
    else if (strcmp(value, "vol_down") == 0 || strcmp(value, "volume_down") == 0)
    {
        *control = PLAYER_SVC_CONTROL_VOLUME_DOWN;
    }
    else if (strcmp(value, "pause") == 0 || strcmp(value, "play_pause") == 0 || strcmp(value, "toggle") == 0)
    {
        *control = PLAYER_SVC_CONTROL_PAUSE;
    }
    else
    {
        return false;
    }

    return true;
}

static struct
{
    struct arg_str *action;
    struct arg_str *value;
    struct arg_end *end;
} s_media_args;

static const char *playback_mode_name(player_service_playback_mode_t mode)
{
    switch (mode)
    {
    case PLAYER_SVC_MODE_SEQUENTIAL:
        return "sequential";
    case PLAYER_SVC_MODE_SINGLE_REPEAT:
        return "single_repeat";
    case PLAYER_SVC_MODE_SHUFFLE:
        return "shuffle";
    default:
        return "unknown";
    }
}

static bool media_parse_mode(const char *value, player_service_playback_mode_t *mode)
{
    if (!value || !mode)
    {
        return false;
    }
    if (strcmp(value, "sequential") == 0)
    {
        *mode = PLAYER_SVC_MODE_SEQUENTIAL;
    }
    else if (strcmp(value, "single_repeat") == 0 || strcmp(value, "repeat") == 0)
    {
        *mode = PLAYER_SVC_MODE_SINGLE_REPEAT;
    }
    else if (strcmp(value, "shuffle") == 0 || strcmp(value, "random") == 0)
    {
        *mode = PLAYER_SVC_MODE_SHUFFLE;
    }
    else
    {
        return false;
    }
    return true;
}

static int cmd_media(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_media_args);
    player_service_control_t control;
    esp_err_t err;

    if (nerrors)
    {
        arg_print_errors(stderr, s_media_args.end, argv[0]);
        return 1;
    }

    if (s_media_args.action->count == 0 || strcmp(s_media_args.action->sval[0], "status") == 0)
    {
        printf("Playback: %s\n", player_service_is_paused() ? "paused" : "running");
        printf("Volume:   %u%%\n", (unsigned)player_service_get_volume_percent());
        printf("Mode:     %s\n", playback_mode_name(player_service_get_playback_mode()));
        return 0;
    }

    if (strcmp(s_media_args.action->sval[0], "mode") == 0)
    {
        if (s_media_args.value->count == 0)
        {
            printf("Mode: %s\n", playback_mode_name(player_service_get_playback_mode()));
            return 0;
        }
        player_service_playback_mode_t mode;
        if (!media_parse_mode(s_media_args.value->sval[0], &mode))
        {
            printf("Unknown mode '%s'. Use: sequential, single_repeat, shuffle\n",
                   s_media_args.value->sval[0]);
            return 1;
        }
        err = player_service_set_playback_mode(mode);
        if (err != ESP_OK)
        {
            printf("Error: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Playback mode set to: %s\n", playback_mode_name(mode));
        return 0;
    }

    if (!media_parse_control(s_media_args.action->sval[0], &control))
    {
        printf("Usage: media [status|next|prev|pause|ff|rewind|vol_up|vol_down|mode [sequential|single_repeat|shuffle]]\n");
        return 1;
    }

    err = player_service_request_control(control);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Media control queued: %s\n", media_control_name(control));
    return 0;
}

static int cmd_bt(int argc, char **argv)
{
    static const char *usage =
        "Usage: bt <subcommand> [args]\n"
        "  pair                     Discover and pair the best A2DP sink\n"
        "  connect                  Reconnect to last bonded A2DP device\n"
        "  confirm <accept|reject>  Reply to pending SSP pairing confirmation\n"
        "  disconnect               Disconnect current A2DP link\n"
        "  audio <start|suspend>    Control A2DP audio streaming\n"
        "  media_key <key>          Send AVRCP media key (play/pause/stop/next/prev/...)\n"
        "  bonded                   List bonded device addresses\n";

    if (argc < 2)
    {
        printf("%s", usage);
        return 1;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "pair") == 0)
        return bt_pair_handler(argc - 1, argv + 1);
    if (strcmp(sub, "connect") == 0)
        return bt_connect_handler(argc - 1, argv + 1);
    if (strcmp(sub, "confirm") == 0)
        return bt_confirm_handler(argc - 1, argv + 1);
    if (strcmp(sub, "disconnect") == 0)
        return bt_disconnect_handler(argc - 1, argv + 1);
    if (strcmp(sub, "audio") == 0)
        return bt_audio_handler(argc - 1, argv + 1);
    if (strcmp(sub, "media_key") == 0)
        return bt_media_key_handler(argc - 1, argv + 1);
    if (strcmp(sub, "bonded") == 0)
        return bt_bonded_handler(argc - 1, argv + 1);

    printf("Unknown subcommand '%s'.\n%s", sub, usage);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  WiFi commands                                                      *
 * ════════════════════════════════════════════════════════════════════ */

static struct
{
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} s_connect_args;

static int wifi_connect_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_connect_args);
    if (nerrors)
    {
        arg_print_errors(stderr, s_connect_args.end, argv[0]);
        return 1;
    }
    const char *ssid = s_connect_args.ssid->sval[0];
    const char *pass = s_connect_args.password->count > 0
                           ? s_connect_args.password->sval[0]
                           : "";

    printf("Connecting to '%s'...\n", ssid);
    esp_err_t err = wifi_service_connect(ssid, pass);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int wifi_disconnect_handler(int argc, char **argv)
{
    printf("Disconnecting...\n");
    esp_err_t err = wifi_service_disconnect();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "?";
    }
}

static int wifi_scan_handler(int argc, char **argv)
{
    if (wifi_service_get_state() == WIFI_SVC_STATE_SCANNING)
    {
        printf("WiFi scan already in progress. Results will print when ready.\n");
        return 0;
    }

    esp_err_t err = wifi_service_scan();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Scan started. Results will print asynchronously when ready.\n");
    return 0;
}

static const char *state_str(wifi_svc_state_t st)
{
    switch (st)
    {
    case WIFI_SVC_STATE_IDLE:
        return "IDLE";
    case WIFI_SVC_STATE_SCANNING:
        return "SCANNING";
    case WIFI_SVC_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_SVC_STATE_CONNECTED:
        return "CONNECTED";
    case WIFI_SVC_STATE_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

static int wifi_status_handler(int argc, char **argv)
{
    wifi_svc_state_t st = wifi_service_get_state();
    printf("WiFi state: %s\n", state_str(st));
    printf("Auto-reconnect: %s\n",
           wifi_service_get_autoreconnect() ? "ON" : "OFF");

    if (st == WIFI_SVC_STATE_CONNECTED)
    {
        esp_netif_ip_info_t ip;
        if (wifi_service_get_ip_info(&ip) == ESP_OK)
        {
            printf("IP:      " IPSTR "\n", IP2STR(&ip.ip));
            printf("Netmask: " IPSTR "\n", IP2STR(&ip.netmask));
            printf("Gateway: " IPSTR "\n", IP2STR(&ip.gw));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            printf("SSID:    %s\n", (char *)ap.ssid);
            printf("RSSI:    %d dBm\n", ap.rssi);
            printf("Channel: %d\n", ap.primary);
        }
    }
    return 0;
}

static struct
{
    struct arg_str *action;
    struct arg_end *end;
} s_autoreconnect_args;

static int wifi_reconnect_handler(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_service_reconnect();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Reconnect requested.\n");
    return 0;
}

static int wifi_autoreconnect_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_autoreconnect_args);
    if (nerrors)
    {
        arg_print_errors(stderr, s_autoreconnect_args.end, argv[0]);
        return 1;
    }

    if (s_autoreconnect_args.action->count == 0)
    {
        /* No argument: print current status */
        printf("Auto-reconnect: %s\n",
               wifi_service_get_autoreconnect() ? "ON" : "OFF");
        return 0;
    }

    const char *val = s_autoreconnect_args.action->sval[0];
    bool enable;
    if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0)
    {
        enable = true;
    }
    else if (strcmp(val, "off") == 0 || strcmp(val, "0") == 0)
    {
        enable = false;
    }
    else
    {
        printf("Usage: wifi autoreconnect [on|off]\n");
        return 1;
    }

    esp_err_t err = wifi_service_set_autoreconnect(enable);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Auto-reconnect set to %s\n", enable ? "ON" : "OFF");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    static const char *usage =
        "Usage: wifi <subcommand> [args]\n"
        "  connect <ssid> [password]   Connect to a WiFi access point\n"
        "  disconnect                  Disconnect from the current network\n"
        "  reconnect                   Immediately reconnect using saved credentials\n"
        "  scan                        Scan for nearby access points\n"
        "  status                      Show WiFi state, IP info, and AP details\n"
        "  autoreconnect [on|off]      Get or set 30 s periodic auto-reconnect\n";

    if (argc < 2)
    {
        printf("%s", usage);
        return 1;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "connect") == 0)
        return wifi_connect_handler(argc - 1, argv + 1);
    if (strcmp(sub, "disconnect") == 0)
        return wifi_disconnect_handler(argc - 1, argv + 1);
    if (strcmp(sub, "reconnect") == 0)
        return wifi_reconnect_handler(argc - 1, argv + 1);
    if (strcmp(sub, "scan") == 0)
        return wifi_scan_handler(argc - 1, argv + 1);
    if (strcmp(sub, "status") == 0)
        return wifi_status_handler(argc - 1, argv + 1);
    if (strcmp(sub, "autoreconnect") == 0)
        return wifi_autoreconnect_handler(argc - 1, argv + 1);

    printf("Unknown subcommand '%s'.\n%s", sub, usage);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  System commands                                                    *
 * ════════════════════════════════════════════════════════════════════ */

/* ── reboot ─────────────────────────────────────────────────────────── */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Unmount SD card if mounted to ensure clean shutdown
    cartridge_service_unmount();
    printf("Rebooting...\n");

    esp_err_t err = power_mgmt_service_reboot();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static void register_reboot(void)
{
    const esp_console_cmd_t cmd = {
        .command = "reboot",
        .help = "Reboot the device",
        .hint = NULL,
        .func = cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── rdl ─────────────────────────────────────────────────────────────── */

static int cmd_rdl(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cartridge_service_unmount();
    printf("Rebooting into download mode...\n");

    esp_err_t err = power_mgmt_service_reboot_to_download();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

static void register_rdl(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rdl",
        .help = "Reboot into esptool download mode",
        .hint = NULL,
        .func = cmd_rdl,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── meminfo ────────────────────────────────────────────────────────── */

static int cmd_meminfo(int argc, char **argv)
{
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t tot_int = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t max_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    size_t free_ext = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t tot_ext = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t max_ext = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    printf("Internal DRAM:  Free %u / Total %u (%.1f%%)  Largest block: %u\n",
           (unsigned)free_int, (unsigned)tot_int,
           tot_int ? (float)free_int / tot_int * 100 : 0,
           (unsigned)max_int);
    printf("External PSRAM: Free %u / Total %u (%.1f%%)  Largest block: %u\n",
           (unsigned)free_ext, (unsigned)tot_ext,
           tot_ext ? (float)free_ext / tot_ext * 100 : 0,
           (unsigned)max_ext);
    return 0;
}

static void register_meminfo(void)
{
    const esp_console_cmd_t cmd = {
        .command = "meminfo",
        .help = "Print internal DRAM and external PSRAM heap statistics",
        .hint = NULL,
        .func = cmd_meminfo,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── telemetry ──────────────────────────────────────────────────────── */

static struct
{
    struct arg_str *action;
    struct arg_end *end;
} s_telemetry_args;

static int cmd_telemetry(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_telemetry_args);
    if (nerrors)
    {
        arg_print_errors(stderr, s_telemetry_args.end, argv[0]);
        return 1;
    }

    if (s_telemetry_args.action->count == 0)
    {
        printf("Memory telemetry: %s\n", s_memory_telemetry_enabled ? "ON" : "OFF");
        return 0;
    }

    const char *value = s_telemetry_args.action->sval[0];
    if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0)
    {
        s_memory_telemetry_enabled = true;
        capture_runtime_snapshot();
    }
    else if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0)
    {
        s_memory_telemetry_enabled = false;
    }
    else
    {
        printf("Usage: telemetry [on|off]\n");
        return 1;
    }

    printf("Memory telemetry %s\n", s_memory_telemetry_enabled ? "ON" : "OFF");
    return 0;
}

static void register_telemetry(void)
{
    s_telemetry_args.action = arg_str0(NULL, NULL, "<on|off>", "Enable or disable periodic memory/CPU telemetry");
    s_telemetry_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "telemetry",
        .help = "Get or set periodic memory and interval CPU telemetry",
        .hint = NULL,
        .func = cmd_telemetry,
        .argtable = &s_telemetry_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ════════════════════════════════════════════════════════════════════ *
 *  SD card commands                                                   *
 * ════════════════════════════════════════════════════════════════════ */

static int sd_mount_handler(int argc, char **argv)
{
    esp_err_t err = cartridge_service_mount();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("SD card mounted.\n");
    return 0;
}

static bool sd_parse_index(const char *value, size_t *index)
{
    char *end = NULL;
    unsigned long parsed;

    if (!value || !index)
    {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || !end || *end != '\0')
    {
        return false;
    }

    *index = (size_t)parsed;
    return true;
}

static void sd_print_metadata_string(const char *value)
{
    printf("%s\n", value ? value : "");
}

static int sd_unmount_handler(int argc, char **argv)
{
    esp_err_t err = cartridge_service_unmount();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("SD card unmounted.\n");
    return 0;
}

static int sd_status_handler(int argc, char **argv)
{
    printf("Inserted: %s\n", cartridge_service_is_inserted() ? "yes" : "no");
    printf("Mounted:  %s\n", cartridge_service_is_mounted() ? "yes" : "no");
    printf("Status:   %s\n", cartridge_service_status_name(cartridge_service_get_status()));
    printf("Tracks:   %u\n", (unsigned)cartridge_service_get_metadata_track_count());
    return 0;
}

static int sd_meta_handler(int argc, char **argv)
{
    static const char *usage =
        "Usage: sd meta <field> [index]\n"
        "  fields:\n"
        "    version\n"
        "    checksum\n"
        "    album_name\n"
        "    album_description\n"
        "    artist\n"
        "    year\n"
        "    duration_sec\n"
        "    genre\n"
        "    tag <0-4>\n"
        "    track_count\n"
        "    track_name <track_index>\n"
        "    track_artists <track_index>\n"
        "    track_duration_sec <track_index>\n"
        "    file_num <track_index>\n";
    const char *field;
    const char *string_value;
    size_t index = 0;

    if (argc < 2)
    {
        printf("%s", usage);
        return 1;
    }

    if (cartridge_service_get_status() != CARTRIDGE_STATUS_READY)
    {
        printf("Metadata unavailable. Cartridge status: %s\n",
               cartridge_service_status_name(cartridge_service_get_status()));
        return 1;
    }

    field = argv[1];
    if (strcmp(field, "version") == 0)
    {
        printf("%lu\n", (unsigned long)cartridge_service_get_metadata_version());
        return 0;
    }
    if (strcmp(field, "checksum") == 0)
    {
        printf("0x%08lx\n", (unsigned long)cartridge_service_get_metadata_checksum());
        return 0;
    }
    if (strcmp(field, "album_name") == 0)
    {
        sd_print_metadata_string(cartridge_service_get_album_name());
        return 0;
    }
    if (strcmp(field, "album_description") == 0)
    {
        sd_print_metadata_string(cartridge_service_get_album_description());
        return 0;
    }
    if (strcmp(field, "artist") == 0)
    {
        sd_print_metadata_string(cartridge_service_get_album_artist());
        return 0;
    }
    if (strcmp(field, "year") == 0)
    {
        printf("%lu\n", (unsigned long)cartridge_service_get_album_year());
        return 0;
    }
    if (strcmp(field, "duration_sec") == 0)
    {
        printf("%lu\n", (unsigned long)cartridge_service_get_album_duration_sec());
        return 0;
    }
    if (strcmp(field, "genre") == 0)
    {
        sd_print_metadata_string(cartridge_service_get_album_genre());
        return 0;
    }
    if (strcmp(field, "track_count") == 0)
    {
        printf("%u\n", (unsigned)cartridge_service_get_metadata_track_count());
        return 0;
    }

    if (argc < 3 || !sd_parse_index(argv[2], &index))
    {
        printf("%s", usage);
        return 1;
    }

    if (strcmp(field, "tag") == 0)
    {
        string_value = cartridge_service_get_album_tag(index);
        if (!string_value)
        {
            printf("Invalid tag index\n");
            return 1;
        }
        sd_print_metadata_string(string_value);
        return 0;
    }

    if (strcmp(field, "track_name") == 0)
    {
        string_value = cartridge_service_get_track_name(index);
        if (!string_value)
        {
            printf("Invalid track index\n");
            return 1;
        }
        sd_print_metadata_string(string_value);
        return 0;
    }

    if (strcmp(field, "track_artists") == 0)
    {
        string_value = cartridge_service_get_track_artists(index);
        if (!string_value)
        {
            printf("Invalid track index\n");
            return 1;
        }
        sd_print_metadata_string(string_value);
        return 0;
    }

    if (strcmp(field, "track_duration_sec") == 0)
    {
        if (!cartridge_service_get_metadata_track(index))
        {
            printf("Invalid track index\n");
            return 1;
        }
        printf("%lu\n", (unsigned long)cartridge_service_get_track_duration_sec(index));
        return 0;
    }

    if (strcmp(field, "file_num") == 0)
    {
        if (!cartridge_service_get_metadata_track(index))
        {
            printf("Invalid track index\n");
            return 1;
        }
        printf("%lu\n", (unsigned long)cartridge_service_get_track_file_num(index));
        return 0;
    }

    printf("%s", usage);
    return 1;
}

static int cmd_sd(int argc, char **argv)
{
    static const char *usage =
        "Usage: sd <subcommand>\n"
        "  mount     Mount the SD card FAT filesystem\n"
        "  unmount   Unmount the SD card\n"
        "  status    Show insertion, mount, and metadata state\n"
        "  meta      Inspect album.jbm fields\n";

    if (argc < 2)
    {
        printf("%s", usage);
        return 1;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "mount") == 0)
        return sd_mount_handler(argc - 1, argv + 1);
    if (strcmp(sub, "unmount") == 0)
        return sd_unmount_handler(argc - 1, argv + 1);
    if (strcmp(sub, "status") == 0)
        return sd_status_handler(argc - 1, argv + 1);
    if (strcmp(sub, "meta") == 0)
        return sd_meta_handler(argc - 1, argv + 1);

    printf("Unknown subcommand '%s'.\n%s", sub, usage);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  Script commands                                                    *
 * ════════════════════════════════════════════════════════════════════ */

static int script_list_directory(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;

    if (!dir)
    {
        printf("Cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    printf("%s\n", path);
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

#if defined(DT_DIR) && defined(DT_REG) && defined(DT_UNKNOWN)
        if (entry->d_type == DT_DIR)
        {
            printf("  [dir]  %s/\n", entry->d_name);
            continue;
        }

        if (entry->d_type == DT_REG)
        {
            printf("  [file] %s\n", entry->d_name);
            continue;
        }
#endif

        printf("  %s\n", entry->d_name);
    }

    closedir(dir);
    return 0;
}

static void script_print_status(void)
{
    script_service_status_snapshot_t snapshot = {0};
    int64_t now_ms = esp_timer_get_time() / 1000;

    script_service_get_status_snapshot(&snapshot);

    printf("Script service: %s\n", script_service_status_name(snapshot.status));
    printf("Host module:    %s\n", SCRIPT_SERVICE_HOST_MODULE_NAME);
    printf("Run mode:       .wasm/.cwasm => %s\n",
           script_service_run_mode_name(SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN));
    printf("Scripts root:   %s\n", script_service_get_root_path());
    printf("Lookup:         <name> -> %s/<name>/<name>.wasm|.cwasm\n",
           script_service_get_root_path());

    printf("Running:        %s\n", snapshot.has_active_run ? "yes" : "no");
    if (snapshot.has_active_run)
    {
        long elapsed_ms = 0;

        if (snapshot.active_run_started_ms > 0 && now_ms >= snapshot.active_run_started_ms)
        {
            int64_t elapsed_ms64 = now_ms - snapshot.active_run_started_ms;

            elapsed_ms = elapsed_ms64 > LONG_MAX ? LONG_MAX : (long)elapsed_ms64;
        }

        printf("Active run ID:  %u\n", (unsigned)snapshot.active_run.run_id);
        printf("Active script:  %s\n", snapshot.active_run.resolved_path);
        printf("Active mode:    %s\n", script_service_run_mode_name(snapshot.active_run.mode));
        printf("Active size:    %u bytes\n", (unsigned)snapshot.active_run.script_size_bytes);
        printf("Active for:     %ld ms\n", elapsed_ms);
        if (snapshot.active_run.message[0] != '\0')
        {
            printf("Active note:    %s\n", snapshot.active_run.message);
        }
    }

    if (snapshot.has_last_run)
    {
        long finished_ago_ms = 0;

        if (snapshot.last_run_finished_ms > 0 && now_ms >= snapshot.last_run_finished_ms)
        {
            int64_t finished_ago_ms64 = now_ms - snapshot.last_run_finished_ms;

            finished_ago_ms = finished_ago_ms64 > LONG_MAX ? LONG_MAX : (long)finished_ago_ms64;
        }

        printf("Last run ID:    %u\n", (unsigned)snapshot.last_run.run_id);
        printf("Last script:    %s\n", snapshot.last_run.resolved_path);
        printf("Last mode:      %s\n", script_service_run_mode_name(snapshot.last_run.mode));
        printf("Last size:      %u bytes\n", (unsigned)snapshot.last_run.script_size_bytes);
        printf("Last exit code: %ld\n", (long)snapshot.last_run.exit_code);
        printf("Last state:     %s\n", snapshot.last_run_err == ESP_OK ? "ok" : "error");
        if (snapshot.last_run.message[0] != '\0')
        {
            printf("Last message:   %s\n", snapshot.last_run.message);
        }
        printf("Last finished:  %ld ms ago\n", finished_ago_ms);
        script_print_output(snapshot.last_run.output);
    }
}

static void script_print_output(const char *output)
{
    size_t output_len;

    if (!output || output[0] == '\0')
    {
        return;
    }

    output_len = strlen(output);
    printf("Output:\n%s", output);
    if (output_len == 0 || output[output_len - 1] != '\n')
    {
        printf("\n");
    }
}

static int script_print_log(const char *name)
{
    char log_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    FILE *file;
    char buffer[256];
    size_t bytes_read;
    bool saw_output = false;
    char last_byte = '\0';
    esp_err_t err;

    err = script_service_get_log_path(name, log_path, sizeof(log_path));
    if (err != ESP_OK)
    {
        printf("Error: script name must be a bare filename under %s\n",
               script_service_get_root_path());
        return 1;
    }

    file = fopen(log_path, "rb");
    if (!file)
    {
        printf("No log available for %s (%s)\n", name, log_path);
        return 1;
    }

    printf("Log: %s\n", log_path);
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        saw_output = true;
        last_byte = buffer[bytes_read - 1];
        fwrite(buffer, 1, bytes_read, stdout);
    }

    if (ferror(file))
    {
        printf("\nError: failed to read %s\n", log_path);
        fclose(file);
        return 1;
    }

    fclose(file);
    if (!saw_output)
    {
        printf("<empty>\n");
    }
    else if (last_byte != '\n')
    {
        printf("\n");
    }

    return 0;
}

static int cmd_script(int argc, char **argv)
{
    static const char *usage =
        "Usage: script <subcommand> [args]\n"
        "  status                 Show runtime status and fixed scripts root\n"
        "  ls [name]              List /lfs/scripts or one script directory\n"
        "  log <name>             Print the ramdisk log for one script\n"
        "  run <name> [args...]   Queue a builtin .wasm or .cwasm script\n";

    if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
        script_print_status();
        return 0;
    }

    if (strcmp(argv[1], "ls") == 0)
    {
        if (argc < 3)
        {
            return script_list_directory(script_service_get_root_path());
        }

        char path[SCRIPT_SERVICE_MAX_PATH_LEN];
        esp_err_t err = script_service_get_script_directory(argv[2], path, sizeof(path));
        if (err != ESP_OK)
        {
            printf("Error: script name must be a bare filename under %s\n",
                   script_service_get_root_path());
            return 1;
        }
        return script_list_directory(path);
    }

    if (strcmp(argv[1], "run") == 0)
    {
        script_service_run_result_t *result = NULL;
        const char *const *script_argv = NULL;
        int script_argc = 0;
        esp_err_t err;

        if (argc < 3)
        {
            printf("Usage: script run <name> [args...]\n");
            return 1;
        }

        if (argc > 3)
        {
            script_argc = argc - 3;
            script_argv = (const char *const *)&argv[3];
        }

        result = calloc(1, sizeof(*result));
        if (!result)
        {
            printf("Error: out of memory\n");
            return 1;
        }

        err = script_service_start(argv[2], script_argc, script_argv, result);
        if (err != ESP_OK)
        {
            if (result->resolved_path[0] != '\0')
            {
                printf("Resolved:  %s\n", result->resolved_path);
                printf("Mode:      %s\n", script_service_run_mode_name(result->mode));
            }
            script_print_output(result->output);
            printf("Error: %s\n",
                   result->message[0] != '\0' ? result->message : esp_err_to_name(err));
            free(result);
            return 1;
        }

        printf("Run ID:    %u\n", (unsigned)result->run_id);
        printf("Resolved:  %s\n", result->resolved_path);
        printf("Mode:      %s\n", script_service_run_mode_name(result->mode));
        printf("Size:      %u bytes\n", (unsigned)result->script_size_bytes);
        printf("Status:    running in background; use 'script status' to monitor completion\n");
        free(result);
        return 0;
    }

    if (strcmp(argv[1], "log") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: script log <name>\n");
            return 1;
        }

        return script_print_log(argv[2]);
    }

    printf("Unknown subcommand '%s'.\n%s", argv[1], usage);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  Audio output commands                                              *
 * ════════════════════════════════════════════════════════════════════ */

static struct
{
    struct arg_str *target;
    struct arg_end *end;
} s_audio_output_args;

static int cmd_audio_output(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_audio_output_args);
    if (nerrors)
    {
        arg_print_errors(stderr, s_audio_output_args.end, argv[0]);
        return 1;
    }

    if (s_audio_output_args.target->count == 0 || strcmp(s_audio_output_args.target->sval[0], "status") == 0)
    {
        printf("Audio output: %s\n", audio_output_switch_target_name(audio_output_switch_get_target()));
        printf("A2DP connected: %s\n", bluetooth_service_is_a2dp_connected() ? "yes" : "no");
        return 0;
    }

    const char *target = s_audio_output_args.target->sval[0];
    audio_output_target_t next_target;
    if (strcmp(target, "i2s") == 0)
    {
        next_target = AUDIO_OUTPUT_TARGET_I2S;
    }
    else if (strcmp(target, "a2dp") == 0)
    {
        next_target = AUDIO_OUTPUT_TARGET_BLUETOOTH;
    }
    else
    {
        printf("Usage: audio_output [status|i2s|a2dp]\n");
        return 1;
    }

    esp_err_t err = audio_output_switch_select(next_target);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Audio output switched to %s\n", audio_output_switch_target_name(next_target));
    return 0;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  Console service init                                               *
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t console_service_init(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32> ";
    repl_config.task_stack_size = CONSOLE_REPL_TASK_STACK_SIZE;
    repl_config.max_cmdline_length = 256;

    if (!s_telemetry_mutex)
    {
        s_telemetry_mutex = xSemaphoreCreateMutexStatic(&s_telemetry_mutex_storage);
        if (!s_telemetry_mutex)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Register built-in help */
    esp_console_register_help_command();

    /* Initialise argtables */
    s_bt_audio_args.action = arg_str1(NULL, NULL, "<start|suspend>", "A2DP media control action");
    s_bt_audio_args.end = arg_end(1);
    s_bt_media_key_args.key = arg_str1(NULL, NULL, "<key>", "Media key name or AVRCP passthrough code");
    s_bt_media_key_args.end = arg_end(1);
    s_connect_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of the AP");
    s_connect_args.password = arg_str0(NULL, NULL, "<password>", "Password (optional for open networks)");
    s_connect_args.end = arg_end(2);
    s_autoreconnect_args.action = arg_str0(NULL, NULL, "<on|off>", "Enable or disable");
    s_autoreconnect_args.end = arg_end(1);
    s_audio_output_args.target = arg_str0(NULL, NULL, "<status|i2s|a2dp>", "Get status or switch audio output target");
    s_audio_output_args.end = arg_end(1);
    s_media_args.action = arg_str0(NULL, NULL, "<action>", "status|next|prev|pause|ff|rewind|vol_up|vol_down|mode");
    s_media_args.value = arg_str0(NULL, NULL, "<value>", "For 'mode': sequential|single_repeat|shuffle");
    s_media_args.end = arg_end(2);
    s_telemetry_args.action = arg_str0(NULL, NULL, "<on|off>", "Enable or disable periodic memory/CPU telemetry");
    s_telemetry_args.end = arg_end(1);

    /* Bluetooth command group */
    const esp_console_cmd_t bt_cmd = {
        .command = "bt",
        .help = "Bluetooth: bt <pair|connect|confirm|disconnect|audio|media_key|bonded> [args]",
        .hint = NULL,
        .func = cmd_bt,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&bt_cmd));

    /* WiFi command group */
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "WiFi: wifi <connect|disconnect|scan|status|autoreconnect> [args]",
        .hint = NULL,
        .func = cmd_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    /* SD card command group */
    const esp_console_cmd_t sd_cmd = {
        .command = "sd",
        .help = "SD card: sd <mount|unmount|status>",
        .hint = NULL,
        .func = cmd_sd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sd_cmd));

    const esp_console_cmd_t audio_output_cmd = {
        .command = "audio_output",
        .help = "Audio output: audio_output [status|i2s|a2dp]",
        .hint = NULL,
        .func = cmd_audio_output,
        .argtable = &s_audio_output_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&audio_output_cmd));

    const esp_console_cmd_t media_cmd = {
        .command = "media",
        .help = "Local media control: media [status|next|prev|pause|ff|rewind|vol_up|vol_down|mode [sequential|single_repeat|shuffle]]",
        .hint = NULL,
        .func = cmd_media,
        .argtable = &s_media_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&media_cmd));

    const esp_console_cmd_t script_cmd = {
        .command = "script",
        .help = "WASM scripts: script <status|roots|ls|resolve|run> [args]",
        .hint = NULL,
        .func = cmd_script,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&script_cmd));

    /* System commands */
    register_reboot();
    register_rdl();
    register_meminfo();
    register_telemetry();

    esp_err_t event_err = esp_event_handler_instance_register(WIFI_SERVICE_EVENT,
                                                              WIFI_SVC_EVENT_SCAN_DONE,
                                                              console_wifi_event_handler,
                                                              NULL,
                                                              &s_wifi_scan_done_handler);
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE)
    {
        return event_err;
    }

    if (!xTaskCreateStaticPinnedToCore(telemetry_task,
                                       "ConsoleTelemetry",
                                       TELEMETRY_TASK_STACK_SIZE,
                                       NULL,
                                       4,
                                       s_telemetry_task_stack,
                                       &s_telemetry_task_tcb,
                                       0))
    {
        return ESP_ERR_NO_MEM;
    }

    /* Start UART REPL */
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Console service started");
    return ESP_OK;
}
