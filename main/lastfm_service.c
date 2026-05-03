#include "lastfm_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "utils.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "jukeboy_formats.h"
#include "play_history_service.h"

#define LASTFM_SERVICE_TASK_STACK_SIZE 8192
#define LASTFM_SERVICE_TASK_PRIORITY 3
#define LASTFM_SERVICE_TASK_NAME "lastfm"
#define LASTFM_SERVICE_TASK_CORE 1
#define LASTFM_SERVICE_NVS_NAMESPACE "lastfm"
#define LASTFM_SERVICE_NVS_AUTH_URL_KEY "auth_url"
#define LASTFM_SERVICE_NVS_TOKEN_KEY "token"
#define LASTFM_SERVICE_NVS_SESSION_KEY_KEY "sk"
#define LASTFM_SERVICE_NVS_USERNAME_KEY "username"
#define LASTFM_SERVICE_AUTH_URL_MAX_LEN LASTFM_SERVICE_BASE_URL_MAX_LEN
#define LASTFM_SERVICE_TOKEN_MAX_LEN 127
#define LASTFM_SERVICE_SESSION_KEY_MAX_LEN 127
#define LASTFM_SERVICE_CMD_QUEUE_LENGTH 8
#define LASTFM_SERVICE_SCROBBLE_QUEUE_LENGTH 512
#define LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS 3
#define LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_WINDOW_MS 10000
#define LASTFM_SERVICE_SCROBBLE_RETRY_DELAY_MS 1000

#define LASTFM_SERVICE_URL_GET_TOKEN_PATH "/lastfm/auth.getToken"
#define LASTFM_SERVICE_URL_GET_MOBILE_SESSION_PATH "/lastfm/auth.getMobileSession"
#define LASTFM_SERVICE_URL_SCROBBLE_PATH "/lastfm/track.scrobble"

static const char *TAG = "lastfm_service";

EXT_RAM_BSS_ATTR static char auth_url[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char lastfm_token[LASTFM_SERVICE_TOKEN_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char session_key[LASTFM_SERVICE_SESSION_KEY_MAX_LEN + 1] = {0};
EXT_RAM_BSS_ATTR static char lastfm_username[LASTFM_SERVICE_USERNAME_MAX_LEN + 1] = {0};

static bool has_auth = false;
static bool has_token = false;
static bool has_session = false;
static bool lastfm_busy = false;
static uint32_t lastfm_successful_scrobbles = 0;
static uint32_t lastfm_failed_scrobbles = 0;

QueueHandle_t lastfm_cmd_scrobble_queue = NULL;
QueueHandle_t lastfm_cmd_queue = NULL;
static QueueSetHandle_t lastfm_queue_set = NULL;

static nvs_handle_t lastfm_nvs_handle;
static TickType_t lastfm_scrobble_request_ticks[LASTFM_SERVICE_SCROBBLE_RATE_LIMIT_REQUESTS] = {0};
static size_t lastfm_scrobble_request_count = 0;
static size_t lastfm_scrobble_request_next_index = 0;

static void lastfm_service_log_error_from_err(const char *prefix, esp_err_t err)
{
    if (prefix && prefix[0] != '\0')
    {
        ESP_LOGE(TAG, "%s: %s", prefix, esp_err_to_name(err));
        return;
    }

    ESP_LOGE(TAG, "%s", esp_err_to_name(err));
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
} lastfm_cmd_t;
typedef struct
{
    char username[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1];
    char password[LASTFM_SERVICE_AUTH_URL_MAX_LEN + 1];
} lastfm_cmd_auth_t;

// For track info we query from persisted track database in play_history_service to save memory
typedef struct
{
    uint32_t album_checksum;
    uint32_t track_index;
} lastfm_cmd_scrobble_t;

typedef struct
{
    lastfm_cmd_t cmd;
    union
    {
        lastfm_cmd_auth_t auth;
    } *data;
} lastfm_cmd_payload_t;

typedef struct
{
    lastfm_cmd_t cmd;
    lastfm_cmd_scrobble_t scrobble;
} lastfm_cmd_scrobble_payload_t;

static void lastfm_service_wait_for_scrobble_rate_limit(void)
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

static void lastfm_service_record_scrobble_request(void)
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

static void lastfm_service_requeue_scrobble(const lastfm_cmd_scrobble_payload_t *payload, esp_err_t err)
{
    if (!payload)
    {
        return;
    }

    if (xQueueSendToFront(lastfm_cmd_scrobble_queue, payload, 0) != pdTRUE)
    {
        ESP_LOGE(TAG,
                 "failed to requeue scrobble checksum=0x%08lx track=%lu after error %s",
                 (unsigned long)payload->scrobble.album_checksum,
                 (unsigned long)payload->scrobble.track_index,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG,
             "requeued scrobble checksum=0x%08lx track=%lu after error %s",
             (unsigned long)payload->scrobble.album_checksum,
             (unsigned long)payload->scrobble.track_index,
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(LASTFM_SERVICE_SCROBBLE_RETRY_DELAY_MS));
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
    if (!lastfm_cmd_scrobble_queue)
    {
        ESP_LOGE(TAG, "Cannot queue Last.fm scrobble: scrobble queue is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(lastfm_cmd_scrobble_queue, &payload, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Cannot queue Last.fm scrobble: scrobble queue is full");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t lastfm_service_request_token(void)
{
    lastfm_busy = true;
    bool token_received = false;
    lastfm_http_response_buffer_t response_buffer = {0};

    if (!has_auth)
    {
        ESP_LOGW(TAG, "Cannot request Last.fm token: missing auth URL");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
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

    esp_err_t err = esp_http_client_perform(client);
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

    if (!has_auth)
    {
        ESP_LOGW(TAG, "Cannot authenticate with Last.fm: missing auth URL");
        lastfm_busy = false;
        return ESP_ERR_INVALID_STATE;
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
    esp_err_t err = esp_http_client_perform(client);
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

    if (!payload)
    {
        return ESP_ERR_INVALID_ARG;
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

    lastfm_service_wait_for_scrobble_rate_limit();
    lastfm_service_record_scrobble_request();
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

void lastfm_service_task(void *pvParameters)
{
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

    nvs_close(lastfm_nvs_handle);

    // Check if auth url and session key are present
    has_auth = (auth_url[0] != '\0');
    has_session = (session_key[0] != '\0');
    has_token = (lastfm_token[0] != '\0');

    while (1)
    {
        QueueSetMemberHandle_t ready_queue = xQueueSelectFromSet(lastfm_queue_set, portMAX_DELAY);
        if (ready_queue == lastfm_cmd_queue)
        {
            lastfm_cmd_payload_t cmd_payload;
            if (xQueueReceive(lastfm_cmd_queue, &cmd_payload, 0) != pdTRUE)
            {
                continue;
            }

            switch (cmd_payload.cmd)
            {
            case LASTFM_CMD_GET_TOKEN:
                lastfm_service_request_token();
                break;
            case LASTFM_CMD_AUTH:
                if (cmd_payload.data)
                {
                    lastfm_service_request_auth(cmd_payload.data->auth.username, cmd_payload.data->auth.password);
                }
                break;
            case LASTFM_CMD_LOGOUT:
                lastfm_service_logout();
                break;
            default:
                ESP_LOGW(TAG, "Received unknown command: %d", cmd_payload.cmd);
                break;
            }

            if (cmd_payload.data)
            {
                free(cmd_payload.data);
            }
        }
        else if (ready_queue == lastfm_cmd_scrobble_queue)
        {
            lastfm_cmd_scrobble_payload_t scrobble_payload;
            if (xQueueReceive(lastfm_cmd_scrobble_queue, &scrobble_payload, 0) != pdTRUE)
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
    // nvs should be initialized by main
    ESP_LOGI(TAG, "Initializing Last.fm service");

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

    play_history_service_register_listen_count_callback(lastfm_service_on_listen_count_incremented,
                                                        NULL);

    return ESP_OK;
}