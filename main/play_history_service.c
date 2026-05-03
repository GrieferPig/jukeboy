#include "play_history_service.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include "cartridge_service.h"
#include "player_service.h"
#include "power_mgmt_service.h"
#include "storage_paths.h"

#define PLAY_HISTORY_STATE_VERSION 1U
#define PLAY_HISTORY_EVENT_QUEUE_DEPTH 64U
#define PLAY_HISTORY_EVENTS_PER_PASS 8U
#define PLAY_HISTORY_SHUTDOWN_PRIORITY 90
#define PLAY_HISTORY_FILE_PATH APP_LITTLEFS_MOUNT_PATH "/play_history.bin"
#define PLAY_HISTORY_SERIAL_IO_BYTES 4096U
#define PLAY_HISTORY_COMMIT_MAX_PASSES 3U

static const char *TAG = "play_history";

typedef struct
{
    uint8_t in_use;
    uint8_t reserved[3];
    uint32_t checksum;
    uint32_t track_count;
    uint32_t first_seen_sequence;
    uint32_t last_seen_sequence;
    jukeboy_jbm_header_t metadata;
} play_history_album_slot_t;

typedef struct
{
    uint8_t in_use;
    uint8_t reserved[3];
    uint32_t cartridge_checksum;
    uint32_t track_index;
    uint32_t track_file_num;
    uint32_t play_count;
    uint32_t first_seen_sequence;
    uint32_t last_seen_sequence;
    jukeboy_jbm_track_t metadata;
} play_history_track_slot_t;

typedef struct
{
    uint32_t version;
    uint32_t next_sequence;
    uint32_t album_count;
    uint32_t track_count;
    play_history_album_slot_t albums[PLAY_HISTORY_MAX_ALBUMS];
    play_history_track_slot_t tracks[PLAY_HISTORY_MAX_TRACKS];
} play_history_cache_t;

typedef struct
{
    uint32_t version;
    uint32_t next_sequence;
    uint32_t album_count;
    uint32_t track_count;
} play_history_file_header_t;

typedef struct
{
    uint32_t checksum;
    uint32_t track_count;
    uint32_t first_seen_sequence;
    uint32_t last_seen_sequence;
    jukeboy_jbm_header_t metadata;
} play_history_disk_album_t;

typedef struct
{
    uint32_t cartridge_checksum;
    uint32_t track_index;
    uint32_t track_file_num;
    uint32_t play_count;
    uint32_t first_seen_sequence;
    uint32_t last_seen_sequence;
    jukeboy_jbm_track_t metadata;
} play_history_disk_track_t;

#define PLAY_HISTORY_MAX_SERIALIZED_BYTES                                    \
    (sizeof(play_history_file_header_t) +                                    \
     ((size_t)PLAY_HISTORY_MAX_ALBUMS * sizeof(play_history_disk_album_t)) + \
     ((size_t)PLAY_HISTORY_MAX_TRACKS * sizeof(play_history_disk_track_t)))

typedef enum
{
    PLAY_HISTORY_EVENT_SYNC_CURRENT_CARTRIDGE = 0,
    PLAY_HISTORY_EVENT_COUNTABLE_PLAY,
    PLAY_HISTORY_EVENT_CLEAR,
    PLAY_HISTORY_EVENT_REBUILD,
} play_history_event_type_t;

typedef struct
{
    play_history_event_type_t type;
    union
    {
        player_service_track_event_t track_event;
    } data;
} play_history_event_t;

static play_history_cache_t *s_cache_ptr;
#define s_cache (*s_cache_ptr)

static StaticSemaphore_t s_cache_mutex_storage;
static SemaphoreHandle_t s_cache_mutex;
static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_commit_state_lock = portMUX_INITIALIZER_UNLOCKED;
static play_history_event_t s_event_queue[PLAY_HISTORY_EVENT_QUEUE_DEPTH];
static size_t s_event_head;
static size_t s_event_count;
static bool s_initialised;
static bool s_dirty;
static bool s_event_overflow_logged;
static bool s_commit_in_progress;
static bool s_commit_requested;
static esp_err_t s_commit_result;
static TaskHandle_t s_process_task_handle;

static EXT_RAM_BSS_ATTR uint8_t s_serialized_buf[PLAY_HISTORY_MAX_SERIALIZED_BYTES];
static EXT_RAM_BSS_ATTR uint8_t s_commit_actual_buf[PLAY_HISTORY_SERIAL_IO_BYTES];

static void play_history_reset_cache_locked(void);
static int play_history_find_free_album_slot_locked(void);
static int play_history_find_free_track_slot_locked(void);

static esp_err_t play_history_allocate_cache(void)
{
    if (s_cache_ptr)
    {
        return ESP_OK;
    }

    s_cache_ptr = heap_caps_calloc(1,
                                   sizeof(*s_cache_ptr),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cache_ptr)
    {
        ESP_LOGE(TAG,
                 "failed to allocate play history cache in PSRAM (%u bytes)",
                 (unsigned)sizeof(*s_cache_ptr));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "allocated play history cache in PSRAM (%u bytes)",
             (unsigned)sizeof(*s_cache_ptr));
    return ESP_OK;
}

static size_t play_history_serialized_size_locked(void)
{
    return sizeof(play_history_file_header_t) +
           ((size_t)s_cache.album_count * sizeof(play_history_disk_album_t)) +
           ((size_t)s_cache.track_count * sizeof(play_history_disk_track_t));
}

static size_t play_history_serialized_size_for_counts(uint32_t album_count, uint32_t track_count)
{
    return sizeof(play_history_file_header_t) +
           ((size_t)album_count * sizeof(play_history_disk_album_t)) +
           ((size_t)track_count * sizeof(play_history_disk_track_t));
}

static bool play_history_is_compactable_event(play_history_event_type_t type)
{
    return type == PLAY_HISTORY_EVENT_SYNC_CURRENT_CARTRIDGE ||
           type == PLAY_HISTORY_EVENT_CLEAR ||
           type == PLAY_HISTORY_EVENT_REBUILD;
}

static bool play_history_events_equal(const play_history_event_t *lhs, const play_history_event_t *rhs)
{
    if (!lhs || !rhs || lhs->type != rhs->type)
    {
        return false;
    }

    if (lhs->type == PLAY_HISTORY_EVENT_COUNTABLE_PLAY)
    {
        return memcmp(&lhs->data.track_event, &rhs->data.track_event, sizeof(lhs->data.track_event)) == 0;
    }

    return true;
}

static esp_err_t play_history_build_serialized_blob_locked(uint8_t **out_buffer, size_t *out_size)
{
    const size_t total_size = play_history_serialized_size_locked();
    const size_t album_disk_size = sizeof(play_history_disk_album_t);
    const size_t track_disk_size = sizeof(play_history_disk_track_t);
    uint8_t *cursor;
    play_history_file_header_t header = {
        .version = PLAY_HISTORY_STATE_VERSION,
        .next_sequence = s_cache.next_sequence,
        .album_count = s_cache.album_count,
        .track_count = s_cache.track_count,
    };

    if (!out_buffer || !out_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_buffer = NULL;
    *out_size = 0;

    if (total_size > PLAY_HISTORY_MAX_SERIALIZED_BYTES)
    {
        ESP_LOGE(TAG,
                 "serialized history buffer exceeds static PSRAM capacity (%u bytes)",
                 (unsigned)total_size);
        return ESP_ERR_INVALID_SIZE;
    }

    cursor = s_serialized_buf;
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    for (size_t index = 0; index < PLAY_HISTORY_MAX_ALBUMS; index++)
    {
        if (!s_cache.albums[index].in_use)
        {
            continue;
        }

        memcpy(cursor, &s_cache.albums[index].checksum, album_disk_size);
        cursor += album_disk_size;
    }

    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (!s_cache.tracks[index].in_use)
        {
            continue;
        }

        memcpy(cursor, &s_cache.tracks[index].cartridge_checksum, track_disk_size);
        cursor += track_disk_size;
    }

    if ((size_t)(cursor - s_serialized_buf) != total_size)
    {
        ESP_LOGE(TAG, "history cache serialization size mismatch");
        return ESP_FAIL;
    }

    *out_buffer = s_serialized_buf;
    *out_size = total_size;
    return ESP_OK;
}

static uint32_t play_history_next_sequence_locked(void)
{
    uint32_t sequence = s_cache.next_sequence;

    if (sequence == 0)
    {
        sequence = 1;
    }

    s_cache.next_sequence = sequence + 1;
    if (s_cache.next_sequence == 0)
    {
        s_cache.next_sequence = 1;
    }

    return sequence;
}

static void play_history_reset_cache_locked(void)
{
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.version = PLAY_HISTORY_STATE_VERSION;
    s_cache.next_sequence = 1;
}

static esp_err_t play_history_load_from_serialized_bytes_locked(const uint8_t *buffer,
                                                                size_t buffer_size)
{
    const size_t album_disk_size = sizeof(play_history_disk_album_t);
    const size_t track_disk_size = sizeof(play_history_disk_track_t);
    const uint8_t *cursor;
    const uint8_t *end;
    play_history_file_header_t header;

    play_history_reset_cache_locked();

    if (!buffer || buffer_size < sizeof(header))
    {
        return ESP_FAIL;
    }

    memcpy(&header, buffer, sizeof(header));
    if (header.version != PLAY_HISTORY_STATE_VERSION ||
        header.album_count > PLAY_HISTORY_MAX_ALBUMS ||
        header.track_count > PLAY_HISTORY_MAX_TRACKS)
    {
        ESP_LOGW(TAG,
                 "discarding history cache with version=%lu album_count=%lu track_count=%lu",
                 (unsigned long)header.version,
                 (unsigned long)header.album_count,
                 (unsigned long)header.track_count);
        play_history_reset_cache_locked();
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (buffer_size != play_history_serialized_size_for_counts(header.album_count, header.track_count))
    {
        ESP_LOGW(TAG,
                 "discarding history cache with unexpected serialized size=%u for album_count=%lu track_count=%lu",
                 (unsigned)buffer_size,
                 (unsigned long)header.album_count,
                 (unsigned long)header.track_count);
        play_history_reset_cache_locked();
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_cache.version = header.version;
    s_cache.next_sequence = header.next_sequence == 0 ? 1 : header.next_sequence;
    cursor = buffer + sizeof(header);
    end = buffer + buffer_size;

    for (uint32_t index = 0; index < header.album_count; index++)
    {
        int album_slot;

        if ((size_t)(end - cursor) < album_disk_size)
        {
            play_history_reset_cache_locked();
            ESP_LOGW(TAG, "history cache album decode failed; starting with empty cache");
            return ESP_FAIL;
        }

        album_slot = play_history_find_free_album_slot_locked();
        if (album_slot < 0)
        {
            play_history_reset_cache_locked();
            return ESP_ERR_NO_MEM;
        }

        memset(&s_cache.albums[album_slot], 0, sizeof(s_cache.albums[album_slot]));
        s_cache.albums[album_slot].in_use = 1;
        memcpy(&s_cache.albums[album_slot].checksum, cursor, album_disk_size);
        cursor += album_disk_size;
        s_cache.album_count++;
    }

    for (uint32_t index = 0; index < header.track_count; index++)
    {
        int track_slot;

        if ((size_t)(end - cursor) < track_disk_size)
        {
            play_history_reset_cache_locked();
            ESP_LOGW(TAG, "history cache track decode failed; starting with empty cache");
            return ESP_FAIL;
        }

        track_slot = play_history_find_free_track_slot_locked();
        if (track_slot < 0)
        {
            play_history_reset_cache_locked();
            return ESP_ERR_NO_MEM;
        }

        memset(&s_cache.tracks[track_slot], 0, sizeof(s_cache.tracks[track_slot]));
        s_cache.tracks[track_slot].in_use = 1;
        memcpy(&s_cache.tracks[track_slot].cartridge_checksum, cursor, track_disk_size);
        cursor += track_disk_size;
        s_cache.track_count++;
    }

    if (cursor != end)
    {
        play_history_reset_cache_locked();
        ESP_LOGW(TAG, "history cache decode left trailing bytes; starting with empty cache");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (header.next_sequence == 0)
    {
        s_cache.next_sequence = 1;
        s_dirty = true;
    }

    return ESP_OK;
}

static int play_history_find_album_slot_locked(uint32_t checksum)
{
    for (size_t index = 0; index < PLAY_HISTORY_MAX_ALBUMS; index++)
    {
        if (s_cache.albums[index].in_use && s_cache.albums[index].checksum == checksum)
        {
            return (int)index;
        }
    }

    return -1;
}

static int play_history_find_free_album_slot_locked(void)
{
    for (size_t index = 0; index < PLAY_HISTORY_MAX_ALBUMS; index++)
    {
        if (!s_cache.albums[index].in_use)
        {
            return (int)index;
        }
    }

    return -1;
}

static int play_history_find_track_slot_locked(uint32_t checksum, uint32_t track_index)
{
    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (s_cache.tracks[index].in_use &&
            s_cache.tracks[index].cartridge_checksum == checksum &&
            s_cache.tracks[index].track_index == track_index)
        {
            return (int)index;
        }
    }

    return -1;
}

static int play_history_find_free_track_slot_locked(void)
{
    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (!s_cache.tracks[index].in_use)
        {
            return (int)index;
        }
    }

    return -1;
}

static int play_history_find_oldest_album_slot_locked(uint32_t preserve_checksum, bool preserve_current)
{
    int oldest_slot = -1;
    uint32_t oldest_sequence = UINT32_MAX;

    for (size_t index = 0; index < PLAY_HISTORY_MAX_ALBUMS; index++)
    {
        if (!s_cache.albums[index].in_use)
        {
            continue;
        }
        if (preserve_current && s_cache.albums[index].checksum == preserve_checksum)
        {
            continue;
        }
        if (s_cache.albums[index].first_seen_sequence < oldest_sequence)
        {
            oldest_sequence = s_cache.albums[index].first_seen_sequence;
            oldest_slot = (int)index;
        }
    }

    return oldest_slot;
}

static void play_history_remove_album_locked(size_t album_slot)
{
    uint32_t checksum;

    if (album_slot >= PLAY_HISTORY_MAX_ALBUMS || !s_cache.albums[album_slot].in_use)
    {
        return;
    }

    checksum = s_cache.albums[album_slot].checksum;
    memset(&s_cache.albums[album_slot], 0, sizeof(s_cache.albums[album_slot]));
    if (s_cache.album_count > 0)
    {
        s_cache.album_count--;
    }

    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (!s_cache.tracks[index].in_use || s_cache.tracks[index].cartridge_checksum != checksum)
        {
            continue;
        }

        memset(&s_cache.tracks[index], 0, sizeof(s_cache.tracks[index]));
        if (s_cache.track_count > 0)
        {
            s_cache.track_count--;
        }
    }
}

static size_t play_history_count_missing_tracks_for_current_cartridge_locked(uint32_t checksum)
{
    size_t missing_tracks = 0;
    size_t track_count = cartridge_service_get_metadata_track_count();

    for (size_t index = 0; index < track_count; index++)
    {
        if (!cartridge_service_get_metadata_track(index))
        {
            continue;
        }
        if (play_history_find_track_slot_locked(checksum, (uint32_t)index) < 0)
        {
            missing_tracks++;
        }
    }

    return missing_tracks;
}

static esp_err_t play_history_ensure_capacity_locked(uint32_t preserve_checksum,
                                                     bool preserve_current,
                                                     size_t album_delta,
                                                     size_t track_delta)
{
    while ((s_cache.album_count + album_delta > PLAY_HISTORY_MAX_ALBUMS) ||
           (s_cache.track_count + track_delta > PLAY_HISTORY_MAX_TRACKS))
    {
        int prune_slot = play_history_find_oldest_album_slot_locked(preserve_checksum, preserve_current);
        if (prune_slot < 0 && preserve_current)
        {
            prune_slot = play_history_find_oldest_album_slot_locked(0, false);
        }
        if (prune_slot < 0)
        {
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGW(TAG,
                 "pruning album checksum 0x%08lx to stay within history caps",
                 (unsigned long)s_cache.albums[prune_slot].checksum);
        play_history_remove_album_locked((size_t)prune_slot);
    }

    return ESP_OK;
}

static esp_err_t play_history_sync_current_cartridge_locked(void)
{
    const jukeboy_jbm_header_t *metadata;
    uint32_t sequence;
    uint32_t checksum;
    int album_slot;
    size_t album_delta;
    size_t track_delta;
    esp_err_t err;

    if (!cartridge_service_is_mounted())
    {
        return ESP_ERR_INVALID_STATE;
    }

    metadata = cartridge_service_get_metadata_header();
    if (!metadata)
    {
        return ESP_ERR_NOT_FOUND;
    }

    checksum = metadata->checksum;
    album_slot = play_history_find_album_slot_locked(checksum);
    album_delta = (album_slot < 0) ? 1U : 0U;
    track_delta = play_history_count_missing_tracks_for_current_cartridge_locked(checksum);

    err = play_history_ensure_capacity_locked(checksum, true, album_delta, track_delta);
    if (err != ESP_OK)
    {
        return err;
    }

    if (album_slot < 0)
    {
        album_slot = play_history_find_free_album_slot_locked();
        if (album_slot < 0)
        {
            return ESP_ERR_NO_MEM;
        }

        memset(&s_cache.albums[album_slot], 0, sizeof(s_cache.albums[album_slot]));
        s_cache.albums[album_slot].in_use = 1;
        s_cache.albums[album_slot].checksum = checksum;
        s_cache.albums[album_slot].first_seen_sequence = play_history_next_sequence_locked();
        s_cache.album_count++;
    }

    sequence = play_history_next_sequence_locked();
    s_cache.albums[album_slot].track_count = metadata->track_count;
    s_cache.albums[album_slot].last_seen_sequence = sequence;
    memcpy(&s_cache.albums[album_slot].metadata, metadata, sizeof(s_cache.albums[album_slot].metadata));

    for (size_t index = 0; index < cartridge_service_get_metadata_track_count(); index++)
    {
        const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
        int track_slot;

        if (!track)
        {
            continue;
        }

        track_slot = play_history_find_track_slot_locked(checksum, (uint32_t)index);
        if (track_slot < 0)
        {
            track_slot = play_history_find_free_track_slot_locked();
            if (track_slot < 0)
            {
                return ESP_ERR_NO_MEM;
            }

            memset(&s_cache.tracks[track_slot], 0, sizeof(s_cache.tracks[track_slot]));
            s_cache.tracks[track_slot].in_use = 1;
            s_cache.tracks[track_slot].cartridge_checksum = checksum;
            s_cache.tracks[track_slot].track_index = (uint32_t)index;
            s_cache.tracks[track_slot].first_seen_sequence = play_history_next_sequence_locked();
            s_cache.track_count++;
        }

        s_cache.tracks[track_slot].track_file_num = track->file_num;
        s_cache.tracks[track_slot].last_seen_sequence = sequence;
        memcpy(&s_cache.tracks[track_slot].metadata, track, sizeof(s_cache.tracks[track_slot].metadata));
    }

    s_dirty = true;
    return ESP_OK;
}

static esp_err_t play_history_apply_countable_play_locked(const player_service_track_event_t *track_event)
{
    int track_slot;
    int album_slot;
    uint32_t sequence;

    if (!track_event)
    {
        return ESP_ERR_INVALID_ARG;
    }

    track_slot = play_history_find_track_slot_locked(track_event->cartridge_checksum,
                                                     track_event->track_index);
    if (track_slot < 0)
    {
        const jukeboy_jbm_header_t *metadata = cartridge_service_get_metadata_header();
        if (metadata && metadata->checksum == track_event->cartridge_checksum)
        {
            esp_err_t err = play_history_sync_current_cartridge_locked();
            if (err != ESP_OK)
            {
                return err;
            }
            track_slot = play_history_find_track_slot_locked(track_event->cartridge_checksum,
                                                             track_event->track_index);
        }
    }

    if (track_slot < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    sequence = play_history_next_sequence_locked();
    s_cache.tracks[track_slot].play_count++;
    s_cache.tracks[track_slot].track_file_num = track_event->track_file_num;
    s_cache.tracks[track_slot].last_seen_sequence = sequence;

    album_slot = play_history_find_album_slot_locked(track_event->cartridge_checksum);
    if (album_slot >= 0)
    {
        s_cache.albums[album_slot].last_seen_sequence = sequence;
    }

    s_dirty = true;
    return ESP_OK;
}

static bool play_history_queue_event(const play_history_event_t *event)
{
    bool queued = false;

    taskENTER_CRITICAL(&s_event_lock);
    if (s_event_count < PLAY_HISTORY_EVENT_QUEUE_DEPTH)
    {
        if (play_history_is_compactable_event(event->type))
        {
            for (size_t index = 0; index < s_event_count; index++)
            {
                size_t slot = (s_event_head + index) % PLAY_HISTORY_EVENT_QUEUE_DEPTH;
                if (play_history_events_equal(&s_event_queue[slot], event))
                {
                    queued = true;
                    break;
                }
            }
        }

        if (!queued)
        {
            size_t tail = (s_event_head + s_event_count) % PLAY_HISTORY_EVENT_QUEUE_DEPTH;
            s_event_queue[tail] = *event;
            s_event_count++;
            queued = true;
        }
        s_event_overflow_logged = false;
    }
    taskEXIT_CRITICAL(&s_event_lock);

    if (!queued && !s_event_overflow_logged)
    {
        s_event_overflow_logged = true;
        ESP_LOGW(TAG, "history event inbox overflow; dropping event type %d", (int)event->type);
    }

    return queued;
}

static bool play_history_dequeue_event(play_history_event_t *event)
{
    bool dequeued = false;

    taskENTER_CRITICAL(&s_event_lock);
    if (s_event_count > 0)
    {
        *event = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % PLAY_HISTORY_EVENT_QUEUE_DEPTH;
        s_event_count--;
        dequeued = true;
    }
    taskEXIT_CRITICAL(&s_event_lock);

    return dequeued;
}

static void play_history_on_cartridge_event(void *handler_arg,
                                            esp_event_base_t event_base,
                                            int32_t event_id,
                                            void *event_data)
{
    play_history_event_t event = {
        .type = PLAY_HISTORY_EVENT_SYNC_CURRENT_CARTRIDGE,
    };

    (void)handler_arg;
    (void)event_base;
    (void)event_data;

    if (event_id == CARTRIDGE_SVC_EVENT_INSERTED || event_id == CARTRIDGE_SVC_EVENT_MOUNTED)
    {
        (void)play_history_queue_event(&event);
    }
}

static void play_history_on_player_event(void *handler_arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void *event_data)
{
    play_history_event_t event;

    (void)handler_arg;
    (void)event_base;

    if (event_id != PLAYER_SVC_EVENT_TRACK_BECAME_COUNTABLE || !event_data)
    {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = PLAY_HISTORY_EVENT_COUNTABLE_PLAY;
    memcpy(&event.data.track_event, event_data, sizeof(event.data.track_event));
    (void)play_history_queue_event(&event);
}

static esp_err_t play_history_load_locked(void)
{
    FILE *file = fopen(PLAY_HISTORY_FILE_PATH, "rb");
    uint8_t *file_data = s_serialized_buf;
    long file_size_long;
    size_t file_size;
    esp_err_t err;

    play_history_reset_cache_locked();
    s_dirty = false;

    if (!file)
    {
        if (errno == ENOENT)
        {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "failed to open history cache for read: errno=%d", errno);
        return ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        ESP_LOGW(TAG, "history cache size check failed; starting with empty cache");
        return ESP_FAIL;
    }

    file_size_long = ftell(file);
    if (file_size_long < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        ESP_LOGW(TAG, "history cache size readback failed; starting with empty cache");
        return ESP_FAIL;
    }

    file_size = (size_t)file_size_long;
    if (file_size == 0)
    {
        fclose(file);
        return ESP_OK;
    }

    if (file_size > PLAY_HISTORY_MAX_SERIALIZED_BYTES)
    {
        fclose(file);
        ESP_LOGE(TAG,
                 "history file exceeds static PSRAM read capacity (%u bytes)",
                 (unsigned)file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fread(file_data, 1, file_size, file) != file_size)
    {
        fclose(file);
        ESP_LOGW(TAG, "history cache file read failed; starting with empty cache");
        return ESP_FAIL;
    }

    fclose(file);
    file = NULL;

    err = play_history_load_from_serialized_bytes_locked(file_data, file_size);
    return err;
}

static esp_err_t play_history_flush_locked(void)
{
    FILE *file = NULL;
    uint8_t *file_data = NULL;
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    size_t desired_size;
    long existing_size = 0;
    esp_err_t info_err;
    esp_err_t err;

    if (!s_dirty)
    {
        return ESP_OK;
    }

    s_cache.version = PLAY_HISTORY_STATE_VERSION;
    err = play_history_build_serialized_blob_locked(&file_data, &desired_size);
    if (err != ESP_OK)
    {
        return err;
    }

    info_err = esp_littlefs_info(APP_LITTLEFS_PARTITION_LABEL, &total_bytes, &used_bytes);
    if (info_err == ESP_OK)
    {
        file = fopen(PLAY_HISTORY_FILE_PATH, "rb");
        if (file)
        {
            if (fseek(file, 0, SEEK_END) == 0)
            {
                existing_size = ftell(file);
            }
            fclose(file);
            file = NULL;
        }

        if (desired_size > total_bytes ||
            desired_size > (total_bytes - used_bytes + (existing_size > 0 ? (size_t)existing_size : 0)))
        {
            ESP_LOGE(TAG,
                     "history cache commit needs %u bytes but littlefs cannot currently provide enough space",
                     (unsigned)desired_size);
            return ESP_ERR_NO_MEM;
        }
    }

    file = fopen(PLAY_HISTORY_FILE_PATH, "rb+");
    if (!file)
    {
        file = fopen(PLAY_HISTORY_FILE_PATH, "wb+");
    }
    if (!file)
    {
        ESP_LOGE(TAG, "failed to open history cache file for commit: errno=%d", errno);
        return ESP_FAIL;
    }

    if (ftruncate(fileno(file), (off_t)desired_size) != 0)
    {
        fclose(file);
        ESP_LOGE(TAG, "failed to resize history cache file: errno=%d", errno);
        return ESP_FAIL;
    }

    for (size_t pass = 0; pass < PLAY_HISTORY_COMMIT_MAX_PASSES; pass++)
    {
        bool identical = true;

        for (size_t offset = 0; offset < desired_size; offset += PLAY_HISTORY_SERIAL_IO_BYTES)
        {
            const size_t expected_len = ((desired_size - offset) < PLAY_HISTORY_SERIAL_IO_BYTES)
                                            ? (desired_size - offset)
                                            : PLAY_HISTORY_SERIAL_IO_BYTES;
            size_t actual_len = expected_len;

            if (fseek(file, (long)offset, SEEK_SET) != 0)
            {
                fclose(file);
                ESP_LOGE(TAG, "failed to seek history cache file for readback: errno=%d", errno);
                return ESP_FAIL;
            }

            memset(s_commit_actual_buf, 0, expected_len);
            if (fread(s_commit_actual_buf, 1, expected_len, file) != expected_len)
            {
                clearerr(file);
                actual_len = 0;
            }

            if (actual_len != expected_len || memcmp(file_data + offset, s_commit_actual_buf, expected_len) != 0)
            {
                identical = false;
                if (fseek(file, (long)offset, SEEK_SET) != 0)
                {
                    fclose(file);
                    ESP_LOGE(TAG, "failed to seek history cache file for rewrite: errno=%d", errno);
                    return ESP_FAIL;
                }
                if (fwrite(file_data + offset, 1, expected_len, file) != expected_len)
                {
                    fclose(file);
                    ESP_LOGE(TAG, "failed to rewrite history cache bytes: errno=%d", errno);
                    return ESP_FAIL;
                }
            }
        }

        if (fflush(file) != 0)
        {
            fclose(file);
            ESP_LOGE(TAG, "failed to flush history cache file: errno=%d", errno);
            return ESP_FAIL;
        }

        if (identical)
        {
            if (fclose(file) != 0)
            {
                ESP_LOGE(TAG, "failed to close history cache file after verify: errno=%d", errno);
                return ESP_FAIL;
            }
            s_dirty = false;
            return ESP_OK;
        }
    }

    fclose(file);
    ESP_LOGE(TAG, "history cache file failed verification after rewrite passes");
    return ESP_FAIL;
}

static esp_err_t play_history_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return play_history_service_commit();
}

static esp_err_t play_history_run_commit_now(void)
{
    esp_err_t err;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    err = play_history_flush_locked();
    xSemaphoreGive(s_cache_mutex);
    return err;
}

static bool play_history_begin_requested_commit(void)
{
    bool should_commit = false;

    taskENTER_CRITICAL(&s_commit_state_lock);
    if (s_commit_requested && !s_commit_in_progress)
    {
        s_commit_requested = false;
        s_commit_in_progress = true;
        should_commit = true;
    }
    taskEXIT_CRITICAL(&s_commit_state_lock);

    return should_commit;
}

static void play_history_finish_commit(esp_err_t err)
{
    taskENTER_CRITICAL(&s_commit_state_lock);
    s_commit_result = err;
    s_commit_in_progress = false;
    taskEXIT_CRITICAL(&s_commit_state_lock);
}

esp_err_t play_history_service_init(void)
{
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    if (!s_cache_mutex)
    {
        s_cache_mutex = xSemaphoreCreateMutexStatic(&s_cache_mutex_storage);
        if (!s_cache_mutex)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    err = play_history_allocate_cache();
    if (err != ESP_OK)
    {
        return err;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    err = play_history_load_locked();
    xSemaphoreGive(s_cache_mutex);
    if (err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE)
    {
        ESP_LOGW(TAG, "history cache load failed: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_event_handler_register(CARTRIDGE_SERVICE_EVENT,
                                               CARTRIDGE_SVC_EVENT_INSERTED,
                                               play_history_on_cartridge_event,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(CARTRIDGE_SERVICE_EVENT,
                                               CARTRIDGE_SVC_EVENT_MOUNTED,
                                               play_history_on_cartridge_event,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PLAYER_SERVICE_EVENT,
                                               PLAYER_SVC_EVENT_TRACK_BECAME_COUNTABLE,
                                               play_history_on_player_event,
                                               NULL));
    ESP_ERROR_CHECK(power_mgmt_service_register_shutdown_callback(play_history_shutdown_callback,
                                                                  NULL,
                                                                  PLAY_HISTORY_SHUTDOWN_PRIORITY));

    s_initialised = true;

    if (cartridge_service_is_mounted() || cartridge_service_is_inserted())
    {
        play_history_event_t event = {
            .type = PLAY_HISTORY_EVENT_SYNC_CURRENT_CARTRIDGE,
        };
        (void)play_history_queue_event(&event);
    }

    return ESP_OK;
}

void play_history_service_process_once(void)
{
    play_history_event_t event;
    esp_err_t err;

    s_process_task_handle = xTaskGetCurrentTaskHandle();

    if (!s_initialised || !s_cache_mutex)
    {
        return;
    }

    if (play_history_begin_requested_commit())
    {
        err = play_history_run_commit_now();
        play_history_finish_commit(err);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "history commit failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (s_commit_in_progress)
    {
        return;
    }

    for (size_t index = 0; index < PLAY_HISTORY_EVENTS_PER_PASS; index++)
    {
        err = ESP_OK;

        if (!play_history_dequeue_event(&event))
        {
            break;
        }

        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        switch (event.type)
        {
        case PLAY_HISTORY_EVENT_SYNC_CURRENT_CARTRIDGE:
            err = play_history_sync_current_cartridge_locked();
            break;
        case PLAY_HISTORY_EVENT_COUNTABLE_PLAY:
            err = play_history_apply_countable_play_locked(&event.data.track_event);
            break;
        case PLAY_HISTORY_EVENT_CLEAR:
            play_history_reset_cache_locked();
            s_dirty = true;
            break;
        case PLAY_HISTORY_EVENT_REBUILD:
            play_history_reset_cache_locked();
            s_dirty = true;
            err = play_history_sync_current_cartridge_locked();
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        xSemaphoreGive(s_cache_mutex);

        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGW(TAG, "history event %d failed: %s", (int)event.type, esp_err_to_name(err));
        }
    }
}

esp_err_t play_history_service_flush(void)
{
    return play_history_service_commit();
}

esp_err_t play_history_service_commit(void)
{
    esp_err_t err;
    bool done;
    TickType_t wait_ticks = pdMS_TO_TICKS(1);

    if (!s_initialised || !s_cache_mutex)
    {
        return ESP_OK;
    }

    if (wait_ticks == 0)
    {
        wait_ticks = 1;
    }

    if (xTaskGetCurrentTaskHandle() == s_process_task_handle)
    {
        taskENTER_CRITICAL(&s_commit_state_lock);
        if (s_commit_in_progress)
        {
            taskEXIT_CRITICAL(&s_commit_state_lock);
            return ESP_ERR_INVALID_STATE;
        }
        s_commit_in_progress = true;
        taskEXIT_CRITICAL(&s_commit_state_lock);

        err = play_history_run_commit_now();
        play_history_finish_commit(err);
        return err;
    }

    taskENTER_CRITICAL(&s_commit_state_lock);
    if (s_commit_requested || s_commit_in_progress)
    {
        taskEXIT_CRITICAL(&s_commit_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_commit_requested = true;
    s_commit_result = ESP_ERR_INVALID_STATE;
    taskEXIT_CRITICAL(&s_commit_state_lock);

    for (;;)
    {
        taskENTER_CRITICAL(&s_commit_state_lock);
        done = !s_commit_requested && !s_commit_in_progress;
        err = s_commit_result;
        taskEXIT_CRITICAL(&s_commit_state_lock);

        if (done)
        {
            return err;
        }

        vTaskDelay(wait_ticks);
    }
}

bool play_history_service_is_commit_in_progress(void)
{
    bool in_progress;

    taskENTER_CRITICAL(&s_commit_state_lock);
    in_progress = s_commit_requested || s_commit_in_progress;
    taskEXIT_CRITICAL(&s_commit_state_lock);
    return in_progress;
}

bool play_history_service_is_dirty(void)
{
    bool dirty;

    if (!s_initialised || !s_cache_mutex)
    {
        return false;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    dirty = s_dirty;
    xSemaphoreGive(s_cache_mutex);
    return dirty;
}

size_t play_history_service_get_album_count(void)
{
    size_t album_count = 0;

    if (!s_initialised || !s_cache_mutex)
    {
        return 0;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    album_count = s_cache.album_count;
    xSemaphoreGive(s_cache_mutex);
    return album_count;
}

size_t play_history_service_get_track_count(void)
{
    size_t track_count = 0;

    if (!s_initialised || !s_cache_mutex)
    {
        return 0;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    track_count = s_cache.track_count;
    xSemaphoreGive(s_cache_mutex);
    return track_count;
}

bool play_history_service_get_album_record(size_t slot, play_history_album_record_t *out_record)
{
    size_t ordinal = 0;
    bool found = false;

    if (!out_record || !s_initialised || !s_cache_mutex)
    {
        return false;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (size_t index = 0; index < PLAY_HISTORY_MAX_ALBUMS; index++)
    {
        if (!s_cache.albums[index].in_use)
        {
            continue;
        }
        if (ordinal++ != slot)
        {
            continue;
        }

        out_record->checksum = s_cache.albums[index].checksum;
        out_record->track_count = s_cache.albums[index].track_count;
        out_record->first_seen_sequence = s_cache.albums[index].first_seen_sequence;
        out_record->last_seen_sequence = s_cache.albums[index].last_seen_sequence;
        memcpy(&out_record->metadata, &s_cache.albums[index].metadata, sizeof(out_record->metadata));
        found = true;
        break;
    }
    xSemaphoreGive(s_cache_mutex);
    return found;
}

bool play_history_service_get_album_record_by_checksum(uint32_t checksum, play_history_album_record_t *out_record)
{
    int album_slot;

    if (!out_record || !s_initialised || !s_cache_mutex)
    {
        return false;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    album_slot = play_history_find_album_slot_locked(checksum);
    if (album_slot >= 0)
    {
        out_record->checksum = s_cache.albums[album_slot].checksum;
        out_record->track_count = s_cache.albums[album_slot].track_count;
        out_record->first_seen_sequence = s_cache.albums[album_slot].first_seen_sequence;
        out_record->last_seen_sequence = s_cache.albums[album_slot].last_seen_sequence;
        memcpy(&out_record->metadata, &s_cache.albums[album_slot].metadata, sizeof(out_record->metadata));
    }
    xSemaphoreGive(s_cache_mutex);
    return album_slot >= 0;
}

size_t play_history_service_get_album_track_count(uint32_t checksum)
{
    size_t track_count = 0;

    if (!s_initialised || !s_cache_mutex)
    {
        return 0;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (s_cache.tracks[index].in_use && s_cache.tracks[index].cartridge_checksum == checksum)
        {
            track_count++;
        }
    }
    xSemaphoreGive(s_cache_mutex);
    return track_count;
}

bool play_history_service_get_album_track_record(uint32_t checksum,
                                                 size_t slot,
                                                 play_history_track_record_t *out_record)
{
    size_t ordinal = 0;
    bool found = false;

    if (!out_record || !s_initialised || !s_cache_mutex)
    {
        return false;
    }

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (size_t index = 0; index < PLAY_HISTORY_MAX_TRACKS; index++)
    {
        if (!s_cache.tracks[index].in_use || s_cache.tracks[index].cartridge_checksum != checksum)
        {
            continue;
        }
        if (ordinal++ != slot)
        {
            continue;
        }

        out_record->cartridge_checksum = s_cache.tracks[index].cartridge_checksum;
        out_record->track_index = s_cache.tracks[index].track_index;
        out_record->track_file_num = s_cache.tracks[index].track_file_num;
        out_record->play_count = s_cache.tracks[index].play_count;
        out_record->first_seen_sequence = s_cache.tracks[index].first_seen_sequence;
        out_record->last_seen_sequence = s_cache.tracks[index].last_seen_sequence;
        memcpy(&out_record->metadata, &s_cache.tracks[index].metadata, sizeof(out_record->metadata));
        found = true;
        break;
    }
    xSemaphoreGive(s_cache_mutex);
    return found;
}

esp_err_t play_history_service_request_clear(void)
{
    play_history_event_t event = {
        .type = PLAY_HISTORY_EVENT_CLEAR,
    };

    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return play_history_queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t play_history_service_request_rebuild(void)
{
    play_history_event_t event = {
        .type = PLAY_HISTORY_EVENT_REBUILD,
    };

    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return play_history_queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}