#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"

#include "bluetooth_service.h"
#include "wifi_service.h"
#include "console_service.h"
#include "cartridge_service.h"

static const char *TAG = "console_svc";

#define TELEMETRY_INTERVAL_MS 5000
#define TELEMETRY_TASK_STACK_SIZE 2048
#define TELEMETRY_MAX_TASKS 32

typedef struct
{
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime_counter;
    char name[configMAX_TASK_NAME_LEN];
} telemetry_task_snapshot_t;

static volatile bool s_memory_telemetry_enabled = true;
static UBaseType_t s_prev_snapshot_count;
static configRUN_TIME_COUNTER_TYPE s_prev_total_runtime;
static uint8_t s_snapshot_idx;

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

static int cmd_bt(int argc, char **argv)
{
    static const char *usage =
        "Usage: bt <subcommand> [args]\n"
        "  pair                     Discover and pair the best A2DP sink\n"
        "  connect                  Reconnect to last bonded A2DP device\n"
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
    printf("Scanning...\n");
    esp_err_t err = wifi_service_scan();
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Poll until scan finishes (max 10 s) */
    for (int i = 0; i < 20; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (wifi_service_get_state() != WIFI_SVC_STATE_SCANNING)
            break;
    }

    wifi_svc_scan_result_t res;
    wifi_service_get_scan_results(&res);
    if (res.count == 0)
    {
        printf("No access points found.\n");
        return 0;
    }

    printf("\n%-4s %-32s %6s %4s %-8s\n", "#", "SSID", "RSSI", "CH", "AUTH");
    printf("---- -------------------------------- ------ ---- --------\n");
    for (int i = 0; i < res.count; i++)
    {
        printf("%-4d %-32s %4d   %2d   %-8s\n",
               i + 1,
               (char *)res.records[i].ssid,
               res.records[i].rssi,
               res.records[i].primary,
               auth_mode_str(res.records[i].authmode));
    }
    printf("\n");
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
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(100)); /* let UART flush */
    esp_restart();
    return 0; /* unreachable */
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
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    static const char *usage =
        "Usage: sd <subcommand>\n"
        "  mount     Mount the SD card FAT filesystem\n"
        "  unmount   Unmount the SD card\n"
        "  status    Show insertion and mount state\n";

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

    printf("Unknown subcommand '%s'.\n%s", sub, usage);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════ *
 *  Console service init                                               *
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t console_service_init(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32> ";
    repl_config.max_cmdline_length = 256;

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
    s_telemetry_args.action = arg_str0(NULL, NULL, "<on|off>", "Enable or disable periodic memory/CPU telemetry");
    s_telemetry_args.end = arg_end(1);

    /* Bluetooth command group */
    const esp_console_cmd_t bt_cmd = {
        .command = "bt",
        .help = "Bluetooth: bt <pair|connect|disconnect|audio|media_key|bonded> [args]",
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

    /* System commands */
    register_reboot();
    register_meminfo();
    register_telemetry();

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
