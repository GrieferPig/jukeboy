#include "lastfm_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "utils.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "jukeboy_formats.h"
#include "player_service.h"
#include "play_history_service.h"
#include "wifi_service.h"

#define LASTFM_SERVICE_TASK_STACK_SIZE 8192
#define LASTFM_SERVICE_TASK_PRIORITY 3
#define LASTFM_SERVICE_TASK_NAME "lastfm"
#define LASTFM_SERVICE_TASK_CORE 1
#define LASTFM_SERVICE_NVS_NAMESPACE "lastfm"
#define LASTFM_SERVICE_NVS_AUTH_URL_KEY "auth_url"
#define LASTFM_SERVICE_NVS_TOKEN_KEY "token"
#define LASTFM_SERVICE_NVS_SESSION_KEY_KEY "sk"
#define LASTFM_SERVICE_NVS_USERNAME_KEY "username"
#define LASTFM_SERVICE_NVS_SCROBBLING_ENABLED_KEY "scrobble_en"
#define LASTFM_SERVICE_NVS_NOW_PLAYING_ENABLED_KEY "nowplay_en"
#define LASTFM_SERVICE_AUTH_URL_MAX_LEN LASTFM_SERVICE_BASE_URL_MAX_LEN
#define LASTFM_SERVICE_TOKEN_MAX_LEN 127
#define LASTFM_SERVICE_SESSION_KEY_MAX_LEN 127
#define LASTFM_SERVICE_CMD_QUEUE_LENGTH 8
#define LASTFM_SERVICE_SCROBBLE_QUEUE_LENGTH 512
#define LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS 3
#define LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_WINDOW_MS 10000
#define LASTFM_SERVICE_SCROBBLE_RETRY_DELAY_MS 1000
#define LASTFM_SERVICE_SCROBBLE_QUEUE_MUTEX_TIMEOUT_MS 50
#define LASTFM_SERVICE_OFFLINE_POLL_MS 1000
#define LASTFM_SERVICE_NOW_PLAYING_DELAY_MS 10000
#define LASTFM_SERVICE_PAUSE_CLEAR_DELAY_MS 10000

#define LASTFM_SERVICE_URL_GET_TOKEN_PATH "/lastfm/auth.getToken"
#define LASTFM_SERVICE_URL_GET_MOBILE_SESSION_PATH "/lastfm/auth.getMobileSession"
#define LASTFM_SERVICE_URL_SCROBBLE_PATH "/lastfm/track.scrobble"
#define LASTFM_SERVICE_URL_UPDATE_NOW_PLAYING_PATH "/lastfm/track.updateNowPlaying"

static const char *TAG = "lastfm_service";

EXT_RAM_BSS_ATTR static char auth_url[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char lastfm_token[LASTFM_SERVICE_TOKEN_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char session_key[LASTFM_SERVICE_SESSION_KEY_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char lastfm_username[LASTFM_SERVICE_USERNAME_MAX_LEN + 1] = {0};

static bool has_auth = false;
static bool has_token = false;
static bool has_session = false;
static bool lastfm_busy = false;
static bool lastfm_scrobbling_enabled = true;
static bool lastfm_now_playing_enabled = true;
static uint32_t lastfm_successful_scrobbles = 0;
static uint32_t lastfm_failed_scrobbles = 0;

QueueHandle_t lastfm_cmd_scrobble_queue = NULL;
QueueHandle_t lastfm_cmd_queue = NULL;
static QueueSetHandle_t lastfm_queue_set = NULL;
static SemaphoreHandle_t lastfm_scrobble_queue_mutex = NULL;

static nvs_handle_t lastfm_nvs_handle;
static TickType_t lastfm_scrobble_request_ticks[LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS] = {0};
static size_t lastfm_scrobble_request_count = 0;
static size_t lastfm_scrobble_request_next_index = 0;
static esp_timer_handle_t lastfm_now_playing_timer;
static esp_timer_handle_t lastfm_pause_clear_timer;

typedef struct
{
    uint32_t album_checksum;
    uint32_t track_index;
} lastfm_track_ref_t;

static lastfm_track_ref_t lastfm_now_playing_track = {0};
static bool lastfm_now_playing_track_valid = false;
static bool lastfm_now_playing_active = false;

static void lastfm_service_log_error_from_err(const char *prefix, esp_err_t err)
{
    if (prefix && prefix[0] != '\0')
    {
        ESP_LOGE(TAG, "%s: %s", prefix, esp_err_to_name(err));
        return;
    }

    ESP_LOGE(TAG, "%s", esp_err_to_name(err));
}

static TickType_t lastfm_service_timeout_ticks(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    return timeout_ticks == 0 ? 1 : timeout_ticks;
}

static bool lastfm_service_has_internet(void)
{
    return wifi_service_has_internet();
}

static const char *lastfm_service_enabled_state(bool enabled)
{
    return enabled ? "enabled" : "disabled";
}

static bool lastfm_service_lock_scrobble_queue(void)
{
    if (!lastfm_scrobble_queue_mutex)
    {
        return false;
    }

    return xSemaphoreTake(lastfm_scrobble_queue_mutex,
                          lastfm_service_timeout_ticks(LASTFM_SERVICE_SCROBBLE_QUEUE_MUTEX_TIMEOUT_MS)) == pdTRUE;
}

static void lastfm_service_unlock_scrobble_queue(void)
{
    if (lastfm_scrobble_queue_mutex)
    {
        xSemaphoreGive(lastfm_scrobble_queue_mutex);
    }
}

static esp_err_t lastfm_service_require_internet_connection(const char *operation)
{
    if (lastfm_service_has_internet())
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "Cannot %s: internet is not available",
             operation && operation[0] != '\0' ? operation : "perform Last.fm request");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t lastfm_service_store_enabled_flag(const char *key, bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (!key)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_i32(handle, key, enabled ? 1 : 0);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static bool lastfm_service_track_ref_equal(const lastfm_track_ref_t *lhs,
                                           const lastfm_track_ref_t *rhs)
{
    return lhs && rhs &&
           lhs->album_checksum == rhs->album_checksum &&
           lhs->track_index == rhs->track_index;
}

static bool lastfm_service_track_ref_from_player_event(const player_service_track_event_t *event,
                                                       lastfm_track_ref_t *track_ref)
{
    if (!event || !track_ref)
    {
        return false;
    }

    track_ref->album_checksum = event->cartridge_checksum;
    track_ref->track_index = event->track_index;
    return true;
}

static void lastfm_service_on_listen_count_incremented(const play_history_listen_count_event_t *event,
                                                       void *user_ctx)
{
    esp_err_t err;

    (void)user_ctx;

    if (!event)
    {
        return;
    }

    err = lastfm_service_send_scrobble(event->cartridge_checksum, event->track_index);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "failed to queue scrobble after listen increment checksum=0x%08lx track=%lu: %s",
                 (unsigned long)event->cartridge_checksum,
                 (unsigned long)event->track_index,
                 esp_err_to_name(err));
    }
}

typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
    esp_err_t err;
} lastfm_http_response_buffer_t;

static esp_err_t lastfm_service_http_response_buffer_reserve(lastfm_http_response_buffer_t *buffer,
                                                             size_t required_capacity)
{
    char *new_data;
    size_t new_capacity;

    if (!buffer)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (required_capacity <= buffer->capacity)
    {
        return ESP_OK;
    }

    new_capacity = buffer->capacity ? buffer->capacity : 256;
    while (new_capacity < required_capacity)
    {
        size_t doubled_capacity = new_capacity * 2;

        if (doubled_capacity <= new_capacity)
        {
            new_capacity = required_capacity;
            break;
        }

        new_capacity = doubled_capacity;
    }

    new_data = realloc(buffer->data, new_capacity + 1);
    if (!new_data)
    {
        return ESP_ERR_NO_MEM;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void lastfm_service_http_response_buffer_free(lastfm_http_response_buffer_t *buffer)
{
    if (!buffer)
    {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->err = ESP_OK;
}

static esp_err_t lastfm_service_http_event_handler(esp_http_client_event_t *event)
{
    lastfm_http_response_buffer_t *buffer;

    if (!event || !event->user_data)
    {
        return ESP_OK;
    }

    buffer = (lastfm_http_response_buffer_t *)event->user_data;
    if (buffer->err != ESP_OK)
    {
        return buffer->err;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0)
    {
        return ESP_OK;
    }

    buffer->err = lastfm_service_http_response_buffer_reserve(buffer,
                                                              buffer->length + (size_t)event->data_len);
    if (buffer->err != ESP_OK)
    {
        return buffer->err;
    }

    memcpy(buffer->data + buffer->length, event->data, (size_t)event->data_len);
    buffer->length += (size_t)event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static esp_err_t lastfm_service_normalize_base_url(const char *base_url,
                                                   char *normalized_url,
                                                   size_t normalized_url_len)
{
    int written;

    if (!base_url || !normalized_url || normalized_url_len == 0 || base_url[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(base_url, "://") != NULL)
    {
        if (strncmp(base_url, "http://", 7) != 0 && strncmp(base_url, "https://", 8) != 0)
        {
            return ESP_ERR_INVALID_ARG;
        }

        written = snprintf(normalized_url, normalized_url_len, "%s", base_url);
    }
    else
    {
        written = snprintf(normalized_url, normalized_url_len, "https://%s", base_url);
    }

    if (written < 0 || (size_t)written >= normalized_url_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void lastfm_service_cleanup_queues(void)
{
    if (lastfm_scrobble_queue_mutex)
    {
        vSemaphoreDelete(lastfm_scrobble_queue_mutex);
        lastfm_scrobble_queue_mutex = NULL;
    }

    if (lastfm_queue_set)
    {
        if (lastfm_cmd_queue)
        {
            xQueueRemoveFromSet(lastfm_cmd_queue, lastfm_queue_set);
        }
        if (lastfm_cmd_scrobble_queue)
        {
            xQueueRemoveFromSet(lastfm_cmd_scrobble_queue, lastfm_queue_set);
        }
        vQueueDelete(lastfm_queue_set);
        lastfm_queue_set = NULL;
    }

    if (lastfm_cmd_scrobble_queue)
    {
        vQueueDelete(lastfm_cmd_scrobble_queue);
        lastfm_cmd_scrobble_queue = NULL;
    }

    if (lastfm_cmd_queue)
    {
        vQueueDelete(lastfm_cmd_queue);
        lastfm_cmd_queue = NULL;
    }
}

typedef enum
{
    LASTFM_CMD_SET_AUTH_URL,
    LASTFM_CMD_GET_TOKEN,
    LASTFM_CMD_AUTH,
    LASTFM_CMD_SCROBBLE,
    LASTFM_CMD_LOGOUT,
    LASTFM_CMD_UPDATE_NOW_PLAYING,
    LASTFM_CMD_CLEAR_NOW_PLAYING,
} lastfm_cmd_t;
typedef struct
{
    char username[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1];
    char password[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1];
} lastfm_cmd_auth_t;

// For track info we query from persisted track database in play_history_service to save memory
typedef lastfm_track_ref_t lastfm_cmd_scrobble_t;

typedef struct
{
    lastfm_cmd_t cmd;
    union
    {
        lastfm_cmd_auth_t auth;
        lastfm_track_ref_t track;
    } data;
} lastfm_cmd_payload_t;

typedef struct
{
    lastfm_cmd_t cmd;
    lastfm_cmd_scrobble_t scrobble;
} lastfm_cmd_scrobble_payload_t;

static void lastfm_service_wait_for_request_rate_limit(void)
{
    const TickType_t window_ticks = pdMS_TO_TICKS(LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_WINDOW_MS);
    TickType_t now;
    TickType_t elapsed_ticks;
    TickType_t wait_ticks;

    if (lastfm_scrobble_request_count < LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS)
    {
        return;
    }

    now = xTaskGetTickCount();
    elapsed_ticks = now - lastfm_scrobble_request_ticks[lastfm_scrobble_request_next_index];
    if (elapsed_ticks >= window_ticks)
    {
        return;
    }

    wait_ticks = window_ticks - elapsed_ticks;
    if (wait_ticks == 0)
    {
        wait_ticks = 1;
    }

    vTaskDelay(wait_ticks);
}

static void lastfm_service_record_request(void)
{
    lastfm_scrobble_request_ticks[lastfm_scrobble_request_next_index] = xTaskGetTickCount();
    lastfm_scrobble_request_next_index =
        (lastfm_scrobble_request_next_index + 1) % LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS;
    if (lastfm_scrobble_request_count < LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS)
    {
        lastfm_scrobble_request_count++;
    }
}

void lastfm_service_get_status(lastfm_service_status_t *status)
{
    if (!status)
    {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->has_auth_url = has_auth;
    status->has_token = has_token;
    status->has_session = has_session;
    status->busy = lastfm_busy;
    status->scrobbling_enabled = lastfm_scrobbling_enabled;
    status->now_playing_enabled = lastfm_now_playing_enabled;
    status->now_playing_active = lastfm_now_playing_active;
    status->command_queue_ready = (lastfm_cmd_queue != NULL);
    status->scrobble_queue_ready = (lastfm_cmd_scrobble_queue != NULL);
    status->command_queue_capacity = LASTFM_SERVICE_CMD_QUEUE_LENGTH;
    status->scrobble_queue_capacity = LASTFM_SERVICE_SCROBBLE_QUEUE_LENGTH;
    status->successful_scrobbles = lastfm_successful_scrobbles;
    status->failed_scrobbles = lastfm_failed_scrobbles;

    if (lastfm_cmd_queue)
    {
        status->pending_commands = (uint32_t)uxQueueMessagesWaiting(lastfm_cmd_queue);
    }
    if (lastfm_cmd_scrobble_queue)
    {
        status->pending_scrobbles = (uint32_t)uxQueueMessagesWaiting(lastfm_cmd_scrobble_queue);
    }

    strncpy(status->auth_url, auth_url, sizeof(status->auth_url) - 1);
    status->auth_url[sizeof(status->auth_url) - 1] = '\0';
    strncpy(status->username, lastfm_username, sizeof(status->username) - 1);
    status->username[sizeof(status->username) - 1] = '\0';
}

static esp_err_t lastfm_service_enqueue_command(const lastfm_cmd_payload_t *payload)
{
    if (!payload || !lastfm_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueSend(lastfm_cmd_queue, payload, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void lastfm_service_stop_timer(esp_timer_handle_t timer_handle)
{
    esp_err_t err;

    if (!timer_handle)
    {
        return;
    }

    err = esp_timer_stop(timer_handle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "failed to stop timer: %s", esp_err_to_name(err));
    }
}

static esp_err_t lastfm_service_start_timer_once(esp_timer_handle_t timer_handle,
                                                 uint32_t delay_ms)
{
    esp_err_t err;

    if (!timer_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    lastfm_service_stop_timer(timer_handle);
    err = esp_timer_start_once(timer_handle, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to start timer: %s", esp_err_to_name(err));
    }
    return err;
}

static void lastfm_service_reset_now_playing_state(void)
{
    memset(&lastfm_now_playing_track, 0, sizeof(lastfm_now_playing_track));
    lastfm_now_playing_track_valid = false;
    lastfm_now_playing_active = false;
}

static void lastfm_service_now_playing_timer_cb(void *arg)
{
    lastfm_cmd_payload_t payload;

    (void)arg;

    if (!lastfm_now_playing_track_valid)
    {
        return;
    }

    memset(&payload, 0, sizeof(payload));
    payload.cmd = LASTFM_CMD_UPDATE_NOW_PLAYING;
    payload.data.track = lastfm_now_playing_track;
    if (lastfm_service_enqueue_command(&payload) != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "failed to queue now-playing update checksum=0x%08lx track=%lu",
                 (unsigned long)payload.data.track.album_checksum,
                 (unsigned long)payload.data.track.track_index);
    }
}

static void lastfm_service_pause_clear_timer_cb(void *arg)
{
    lastfm_cmd_payload_t payload;

    (void)arg;

    if (!lastfm_now_playing_track_valid)
    {
        return;
    }

    memset(&payload, 0, sizeof(payload));
    payload.cmd = LASTFM_CMD_CLEAR_NOW_PLAYING;
    payload.data.track = lastfm_now_playing_track;
    if (lastfm_service_enqueue_command(&payload) != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "failed to queue now-playing clear checksum=0x%08lx track=%lu",
                 (unsigned long)payload.data.track.album_checksum,
                 (unsigned long)payload.data.track.track_index);
    }
}

static esp_err_t lastfm_service_init_timers(void)
{
    esp_err_t err;
    esp_timer_create_args_t now_playing_timer_args = {
        .callback = lastfm_service_now_playing_timer_cb,
        .arg = NULL,
        .name = "lfm_now_play",
    };
    esp_timer_create_args_t pause_clear_timer_args = {
        .callback = lastfm_service_pause_clear_timer_cb,
        .arg = NULL,
        .name = "lfm_pause",
    };

    if (!lastfm_now_playing_timer)
    {
        err = esp_timer_create(&now_playing_timer_args, &lastfm_now_playing_timer);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (!lastfm_pause_clear_timer)
    {
        err = esp_timer_create(&pause_clear_timer_args, &lastfm_pause_clear_timer);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}

static void lastfm_service_handle_track_started_event(const player_service_track_event_t *event)
{
    lastfm_track_ref_t track_ref;
    bool same_track;
    bool is_resume;

    if (!lastfm_now_playing_enabled)
    {
        lastfm_service_stop_timer(lastfm_now_playing_timer);
        lastfm_service_stop_timer(lastfm_pause_clear_timer);
        lastfm_service_reset_now_playing_state();
        ESP_LOGI(TAG, "ignoring now-playing start event while now-playing is disabled");
        return;
    }

    if (!lastfm_service_track_ref_from_player_event(event, &track_ref))
    {
        lastfm_service_stop_timer(lastfm_now_playing_timer);
        lastfm_service_stop_timer(lastfm_pause_clear_timer);
        lastfm_service_reset_now_playing_state();
        ESP_LOGI(TAG, "cleared now-playing state due to missing track context");
        return;
    }

    same_track = lastfm_now_playing_track_valid &&
                 lastfm_service_track_ref_equal(&lastfm_now_playing_track, &track_ref);
    is_resume = event && event->playback_position_sec > 0;

    lastfm_service_stop_timer(lastfm_pause_clear_timer);
    if (same_track && lastfm_now_playing_active && is_resume)
    {
        ESP_LOGI(TAG,
                 "keeping now-playing active on resume checksum=0x%08lx track=%lu",
                 (unsigned long)track_ref.album_checksum,
                 (unsigned long)track_ref.track_index);
        return;
    }

    lastfm_service_stop_timer(lastfm_now_playing_timer);
    lastfm_now_playing_track = track_ref;
    lastfm_now_playing_track_valid = true;
    lastfm_now_playing_active = false;
    (void)lastfm_service_start_timer_once(lastfm_now_playing_timer,
                                          LASTFM_SERVICE_NOW_PLAYING_DELAY_MS);
    ESP_LOGI(TAG,
             "%s now-playing update in %u ms for checksum=0x%08lx track=%lu",
             is_resume ? "scheduled resumed" : "scheduled",
             (unsigned)LASTFM_SERVICE_NOW_PLAYING_DELAY_MS,
             (unsigned long)track_ref.album_checksum,
             (unsigned long)track_ref.track_index);
}

static void lastfm_service_handle_track_paused_event(const player_service_track_event_t *event)
{
    lastfm_track_ref_t track_ref;

    lastfm_service_stop_timer(lastfm_now_playing_timer);
    if (!lastfm_now_playing_enabled)
    {
        lastfm_service_stop_timer(lastfm_pause_clear_timer);
        lastfm_service_reset_now_playing_state();
        ESP_LOGI(TAG, "ignoring now-playing pause event while now-playing is disabled");
        return;
    }

    if (lastfm_service_track_ref_from_player_event(event, &track_ref))
    {
        lastfm_now_playing_track = track_ref;
        lastfm_now_playing_track_valid = true;
    }

    if (lastfm_now_playing_active)
    {
        (void)lastfm_service_start_timer_once(lastfm_pause_clear_timer,
                                              LASTFM_SERVICE_PAUSE_CLEAR_DELAY_MS);
        ESP_LOGI(TAG,
                 "scheduled local now-playing clear in %u ms for checksum=0x%08lx track=%lu",
                 (unsigned)LASTFM_SERVICE_PAUSE_CLEAR_DELAY_MS,
                 (unsigned long)lastfm_now_playing_track.album_checksum,
                 (unsigned long)lastfm_now_playing_track.track_index);
    }
    else
    {
        ESP_LOGI(TAG, "canceled pending now-playing update because playback paused");
    }
}

static void lastfm_service_handle_track_finished_event(const player_service_track_event_t *event)
{
    (void)event;

    lastfm_service_stop_timer(lastfm_now_playing_timer);
    lastfm_service_stop_timer(lastfm_pause_clear_timer);
    lastfm_service_reset_now_playing_state();
    ESP_LOGI(TAG, "cleared now-playing state because playback finished or session was reset");
}

static void lastfm_service_on_player_event(void *handler_arg,
                                           esp_event_base_t event_base,
                                           int32_t event_id,
                                           void *event_data)
{
    (void)handler_arg;
    (void)event_base;

    switch (event_id)
    {
    case PLAYER_SVC_EVENT_TRACK_STARTED:
        lastfm_service_handle_track_started_event((const player_service_track_event_t *)event_data);
        break;
    case PLAYER_SVC_EVENT_TRACK_PAUSED:
        lastfm_service_handle_track_paused_event((const player_service_track_event_t *)event_data);
        break;
    case PLAYER_SVC_EVENT_TRACK_FINISHED:
        lastfm_service_handle_track_finished_event((const player_service_track_event_t *)event_data);
        break;
    default:
        break;
    }
}

static bool lastfm_service_lookup_scrobble_track(uint32_t album_checksum,
                                                 uint32_t track_index,
                                                 play_history_track_record_t *track_record,
                                                 play_history_album_record_t *album_record)
{
    size_t track_count;

    if (!track_record)
    {
        return false;
    }

    track_count = play_history_service_get_album_track_count(album_checksum);
    for (size_t slot = 0; slot < track_count; slot++)
    {
        play_history_track_record_t candidate;

        if (!play_history_service_get_album_track_record(album_checksum, slot, &candidate))
        {
            continue;
        }
        if (candidate.track_index != track_index)
        {
            continue;
        }

        *track_record = candidate;
        if (album_record)
        {
            memset(album_record, 0, sizeof(*album_record));
            play_history_service_get_album_record_by_checksum(album_checksum, album_record);
        }
        return true;
    }

    if (album_record)
    {
        memset(album_record, 0, sizeof(*album_record));
    }
    return false;
}

static esp_err_t lastfm_service_resolve_track_metadata(const lastfm_track_ref_t *track_ref,
                                                       play_history_track_record_t *track_record,
                                                       play_history_album_record_t *album_record,
                                                       const char **artist_out,
                                                       const char **track_name_out)
{
    const char *artist;
    const char *track_name;

    if (!track_ref || !track_record || !album_record || !artist_out || !track_name_out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lastfm_service_lookup_scrobble_track(track_ref->album_checksum,
                                              track_ref->track_index,
                                              track_record,
                                              album_record))
    {
        ESP_LOGW(TAG,
                 "failed to resolve track metadata checksum=0x%08lx track=%lu",
                 (unsigned long)track_ref->album_checksum,
                 (unsigned long)track_ref->track_index);
        return ESP_ERR_NOT_FOUND;
    }

    track_name = track_record->metadata.track_name;
    artist = track_record->metadata.artists[0] != '\0'
                 ? track_record->metadata.artists
                 : album_record->metadata.artist;
    if (track_name[0] == '\0' || artist[0] == '\0')
    {
        ESP_LOGW(TAG,
                 "track metadata missing artist or track checksum=0x%08lx track=%lu",
                 (unsigned long)track_ref->album_checksum,
                 (unsigned long)track_ref->track_index);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *artist_out = artist;
    *track_name_out = track_name;
    return ESP_OK;
}

static void lastfm_service_requeue_scrobble(const lastfm_cmd_scrobble_payload_t *payload, esp_err_t err)
{
    lastfm_cmd_scrobble_payload_t dropped_payload;

    if (!payload)
    {
        return;
    }

    if (!lastfm_cmd_scrobble_queue || !lastfm_scrobble_queue_mutex)
    {
        ESP_LOGE(TAG,
                 "failed to requeue scrobble checksum=0x%08lx track=%lu after error %s: queue is not ready",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index,
                 esp_err_to_name(err));
        return;
    }

    if (!lastfm_service_lock_scrobble_queue())
    {
        ESP_LOGE(TAG,
                 "failed to requeue scrobble checksum=0x%08lx track=%lu after error %s: mutex timeout",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index,
                 esp_err_to_name(err));
        return;
    }

    if (xQueueSendToFront(lastfm_cmd_scrobble_queue, payload, 0) != pdTRUE)
    {
        if (xQueueReceive(lastfm_cmd_scrobble_queue, &dropped_payload, 0) == pdTRUE &&
            xQueueSendToFront(lastfm_cmd_scrobble_queue, payload, 0) == pdTRUE)
        {
            lastfm_service_unlock_scrobble_queue();
            ESP_LOGW(TAG,
                     "dropped oldest queued scrobble checksum=0x%08lx track=%lu while requeueing failed scrobble checksum=0x%08lx track=%lu",
                     (unsigned long)dropped_payload.scrobble.album_checksum,
                     (unsigned long)dropped_payload.scrobble.track_index,
                     (unsigned long)payload->scrobble.album_checksum,
                     (unsigned long)payload->scrobble.track_index);
            ESP_LOGW(TAG,
                     "requeued scrobble checksum=0x%08lx track=%lu after error %s",
                     (unsigned long)payload->scrobble.album_checksum,
                     (unsigned long)payload->scrobble.track_index,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(LASTFM_SERVICE_SCROBBLE_RETRY_DELAY_MS));
            return;
        }

        lastfm_service_unlock_scrobble_queue();
        ESP_LOGE(TAG,
                 "failed to requeue scrobble checksum=0x%08lx track=%lu after error %s",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index,
                 esp_err_to_name(err));
        return;
    }

    lastfm_service_unlock_scrobble_queue();

    ESP_LOGW(TAG,
             "requeued scrobble checksum=0x%08lx track=%lu after error %s",
             (unsigned long)payload->scrobble.album_checksum,
             (unsigned long)payload->scrobble.track_index,
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(LASTFM_SERVICE_SCROBBLE_RETRY_DELAY_MS));
}

static esp_err_t lastfm_service_receive_scrobble(lastfm_cmd_scrobble_payload_t *payload)
{
    BaseType_t queue_result;

    if (!payload)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lastfm_cmd_scrobble_queue || !lastfm_scrobble_queue_mutex)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lastfm_service_lock_scrobble_queue())
    {
        return ESP_ERR_TIMEOUT;
    }

    queue_result = xQueueReceive(lastfm_cmd_scrobble_queue, payload, 0);
    lastfm_service_unlock_scrobble_queue();

    return queue_result == pdTRUE ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t lastfm_update_now_playing(const char *artist, const char *track)
{
    char url[256];
    cJSON *root = NULL;
    char *post_data = NULL;
    esp_http_client_handle_t http_client = NULL;
    esp_err_t err;

    lastfm_busy = true;

    if (!has_auth || !has_session)
    {
        ESP_LOGW(TAG, "Cannot update Last.fm now playing: missing auth URL or session key");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    if (!root)
    {
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "sk", session_key);
    cJSON_AddStringToObject(root, "artist", artist);
    cJSON_AddStringToObject(root, "track", track);

    post_data = cJSON_PrintUnformatted(root);
    if (!post_data)
    {
        cJSON_Delete(root);
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }

    snprintf(url, sizeof(url), "%s%s", auth_url, LASTFM_SERVICE_URL_UPDATE_NOW_PLAYING_PATH);

    esp_http_client_config_t http_client_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    http_client = esp_http_client_init(&http_client_config);
    if (!http_client)
    {
        free(post_data);
        cJSON_Delete(root);
        lastfm_busy = false;
        return ESP_FAIL;
    }

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(http_client, post_data, strlen(post_data));

    err = esp_http_client_perform(http_client);
    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(http_client);
        ESP_LOGI(TAG, "Now-playing update sent. HTTP Status: %d", status_code);
        if (status_code != 200)
        {
            ESP_LOGE(TAG, "track.updateNowPlaying returned HTTP status %d", status_code);
            err = ESP_FAIL;
        }
    }
    else
    {
        lastfm_service_log_error_from_err("track.updateNowPlaying request failed", err);
    }

    esp_http_client_cleanup(http_client);
    cJSON_Delete(root);
    free(post_data);
    lastfm_busy = false;
    return err;
}

static esp_err_t lastfm_service_process_now_playing_track(const lastfm_track_ref_t *track_ref)
{
    play_history_track_record_t track_record;
    play_history_album_record_t album_record;
    const char *artist;
    const char *track_name;
    esp_err_t err;

    if (!track_ref)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lastfm_now_playing_enabled)
    {
        ESP_LOGI(TAG,
                 "consumed now-playing update without sending checksum=0x%08lx track=%lu because now-playing is disabled",
                 (unsigned long)track_ref->album_checksum,
                 (unsigned long)track_ref->track_index);
        return ESP_OK;
    }

    if (!lastfm_now_playing_track_valid ||
        !lastfm_service_track_ref_equal(track_ref, &lastfm_now_playing_track) ||
        lastfm_now_playing_active)
    {
        return ESP_OK;
    }

    err = lastfm_service_require_internet_connection("update Last.fm now playing");
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG,
                 "consumed now-playing update without sending checksum=0x%08lx track=%lu because internet is unavailable",
                 (unsigned long)track_ref->album_checksum,
                 (unsigned long)track_ref->track_index);
        return ESP_OK;
    }

    err = lastfm_service_resolve_track_metadata(track_ref,
                                                &track_record,
                                                &album_record,
                                                &artist,
                                                &track_name);
    if (err != ESP_OK)
    {
        return err;
    }

    lastfm_service_wait_for_request_rate_limit();
    lastfm_service_record_request();
    err = lastfm_update_now_playing(artist, track_name);
    if (err == ESP_OK &&
        lastfm_now_playing_track_valid &&
        lastfm_service_track_ref_equal(track_ref, &lastfm_now_playing_track))
    {
        lastfm_now_playing_active = true;
        ESP_LOGI(TAG,
                 "now-playing active for checksum=0x%08lx track=%lu",
                 (unsigned long)track_ref->album_checksum,
                 (unsigned long)track_ref->track_index);
    }
    return err;
}

static void lastfm_service_process_now_playing_clear(const lastfm_track_ref_t *track_ref)
{
    if (!track_ref ||
        !lastfm_now_playing_track_valid ||
        !lastfm_service_track_ref_equal(track_ref, &lastfm_now_playing_track))
    {
        return;
    }

    lastfm_now_playing_active = false;
    ESP_LOGI(TAG,
             "cleared local now-playing state after pause timeout checksum=0x%08lx track=%lu",
             (unsigned long)track_ref->album_checksum,
             (unsigned long)track_ref->track_index);
}

static void lastfm_service_process_command(lastfm_cmd_payload_t *cmd_payload)
{
    esp_err_t err;

    if (!cmd_payload)
    {
        return;
    }

    switch (cmd_payload->cmd)
    {
    case LASTFM_CMD_GET_TOKEN:
        lastfm_service_request_token();
        break;
    case LASTFM_CMD_AUTH:
        lastfm_service_request_auth(cmd_payload->data.auth.username, cmd_payload->data.auth.password);
        break;
    case LASTFM_CMD_LOGOUT:
        lastfm_service_logout();
        lastfm_service_handle_track_finished_event(NULL);
        break;
    case LASTFM_CMD_UPDATE_NOW_PLAYING:
        err = lastfm_service_process_now_playing_track(&cmd_payload->data.track);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "failed to process now-playing update checksum=0x%08lx track=%lu: %s",
                     (unsigned long)cmd_payload->data.track.album_checksum,
                     (unsigned long)cmd_payload->data.track.track_index,
                     esp_err_to_name(err));
        }
        break;
    case LASTFM_CMD_CLEAR_NOW_PLAYING:
        lastfm_service_process_now_playing_clear(&cmd_payload->data.track);
        break;
    default:
        ESP_LOGW(TAG, "Received unknown command: %d", cmd_payload->cmd);
        break;
    }
}

esp_err_t lastfm_service_send_scrobble(uint32_t album_checksum, uint32_t track_index)
{
    lastfm_cmd_scrobble_payload_t payload = {
        .cmd = LASTFM_CMD_SCROBBLE,
        .scrobble = {
            .album_checksum = album_checksum,
            .track_index = track_index,
        },
    };
    lastfm_cmd_scrobble_payload_t dropped_payload;

    if (!lastfm_scrobbling_enabled)
    {
        ESP_LOGI(TAG,
                 "ignoring scrobble checksum=0x%08lx track=%lu because scrobbling is disabled",
                 (unsigned long)album_checksum,
                 (unsigned long)track_index);
        return ESP_OK;
    }

    if (!lastfm_cmd_scrobble_queue || !lastfm_scrobble_queue_mutex)
    {
        ESP_LOGE(TAG, "Cannot queue Last.fm scrobble: scrobble queue is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (!lastfm_service_lock_scrobble_queue())
    {
        ESP_LOGE(TAG, "Cannot queue Last.fm scrobble: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    if (xQueueSend(lastfm_cmd_scrobble_queue, &payload, 0) != pdTRUE)
    {
        if (xQueueReceive(lastfm_cmd_scrobble_queue, &dropped_payload, 0) != pdTRUE ||
            xQueueSend(lastfm_cmd_scrobble_queue, &payload, 0) != pdTRUE)
        {
            lastfm_service_unlock_scrobble_queue();
            ESP_LOGE(TAG, "Cannot queue Last.fm scrobble: scrobble queue is full");
            return ESP_FAIL;
        }

        lastfm_service_unlock_scrobble_queue();
        ESP_LOGW(TAG,
                 "dropped oldest queued scrobble checksum=0x%08lx track=%lu to queue checksum=0x%08lx track=%lu",
                 (unsigned long)dropped_payload.scrobble.album_checksum,
                 (unsigned long)dropped_payload.scrobble.track_index,
                 (unsigned long)payload.scrobble.album_checksum,
                 (unsigned long)payload.scrobble.track_index);
        return ESP_OK;
    }

    lastfm_service_unlock_scrobble_queue();
    return ESP_OK;
}

esp_err_t lastfm_service_request_token(void)
{
    lastfm_busy = true;
    bool token_received = false;
    lastfm_http_response_buffer_t response_buffer = {0};
    esp_err_t err;

    if (!has_auth)
    {
        ESP_LOGW(TAG, "Cannot request Last.fm token: missing auth URL");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
    }

    err = lastfm_service_require_internet_connection("request Last.fm token");
    if (err != ESP_OK)
    {
        lastfm_busy = false;
        return err;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", auth_url, LASTFM_SERVICE_URL_GET_TOKEN_PATH);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .event_handler = lastfm_service_http_event_handler,
        .user_data = &response_buffer,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for auth.getToken");
        lastfm_busy = false;
        return ESP_FAIL;
    }

    // getToken in the backend is defined as a proxy method, so it needs POST but no specific body based on METHOD_CONFIGS
    esp_http_client_set_post_field(client, "{}", 2);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    err = esp_http_client_perform(client);
    if (err == ESP_OK && response_buffer.err != ESP_OK)
    {
        err = response_buffer.err;
    }

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200)
        {
            if (response_buffer.length > 0 && response_buffer.data)
            {
                cJSON *resp_json = cJSON_Parse(response_buffer.data);
                if (resp_json)
                {
                    cJSON *token_item = cJSON_GetObjectItem(resp_json, "token");
                    if (cJSON_IsString(token_item) && (token_item->valuestring != NULL))
                    {
                        strncpy(lastfm_token, token_item->valuestring, LASTFM_SERVICE_TOKEN_MAX_LEN);
                        lastfm_token[LASTFM_SERVICE_TOKEN_MAX_LEN] = '\0';
                        has_token = true;
                        token_received = true;

                        nvs_handle_t nvs_h;
                        if (nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK)
                        {
                            nvs_set_str(nvs_h, LASTFM_SERVICE_NVS_TOKEN_KEY, lastfm_token);
                            nvs_commit(nvs_h);
                            nvs_close(nvs_h);
                            ESP_LOGI(TAG, "Token received and saved: %s", lastfm_token);
                        }
                    }
                    cJSON_Delete(resp_json);
                }
            }

            if (!token_received)
            {
                if (response_buffer.length == 0)
                {
                    ESP_LOGE(TAG, "auth.getToken returned an empty response body");
                }
                else
                {
                    ESP_LOGE(TAG, "auth.getToken response missing token");
                }
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        else
        {
            ESP_LOGE(TAG, "auth.getToken returned HTTP status %d", status_code);
            err = ESP_FAIL;
        }
    }
    else
    {
        lastfm_service_log_error_from_err("auth.getToken request failed", err);
    }

    lastfm_service_http_response_buffer_free(&response_buffer);
    esp_http_client_cleanup(client);
    lastfm_busy = false;
    return err;
}

static esp_err_t lastfm_scrobble(const char *artist, const char *track)
{
    lastfm_busy = true;
    char url[256];
    cJSON *root = NULL;
    char *post_data = NULL;
    esp_http_client_handle_t http_client = NULL;
    esp_err_t err;

    // Check if we have valid session and auth URL
    if (!has_auth || !has_session)
    {
        ESP_LOGW(TAG, "Cannot scrobble: missing auth URL or session key");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
    }
    // Get current Unix timestamp
    time_t now;
    time(&now);

    // Check if timestamp is valid
    if (now < 1577836800) // Jan 1, 2020
    {
        ESP_LOGW(TAG, "Current time is before Jan 1, 2020; cannot scrobble");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
    }

    // Construct JSON Payload
    root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to allocate track.scrobble payload");
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "sk", session_key);
    cJSON_AddStringToObject(root, "artist", artist);
    cJSON_AddStringToObject(root, "track", track);
    cJSON_AddNumberToObject(root, "timestamp", (double)now);

    post_data = cJSON_PrintUnformatted(root);
    if (!post_data)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to encode track.scrobble payload");
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }

    snprintf(url, sizeof(url), "%s%s", auth_url, LASTFM_SERVICE_URL_SCROBBLE_PATH);

    esp_http_client_config_t http_client_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, // Crucial for HTTPS
    };
    http_client = esp_http_client_init(&http_client_config);
    if (!http_client)
    {
        free(post_data);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to initialize HTTP client for track.scrobble");
        lastfm_busy = false;
        return ESP_FAIL;
    }

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(http_client, post_data, strlen(post_data));

    err = esp_http_client_perform(http_client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(http_client);
        ESP_LOGI(TAG, "Scrobble sent. HTTP Status: %d", status_code);
        if (status_code == 200)
        {
            lastfm_successful_scrobbles++;
        }
        else
        {
            lastfm_failed_scrobbles++;
            ESP_LOGE(TAG, "track.scrobble returned HTTP status %d", status_code);
            err = ESP_FAIL;
        }
    }
    else
    {
        lastfm_failed_scrobbles++;
        lastfm_service_log_error_from_err("track.scrobble request failed", err);
    }

    esp_http_client_cleanup(http_client);
    cJSON_Delete(root);
    free(post_data);
    lastfm_busy = false;
    return err;
}

esp_err_t lastfm_service_set_auth_url(const char *url)
{
    char normalized_url[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1];
    esp_err_t err = lastfm_service_normalize_base_url(url, normalized_url, sizeof(normalized_url));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Invalid Last.fm base URL");
        return err;
    }

    err = nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &lastfm_nvs_handle);
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to open Last.fm NVS namespace", err);
        return err;
    }

    err = nvs_set_str(lastfm_nvs_handle, LASTFM_SERVICE_NVS_AUTH_URL_KEY, normalized_url);
    if (err == ESP_OK)
    {
        err = nvs_commit(lastfm_nvs_handle);
        if (err == ESP_OK)
        {
            strncpy(auth_url, normalized_url, sizeof(auth_url) - 1);
            auth_url[sizeof(auth_url) - 1] = '\0';
            has_auth = true;
        }
    }
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to save Last.fm base URL", err);
    }

    nvs_close(lastfm_nvs_handle);
    return err;
}

esp_err_t lastfm_service_request_auth(const char *username, const char *password)
{
    lastfm_busy = true;
    bool session_received = false;
    lastfm_http_response_buffer_t response_buffer = {0};
    esp_err_t err;

    if (!has_auth)
    {
        ESP_LOGW(TAG, "Cannot authenticate with Last.fm: missing auth URL");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
    }

    err = lastfm_service_require_internet_connection("authenticate with Last.fm");
    if (err != ESP_OK)
    {
        lastfm_busy = false;
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to allocate auth.getMobileSession payload");
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "username", username);
    cJSON_AddStringToObject(root, "password", password);
    char *post_data = cJSON_PrintUnformatted(root);
    if (!post_data)
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to encode auth.getMobileSession payload");
        lastfm_busy = false;
        return ESP_ERR_NO_MEM;
    }

    // 2. Configure the HTTP Client targeting your Worker
    char url[256];
    snprintf(url, sizeof(url), "%s%s", auth_url, LASTFM_SERVICE_URL_GET_MOBILE_SESSION_PATH);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, // Crucial for HTTPS
        .method = HTTP_METHOD_POST,
        .event_handler = lastfm_service_http_event_handler,
        .user_data = &response_buffer,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        cJSON_Delete(root);
        free(post_data);
        ESP_LOGE(TAG, "Failed to initialize HTTP client for auth.getMobileSession");
        lastfm_busy = false;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    // 3. Perform the request and handle the response
    err = esp_http_client_perform(client);
    if (err == ESP_OK && response_buffer.err != ESP_OK)
    {
        err = response_buffer.err;
    }

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200)
        {
            if (response_buffer.length > 0 && response_buffer.data)
            {
                cJSON *resp_json = cJSON_Parse(response_buffer.data);
                if (resp_json != NULL)
                {
                    cJSON *session = cJSON_GetObjectItem(resp_json, "session");
                    cJSON *session_key_item = NULL;

                    if (cJSON_IsObject(session))
                    {
                        session_key_item = cJSON_GetObjectItem(session, "key");
                    }

                    if (cJSON_IsString(session_key_item) && (session_key_item->valuestring != NULL))
                    {
                        const char *sk = session_key_item->valuestring;

                        strncpy(session_key, sk, LASTFM_SERVICE_SESSION_KEY_MAX_LEN);
                        session_key[LASTFM_SERVICE_SESSION_KEY_MAX_LEN] = '\0';
                        has_session = true;
                        session_received = true;

                        // 5. Save to NVS
                        nvs_handle_t my_handle;
                        if (nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK)
                        {
                            nvs_set_str(my_handle, LASTFM_SERVICE_NVS_SESSION_KEY_KEY, sk);
                            nvs_set_str(my_handle, LASTFM_SERVICE_NVS_USERNAME_KEY, username);
                            nvs_commit(my_handle);
                            nvs_close(my_handle);
                            ESP_LOGI(TAG, "Last.fm session key saved to NVS");
                        }

                        strncpy(lastfm_username, username, sizeof(lastfm_username) - 1);
                        lastfm_username[sizeof(lastfm_username) - 1] = '\0';
                    }
                    cJSON_Delete(resp_json);
                }
            }

            if (!session_received)
            {
                if (response_buffer.length == 0)
                {
                    ESP_LOGE(TAG, "auth.getMobileSession returned an empty response body");
                }
                else
                {
                    ESP_LOGE(TAG, "auth.getMobileSession response missing session");
                }
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        else
        {
            ESP_LOGE(TAG, "auth.getMobileSession returned HTTP status %d", status_code);
            err = ESP_FAIL;
        }
    }
    else
    {
        lastfm_service_log_error_from_err("auth.getMobileSession request failed", err);
    }

    // Cleanup
    lastfm_service_http_response_buffer_free(&response_buffer);
    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    free(post_data);
    lastfm_busy = false;
    return err;
}

static esp_err_t lastfm_service_process_scrobble_payload(const lastfm_cmd_scrobble_payload_t *payload)
{
    play_history_track_record_t track_record;
    play_history_album_record_t album_record;
    const char *artist;
    const char *track_name;
    esp_err_t err;

    if (!payload)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lastfm_scrobbling_enabled)
    {
        ESP_LOGI(TAG,
                 "consumed scrobble without sending checksum=0x%08lx track=%lu because scrobbling is disabled",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index);
        return ESP_OK;
    }

    if (!lastfm_service_lookup_scrobble_track(payload->scrobble.album_checksum,
                                              payload->scrobble.track_index,
                                              &track_record,
                                              &album_record))
    {
        ESP_LOGW(TAG,
                 "failed to resolve scrobble metadata for checksum=0x%08lx track=%lu",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index);
        return ESP_ERR_NOT_FOUND;
    }

    track_name = track_record.metadata.track_name;
    artist = track_record.metadata.artists[0] != '\0'
                 ? track_record.metadata.artists
                 : album_record.metadata.artist;
    if (track_name[0] == '\0' || artist[0] == '\0')
    {
        ESP_LOGW(TAG,
                 "scrobble metadata missing artist or track for checksum=0x%08lx track=%lu",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = lastfm_service_require_internet_connection("scrobble to Last.fm");
    if (err != ESP_OK)
    {
        return err;
    }

    lastfm_service_wait_for_request_rate_limit();
    lastfm_service_record_request();
    return lastfm_scrobble(artist, track_name);
}

esp_err_t lastfm_service_logout(void)
{
    esp_err_t err;

    err = nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &lastfm_nvs_handle);
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to open Last.fm NVS namespace", err);
        return err;
    }

    err = nvs_erase_key(lastfm_nvs_handle, LASTFM_SERVICE_NVS_SESSION_KEY_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }
    if (err == ESP_OK)
    {
        esp_err_t erase_username_err = nvs_erase_key(lastfm_nvs_handle, LASTFM_SERVICE_NVS_USERNAME_KEY);
        if (erase_username_err != ESP_OK && erase_username_err != ESP_ERR_NVS_NOT_FOUND)
        {
            err = erase_username_err;
        }
    }
    if (err == ESP_OK)
    {
        esp_err_t erase_token_err = nvs_erase_key(lastfm_nvs_handle, LASTFM_SERVICE_NVS_TOKEN_KEY);
        if (erase_token_err != ESP_OK && erase_token_err != ESP_ERR_NVS_NOT_FOUND)
        {
            err = erase_token_err;
        }
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(lastfm_nvs_handle);
    }

    nvs_close(lastfm_nvs_handle);
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to clear Last.fm session", err);
        return err;
    }

    session_key[0] = '\0';
    lastfm_username[0] = '\0';
    lastfm_token[0] = '\0';
    has_session = false;
    has_token = false;
    return ESP_OK;
}

esp_err_t lastfm_service_set_scrobbling_enabled(bool enabled)
{
    esp_err_t err = lastfm_service_store_enabled_flag(LASTFM_SERVICE_NVS_SCROBBLING_ENABLED_KEY, enabled);

    if (err != ESP_OK)
    {
        return err;
    }

    lastfm_scrobbling_enabled = enabled;
    ESP_LOGI(TAG, "Last.fm scrobbling %s", lastfm_service_enabled_state(enabled));
    return ESP_OK;
}

esp_err_t lastfm_service_set_now_playing_enabled(bool enabled)
{
    esp_err_t err = lastfm_service_store_enabled_flag(LASTFM_SERVICE_NVS_NOW_PLAYING_ENABLED_KEY, enabled);

    if (err != ESP_OK)
    {
        return err;
    }

    lastfm_now_playing_enabled = enabled;
    if (!enabled)
    {
        lastfm_service_stop_timer(lastfm_now_playing_timer);
        lastfm_service_stop_timer(lastfm_pause_clear_timer);
        lastfm_service_reset_now_playing_state();
    }
    ESP_LOGI(TAG, "Last.fm now-playing %s", lastfm_service_enabled_state(enabled));
    return ESP_OK;
}

void lastfm_service_task(void *pvParameters)
{
    (void)pvParameters;

    // init queues
    lastfm_cmd_queue = xQueueCreateWithCaps(LASTFM_SERVICE_CMD_QUEUE_LENGTH, sizeof(lastfm_cmd_payload_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lastfm_cmd_queue)
    {
        ESP_LOGE(TAG, "Failed to create command queue");
        return;
    }

    lastfm_cmd_scrobble_queue = xQueueCreateWithCaps(LASTFM_SERVICE_SCROBBLE_QUEUE_LENGTH, sizeof(lastfm_cmd_scrobble_payload_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lastfm_cmd_scrobble_queue)
    {
        ESP_LOGE(TAG, "Failed to create scrobble queue");
        lastfm_service_cleanup_queues();
        return;
    }

    lastfm_scrobble_queue_mutex = xSemaphoreCreateMutex();
    if (!lastfm_scrobble_queue_mutex)
    {
        ESP_LOGE(TAG, "Failed to create scrobble queue mutex");
        lastfm_service_cleanup_queues();
        return;
    }

    lastfm_queue_set = xQueueCreateSet(LASTFM_SERVICE_CMD_QUEUE_LENGTH + LASTFM_SERVICE_SCROBBLE_QUEUE_LENGTH);
    if (!lastfm_queue_set)
    {
        ESP_LOGE(TAG, "Failed to create queue set");
        lastfm_service_cleanup_queues();
        return;
    }

    if (xQueueAddToSet(lastfm_cmd_queue, lastfm_queue_set) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to add command queue to queue set");
        lastfm_service_cleanup_queues();
        return;
    }

    if (xQueueAddToSet(lastfm_cmd_scrobble_queue, lastfm_queue_set) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to add scrobble queue to queue set");
        lastfm_service_cleanup_queues();
        return;
    }

    esp_err_t err = nvs_open(LASTFM_SERVICE_NVS_NAMESPACE, NVS_READWRITE, &lastfm_nvs_handle);
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to open Last.fm NVS namespace", err);
        lastfm_service_cleanup_queues();
        return;
    }

    // initialize keys if they don't exist
    err = nvs_get_str_or_init(lastfm_nvs_handle, LASTFM_SERVICE_NVS_AUTH_URL_KEY, auth_url, LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1, "");
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to load Last.fm base URL", err);
        nvs_close(lastfm_nvs_handle);
        lastfm_service_cleanup_queues();
        return;
    }
    err = nvs_get_str_or_init(lastfm_nvs_handle, LASTFM_SERVICE_NVS_SESSION_KEY_KEY, session_key, LASTFM_SERVICE_SESSION_KEY_MAX_LEN + 1, "");
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to load Last.fm session key", err);
        nvs_close(lastfm_nvs_handle);
        lastfm_service_cleanup_queues();
        return;
    }
    err = nvs_get_str_or_init(lastfm_nvs_handle, LASTFM_SERVICE_NVS_TOKEN_KEY, lastfm_token, LASTFM_SERVICE_TOKEN_MAX_LEN + 1, "");
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to load Last.fm token", err);
        nvs_close(lastfm_nvs_handle);
        lastfm_service_cleanup_queues();
        return;
    }
    err = nvs_get_str_or_init(lastfm_nvs_handle, LASTFM_SERVICE_NVS_USERNAME_KEY, lastfm_username, LASTFM_SERVICE_USERNAME_MAX_LEN + 1, "");
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to load Last.fm username", err);
        nvs_close(lastfm_nvs_handle);
        lastfm_service_cleanup_queues();
        return;
    }
    {
        int32_t scrobbling_enabled_i32 = 1;
        int32_t now_playing_enabled_i32 = 1;

        err = nvs_get_i32_or_init(lastfm_nvs_handle,
                                  LASTFM_SERVICE_NVS_SCROBBLING_ENABLED_KEY,
                                  &scrobbling_enabled_i32,
                                  1);
        if (err != ESP_OK)
        {
            lastfm_service_log_error_from_err("failed to load Last.fm scrobbling toggle", err);
            nvs_close(lastfm_nvs_handle);
            lastfm_service_cleanup_queues();
            return;
        }

        err = nvs_get_i32_or_init(lastfm_nvs_handle,
                                  LASTFM_SERVICE_NVS_NOW_PLAYING_ENABLED_KEY,
                                  &now_playing_enabled_i32,
                                  1);
        if (err != ESP_OK)
        {
            lastfm_service_log_error_from_err("failed to load Last.fm now-playing toggle", err);
            nvs_close(lastfm_nvs_handle);
            lastfm_service_cleanup_queues();
            return;
        }

        lastfm_scrobbling_enabled = (scrobbling_enabled_i32 != 0);
        lastfm_now_playing_enabled = (now_playing_enabled_i32 != 0);
    }

    nvs_close(lastfm_nvs_handle);

    // Check if auth url and session key are present
    has_auth = (auth_url[0] != '\0');
    has_session = (session_key[0] != '\0');
    has_token = (lastfm_token[0] != '\0');
    ESP_LOGI(TAG,
             "Last.fm toggles loaded: scrobbling=%s now-playing=%s",
             lastfm_service_enabled_state(lastfm_scrobbling_enabled),
             lastfm_service_enabled_state(lastfm_now_playing_enabled));

    while (1)
    {
        if (!lastfm_service_has_internet())
        {
            lastfm_cmd_payload_t cmd_payload;

            if (xQueueReceive(lastfm_cmd_queue,
                              &cmd_payload,
                              lastfm_service_timeout_ticks(LASTFM_SERVICE_OFFLINE_POLL_MS)) != pdTRUE)
            {
                continue;
            }

            lastfm_service_process_command(&cmd_payload);
            continue;
        }

        QueueSetMemberHandle_t ready_queue = xQueueSelectFromSet(lastfm_queue_set, portMAX_DELAY);
        if (ready_queue == lastfm_cmd_queue)
        {
            lastfm_cmd_payload_t cmd_payload;
            if (xQueueReceive(lastfm_cmd_queue, &cmd_payload, 0) != pdTRUE)
            {
                continue;
            }

            lastfm_service_process_command(&cmd_payload);
        }
        else if (ready_queue == lastfm_cmd_scrobble_queue)
        {
            lastfm_cmd_scrobble_payload_t scrobble_payload;
            if (!lastfm_service_has_internet())
            {
                continue;
            }

            if (lastfm_service_receive_scrobble(&scrobble_payload) != ESP_OK)
            {
                continue;
            }

            switch (scrobble_payload.cmd)
            {
            case LASTFM_CMD_SCROBBLE:
            {
                esp_err_t scrobble_err = lastfm_service_process_scrobble_payload(&scrobble_payload);
                if (scrobble_err != ESP_OK)
                {
                    lastfm_service_requeue_scrobble(&scrobble_payload, scrobble_err);
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "Received unknown scrobble command: %d", scrobble_payload.cmd);
                break;
            }
        }
    }
}

esp_err_t lastfm_service_init(void)
{
    esp_err_t err;

    // nvs should be initialized by main
    ESP_LOGI(TAG, "Initializing Last.fm service");

    err = lastfm_service_init_timers();
    if (err != ESP_OK)
    {
        lastfm_service_log_error_from_err("failed to initialize Last.fm now-playing timers", err);
        return err;
    }

    TaskHandle_t lastfm_service_task_handle;
    xTaskCreatePinnedToCore(
        lastfm_service_task,
        LASTFM_SERVICE_TASK_NAME,
        LASTFM_SERVICE_TASK_STACK_SIZE,
        NULL,
        LASTFM_SERVICE_TASK_PRIORITY,
        &lastfm_service_task_handle,
        LASTFM_SERVICE_TASK_CORE);

    if (lastfm_service_task_handle == NULL)
    {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(PLAYER_SERVICE_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               lastfm_service_on_player_event,
                                               NULL));
    play_history_service_register_listen_count_callback(lastfm_service_on_listen_count_incremented,
                                                        NULL);

    return ESP_OK;
}