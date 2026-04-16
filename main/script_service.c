#include "script_service.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cartridge_service.h"
#include "player_service.h"
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "storage_paths.h"
#include "wifi_service.h"
#include "wasm_export.h"
#include "wm_wamr.h"

#define SCRIPT_SERVICE_RUNNER_STACK_SIZE (16 * 1024)
#define SCRIPT_SERVICE_WASM_STACK_SIZE (32 * 1024)
#define SCRIPT_SERVICE_WASM_HEAP_SIZE (32 * 1024)

#define SCRIPT_SD_ROOT_PATH "/sdcard/scripts"
#define SCRIPT_STDIO_CAPTURE_PATH RAMDISK_SERVICE_MOUNT_PATH "/script-run-output.txt"

typedef struct
{
    const char *label;
    const char *path;
    bool ensure_on_init;
} script_root_t;

typedef struct
{
    char requested_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    char **argv;
    int argc;
    uint8_t *script_buffer;
    uint32_t script_buffer_size;
    esp_err_t err;
    SemaphoreHandle_t completion_sem;
    StaticSemaphore_t completion_sem_storage;
    script_service_run_result_t result;
} script_run_context_t;

static const char *TAG = "script_svc";

static const script_root_t s_script_roots[] = {
    {"lfs", APP_LITTLEFS_MOUNT_PATH "/scripts", true},
};

static script_service_status_t s_status = SCRIPT_SERVICE_STATUS_UNINITIALIZED;
static StaticSemaphore_t s_runtime_mutex_storage;
static SemaphoreHandle_t s_runtime_mutex;
static StaticSemaphore_t s_worker_request_sem_storage;
static SemaphoreHandle_t s_worker_request_sem;
static bool s_natives_registered;
static script_run_context_t *s_active_run_context;
static script_run_context_t *s_pending_context;
static pthread_t s_worker_thread;
static bool s_worker_thread_started;

static const char *s_wasi_addr_pool[] = {
    "0.0.0.0/0",
    "::/0",
};

static const char *s_wasi_ns_lookup_pool[] = {
    "*",
};

static void script_set_result_message(script_service_run_result_t *result, const char *format, ...)
{
    va_list args;

    if (!result || !format)
    {
        return;
    }

    va_start(args, format);
    vsnprintf(result->message, sizeof(result->message), format, args);
    va_end(args);
}

static void script_append_result_output(script_service_run_result_t *result, const char *data, size_t data_len)
{
    size_t current_len;
    size_t copy_len;

    if (!result || !data || data_len == 0)
    {
        return;
    }

    current_len = strnlen(result->output, sizeof(result->output));
    if (current_len >= sizeof(result->output) - 1)
    {
        return;
    }

    copy_len = data_len;
    if (copy_len > sizeof(result->output) - 1 - current_len)
    {
        copy_len = sizeof(result->output) - 1 - current_len;
    }

    memcpy(result->output + current_len, data, copy_len);
    result->output[current_len + copy_len] = '\0';
}

static void script_append_output(script_service_run_result_t *result, const char *text)
{
    if (!text)
    {
        return;
    }

    script_append_result_output(result, text, strlen(text));
}

static void script_capture_output_file(int capture_fd, script_service_run_result_t *result)
{
    char buffer[128];
    ssize_t bytes_read;

    if (capture_fd < 0 || !result)
    {
        return;
    }

    if (lseek(capture_fd, 0, SEEK_SET) < 0)
    {
        return;
    }

    while ((bytes_read = read(capture_fd, buffer, sizeof(buffer))) > 0)
    {
        script_append_result_output(result, buffer, (size_t)bytes_read);
        if (result->output[sizeof(result->output) - 1] != '\0')
        {
            break;
        }
    }
}

static bool script_is_regular_file(const char *path)
{
    struct stat st = {0};

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool script_has_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');

    return dot && (!slash || dot > slash);
}

static bool script_copy_candidate(const char *candidate, char *out_path, size_t out_path_size)
{
    size_t candidate_len;

    if (!script_is_regular_file(candidate))
    {
        return false;
    }

    candidate_len = strlen(candidate);
    if (candidate_len + 1 > out_path_size)
    {
        return false;
    }

    memcpy(out_path, candidate, candidate_len + 1);
    return true;
}

static bool script_try_path_variants(const char *base_path, char *out_path, size_t out_path_size)
{
    char candidate[SCRIPT_SERVICE_MAX_PATH_LEN];
    int written;

    if (script_copy_candidate(base_path, out_path, out_path_size))
    {
        return true;
    }

    if (script_has_extension(base_path))
    {
        return false;
    }

    written = snprintf(candidate, sizeof(candidate), "%s.wasm", base_path);
    if (written > 0 && (size_t)written < sizeof(candidate) &&
        script_copy_candidate(candidate, out_path, out_path_size))
    {
        return true;
    }

#if defined(CONFIG_WAMR_ENABLE_AOT) && CONFIG_WAMR_ENABLE_AOT != 0
    written = snprintf(candidate, sizeof(candidate), "%s.aot", base_path);
    if (written > 0 && (size_t)written < sizeof(candidate) &&
        script_copy_candidate(candidate, out_path, out_path_size))
    {
        return true;
    }
#endif

    return false;
}

static bool script_try_root(const char *root_path, const char *relative_path, char *out_path, size_t out_path_size)
{
    char candidate[SCRIPT_SERVICE_MAX_PATH_LEN];
    int written;

    // Check if relative_path is just the script name (without extension or slash)
    if (!strchr(relative_path, '/'))
    {
        // Try root_path/name/name.wasm
        written = snprintf(candidate, sizeof(candidate), "%s/%s/%s.wasm", root_path, relative_path, relative_path);
        if (written > 0 && (size_t)written < sizeof(candidate) && script_is_regular_file(candidate))
        {
            if ((size_t)written + 1 <= out_path_size)
            {
                memcpy(out_path, candidate, (size_t)written + 1);
                return true;
            }
        }
    }

    written = snprintf(candidate, sizeof(candidate), "%s/%s", root_path, relative_path);
    if (written <= 0 || (size_t)written >= sizeof(candidate))
    {
        return false;
    }

    return script_try_path_variants(candidate, out_path, out_path_size);
}

static bool script_try_labeled_root(const char *path, char *out_path, size_t out_path_size)
{
    const char *separator = strchr(path, '/');

    if (!separator)
    {
        return false;
    }

    size_t label_len = (size_t)(separator - path);
    const char *relative_path = separator + 1;
    size_t root_count = script_service_get_root_count();

    if (*relative_path == '\0')
    {
        return false;
    }

    for (size_t index = 0; index < root_count; index++)
    {
        const char *label = script_service_get_root_label(index);
        if (strlen(label) == label_len && strncmp(path, label, label_len) == 0)
        {
            return script_try_root(script_service_get_root_path(index), relative_path, out_path, out_path_size);
        }
    }

    return false;
}

static void script_ensure_directory(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            ESP_LOGW(TAG, "%s exists but is not a directory", path);
        }
        return;
    }

    if (mkdir(path, 0775) != 0 && errno != EEXIST)
    {
        ESP_LOGW(TAG, "failed to create %s: %s", path, strerror(errno));
    }
}

static char **script_duplicate_argv(const char *program_path,
                                    int argc,
                                    const char *const *argv,
                                    int *argc_out)
{
    size_t total_bytes = 0;
    int total_argc = argc + 1;
    char **argv_copy;
    char *storage;
    const char *effective_program_path = program_path && program_path[0] ? program_path : "script";

    if (argc_out)
    {
        *argc_out = 0;
    }

    total_bytes += strlen(effective_program_path) + 1;

    for (int index = 0; index < argc; index++)
    {
        const char *arg = argv[index] ? argv[index] : "";
        total_bytes += strlen(arg) + 1;
    }

    argv_copy = malloc(sizeof(char *) * (size_t)(total_argc + 1) + total_bytes);
    if (!argv_copy)
    {
        return NULL;
    }

    storage = (char *)(argv_copy + total_argc + 1);
    argv_copy[0] = storage;
    memcpy(storage, effective_program_path, strlen(effective_program_path) + 1);
    storage += strlen(effective_program_path) + 1;

    for (int index = 0; index < argc; index++)
    {
        const char *arg = argv[index] ? argv[index] : "";
        size_t arg_len = strlen(arg) + 1;

        argv_copy[index + 1] = storage;
        memcpy(storage, arg, arg_len);
        storage += arg_len;
    }
    argv_copy[total_argc] = NULL;

    if (argc_out)
    {
        *argc_out = total_argc;
    }

    return argv_copy;
}

static uint8_t *script_load_file(const char *path, uint32_t *size_out, script_service_run_result_t *result)
{
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    long file_size = 0;

    if (!path || !size_out)
    {
        return NULL;
    }

    file = fopen(path, "rb");
    if (!file)
    {
        script_set_result_message(result, "cannot open %s", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        script_set_result_message(result, "cannot seek %s", path);
        return NULL;
    }

    file_size = ftell(file);
    if (file_size <= 0 || (uint64_t)file_size > UINT32_MAX)
    {
        fclose(file);
        script_set_result_message(result, "invalid script size for %s", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        script_set_result_message(result, "cannot rewind %s", path);
        return NULL;
    }

    ESP_LOGI(TAG, "reading %ld bytes from %s", file_size, path);

    buffer = malloc((size_t)file_size);
    if (!buffer)
    {
        fclose(file);
        script_set_result_message(result, "out of memory loading %s", path);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)file_size, file) != (size_t)file_size)
    {
        fclose(file);
        free(buffer);
        script_set_result_message(result, "failed to read %s", path);
        return NULL;
    }

    fclose(file);
    *size_out = (uint32_t)file_size;
    result->script_size_bytes = (uint32_t)file_size;
    return buffer;
}

static int log_wrapper(wasm_exec_env_t exec_env, const char *message)
{
    (void)exec_env;

    if (s_active_run_context)
    {
        script_append_output(&s_active_run_context->result, "[wasm] ");
        script_append_output(&s_active_run_context->result, message ? message : "");
        script_append_output(&s_active_run_context->result, "\n");
    }
    return ESP_OK;
}

static int next_track_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_NEXT);
}

static int previous_track_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_PREVIOUS);
}

static int pause_toggle_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_PAUSE);
}

static int fast_forward_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_FAST_FORWARD);
}

static int rewind_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_FAST_BACKWARD);
}

static int volume_up_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_VOLUME_UP);
}

static int volume_down_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_request_control(PLAYER_SVC_CONTROL_VOLUME_DOWN);
}

static int set_playback_mode_wrapper(wasm_exec_env_t exec_env, int mode)
{
    (void)exec_env;
    return player_service_set_playback_mode((player_service_playback_mode_t)mode);
}

static int get_playback_mode_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)player_service_get_playback_mode();
}

static int is_paused_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return player_service_is_paused() ? 1 : 0;
}

static int get_volume_percent_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)player_service_get_volume_percent();
}

static int set_volume_percent_wrapper(wasm_exec_env_t exec_env, int percent)
{
    (void)exec_env;

    if (percent < 0)
    {
        percent = 0;
    }
    else if (percent > 100)
    {
        percent = 100;
    }

    player_service_set_volume_absolute((uint8_t)((percent * 127 + 50) / 100));
    return ESP_OK;
}

static int sleep_ms_wrapper(wasm_exec_env_t exec_env, int milliseconds)
{
    (void)exec_env;

    if (milliseconds < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    vTaskDelay(pdMS_TO_TICKS((uint32_t)milliseconds));
    return ESP_OK;
}

static int get_track_count_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)cartridge_service_get_metadata_track_count();
}

static int get_track_title_wrapper(wasm_exec_env_t exec_env, int index, char *buf, uint32_t buf_len)
{
    const char *title;
    size_t title_len;

    (void)exec_env;

    if (!buf || buf_len == 0 || index < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    title = cartridge_service_get_track_name((size_t)index);
    if (!title)
    {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    title_len = strlen(title);
    if (title_len >= buf_len)
    {
        title_len = buf_len - 1;
    }

    memcpy(buf, title, title_len);
    buf[title_len] = '\0';
    return ESP_OK;
}

static int wifi_is_connected_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return wifi_service_get_state() == WIFI_SVC_STATE_CONNECTED ? 1 : 0;
}

static int get_free_heap_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int)esp_get_free_heap_size();
}

static int64_t get_uptime_ms_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int64_t)(esp_timer_get_time() / 1000);
}

static NativeSymbol s_jukeboy_symbols[] = {
    {"log", log_wrapper, "($)i", NULL},
    {"next_track", next_track_wrapper, "()i", NULL},
    {"previous_track", previous_track_wrapper, "()i", NULL},
    {"pause_toggle", pause_toggle_wrapper, "()i", NULL},
    {"fast_forward", fast_forward_wrapper, "()i", NULL},
    {"rewind", rewind_wrapper, "()i", NULL},
    {"volume_up", volume_up_wrapper, "()i", NULL},
    {"volume_down", volume_down_wrapper, "()i", NULL},
    {"set_playback_mode", set_playback_mode_wrapper, "(i)i", NULL},
    {"get_playback_mode", get_playback_mode_wrapper, "()i", NULL},
    {"is_paused", is_paused_wrapper, "()i", NULL},
    {"get_volume_percent", get_volume_percent_wrapper, "()i", NULL},
    {"set_volume_percent", set_volume_percent_wrapper, "(i)i", NULL},
    {"sleep_ms", sleep_ms_wrapper, "(i)i", NULL},
    {"get_track_count", get_track_count_wrapper, "()i", NULL},
    {"get_track_title", get_track_title_wrapper, "(i*~)i", NULL},
    {"wifi_is_connected", wifi_is_connected_wrapper, "()i", NULL},
    {"get_free_heap", get_free_heap_wrapper, "()i", NULL},
    {"get_uptime_ms", get_uptime_ms_wrapper, "()I", NULL},
};

static esp_err_t script_execute_module(script_run_context_t *context)
{
    uint8_t *buffer = context->script_buffer;
    uint32_t buffer_size = context->script_buffer_size;
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    char error_buf[128] = {0};
    int capture_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    esp_err_t err = ESP_OK;

    if (!buffer)
    {
        buffer = script_load_file(context->result.resolved_path, &buffer_size, &context->result);
        if (!buffer)
        {
            return ESP_ERR_NOT_FOUND;
        }

        context->script_buffer = buffer;
        context->script_buffer_size = buffer_size;
    }

    ESP_EARLY_LOGI(TAG, "exec load ok");

    module = wasm_runtime_load(buffer, buffer_size, error_buf, sizeof(error_buf));
    if (!module)
    {
        script_set_result_message(&context->result, "%s", error_buf[0] ? error_buf : "wasm load failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_EARLY_LOGI(TAG, "exec module loaded");

#if defined(CONFIG_WAMR_ENABLE_LIBC_WASI) && CONFIG_WAMR_ENABLE_LIBC_WASI != 0
    {
        const char *preopen_dir = NULL;
        const char **dir_list = NULL;
        uint32_t dir_count = 0;
        const char *env_list[2] = {
            "JUKEBOY=1",
            app_is_running_in_qemu() ? "JUKEBOY_QEMU=1" : "JUKEBOY_QEMU=0",
        };

        if (!app_is_running_in_qemu() &&
            strncmp(context->result.resolved_path,
                    APP_LITTLEFS_MOUNT_PATH "/scripts",
                    strlen(APP_LITTLEFS_MOUNT_PATH "/scripts")) == 0)
        {
            /* WAMR needs a directory fd for WASI preopens; only LittleFS
             * exposes the open/fcntl hooks required for that on hardware. */
            preopen_dir = APP_LITTLEFS_MOUNT_PATH "/scripts";
            dir_list = &preopen_dir;
            dir_count = 1;
        }

        capture_fd = open(SCRIPT_STDIO_CAPTURE_PATH, O_CREAT | O_TRUNC | O_RDWR, 0664);
        if (capture_fd < 0)
        {
            script_set_result_message(&context->result,
                                      "failed to open WASI output capture: %s",
                                      strerror(errno));
            err = ESP_FAIL;
            goto cleanup;
        }

        stdout_fd = open(SCRIPT_STDIO_CAPTURE_PATH, O_WRONLY | O_APPEND);
        stderr_fd = open(SCRIPT_STDIO_CAPTURE_PATH, O_WRONLY | O_APPEND);
        if (stdout_fd < 0 || stderr_fd < 0)
        {
            script_set_result_message(&context->result,
                                      "failed to open WASI output capture stream: %s",
                                      strerror(errno));
            err = ESP_FAIL;
            goto cleanup;
        }

        wasm_runtime_set_wasi_args_ex(module,
                                      dir_list,
                                      dir_count,
                                      NULL,
                                      0,
                                      env_list,
                                      sizeof(env_list) / sizeof(env_list[0]),
                                      context->argv,
                                      context->argc,
                                      -1,
                                      stdout_fd,
                                      stderr_fd);

        wasm_runtime_set_wasi_addr_pool(module,
                                        s_wasi_addr_pool,
                                        sizeof(s_wasi_addr_pool) / sizeof(s_wasi_addr_pool[0]));
        wasm_runtime_set_wasi_ns_lookup_pool(module,
                                             s_wasi_ns_lookup_pool,
                                             sizeof(s_wasi_ns_lookup_pool) / sizeof(s_wasi_ns_lookup_pool[0]));
    }
#endif

    module_inst = wasm_runtime_instantiate(module,
                                           SCRIPT_SERVICE_WASM_STACK_SIZE,
                                           SCRIPT_SERVICE_WASM_HEAP_SIZE,
                                           error_buf,
                                           sizeof(error_buf));
    if (!module_inst)
    {
        script_set_result_message(&context->result, "%s", error_buf[0] ? error_buf : "wasm instantiate failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_EARLY_LOGI(TAG, "exec instantiated");

    if (!wasm_application_execute_main(module_inst, context->argc, context->argv))
    {
        const char *exception = wasm_runtime_get_exception(module_inst);
        script_set_result_message(&context->result,
                                  "%s",
                                  exception && exception[0] ? exception : "script execution failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_EARLY_LOGI(TAG, "exec main returned");

    context->result.exit_code = 0;
#if defined(CONFIG_WAMR_ENABLE_LIBC_WASI) && CONFIG_WAMR_ENABLE_LIBC_WASI != 0
    if (wasm_runtime_is_wasi_mode(module_inst))
    {
        context->result.exit_code = (int32_t)wasm_runtime_get_wasi_exit_code(module_inst);
    }
#endif

    if (context->result.exit_code == 0)
    {
        script_set_result_message(&context->result, "completed successfully");
    }
    else
    {
        script_set_result_message(&context->result, "completed with exit code %ld", (long)context->result.exit_code);
    }

cleanup:
    if (module_inst)
    {
        wasm_runtime_deinstantiate(module_inst);
    }
    if (module)
    {
        wasm_runtime_unload(module);
    }
    if (stdout_fd >= 0)
    {
        fsync(stdout_fd);
        close(stdout_fd);
        stdout_fd = -1;
    }
    if (stderr_fd >= 0)
    {
        fsync(stderr_fd);
        close(stderr_fd);
        stderr_fd = -1;
    }

    script_capture_output_file(capture_fd, &context->result);

    if (capture_fd >= 0)
    {
        fsync(capture_fd);
    }
    if (capture_fd >= 0)
    {
        close(capture_fd);
        unlink(SCRIPT_STDIO_CAPTURE_PATH);
    }
    free(context->script_buffer);
    context->script_buffer = NULL;
    context->script_buffer_size = 0;
    return err;
}

static void *script_runner_task(void *parameter)
{
    script_run_context_t *context = (script_run_context_t *)parameter;
    bool thread_env_initialized_here = false;

    ESP_LOGI(TAG, "runner entered for %s", context->result.resolved_path);
    ESP_EARLY_LOGI(TAG, "runner start");

    if (!wasm_runtime_thread_env_inited())
    {
        if (!wasm_runtime_init_thread_env())
        {
            context->err = ESP_FAIL;
            script_set_result_message(&context->result, "failed to initialize WAMR thread environment");
            goto done;
        }
        thread_env_initialized_here = true;
    }

    ESP_EARLY_LOGI(TAG, "runner env ready");

    context->err = script_execute_module(context);

done:
    if (thread_env_initialized_here)
    {
        wasm_runtime_destroy_thread_env();
    }

    return NULL;
}

static void *script_worker_task(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        xSemaphoreTake(s_worker_request_sem, portMAX_DELAY);

        if (s_pending_context)
        {
            ESP_LOGI(TAG, "worker picked up %s", s_pending_context->result.resolved_path);
            script_runner_task(s_pending_context);
            xSemaphoreGive(s_pending_context->completion_sem);
            ESP_LOGI(TAG, "worker completed %s", s_pending_context->result.resolved_path);
            s_pending_context = NULL;
        }
    }

    return NULL;
}

esp_err_t script_service_init(void)
{
    if (s_status == SCRIPT_SERVICE_STATUS_READY || s_status == SCRIPT_SERVICE_STATUS_BUSY)
    {
        return ESP_OK;
    }

    if (s_status == SCRIPT_SERVICE_STATUS_ERROR)
    {
        return ESP_FAIL;
    }

    s_runtime_mutex = xSemaphoreCreateMutexStatic(&s_runtime_mutex_storage);
    if (!s_runtime_mutex)
    {
        s_status = SCRIPT_SERVICE_STATUS_ERROR;
        return ESP_ERR_NO_MEM;
    }

    s_worker_request_sem = xSemaphoreCreateBinaryStatic(&s_worker_request_sem_storage);
    if (!s_worker_request_sem)
    {
        s_status = SCRIPT_SERVICE_STATUS_ERROR;
        return ESP_ERR_NO_MEM;
    }

    wm_wamr_init();

    if (!s_natives_registered)
    {
        if (!wasm_runtime_register_natives(SCRIPT_SERVICE_HOST_MODULE_NAME,
                                           s_jukeboy_symbols,
                                           sizeof(s_jukeboy_symbols) / sizeof(s_jukeboy_symbols[0])))
        {
            s_status = SCRIPT_SERVICE_STATUS_ERROR;
            return ESP_FAIL;
        }
        s_natives_registered = true;
    }

    for (size_t index = 0; index < sizeof(s_script_roots) / sizeof(s_script_roots[0]); index++)
    {
        if (s_script_roots[index].ensure_on_init)
        {
            script_ensure_directory(s_script_roots[index].path);
        }
    }

    if (cartridge_service_is_mounted())
    {
        script_ensure_directory(SCRIPT_SD_ROOT_PATH);
    }

    if (!s_worker_thread_started)
    {
        esp_pthread_cfg_t worker_cfg = esp_pthread_get_default_config();
        esp_pthread_cfg_t restore_cfg = esp_pthread_get_default_config();

        worker_cfg.stack_size = SCRIPT_SERVICE_RUNNER_STACK_SIZE;
        worker_cfg.prio = 4;
        worker_cfg.inherit_cfg = false;
        worker_cfg.thread_name = "ScriptRunner";
        worker_cfg.pin_to_core = tskNO_AFFINITY;

        if (esp_pthread_set_cfg(&worker_cfg) != ESP_OK ||
            pthread_create(&s_worker_thread, NULL, script_worker_task, NULL) != 0)
        {
            esp_pthread_set_cfg(&restore_cfg);
            s_status = SCRIPT_SERVICE_STATUS_ERROR;
            return ESP_FAIL;
        }

        esp_pthread_set_cfg(&restore_cfg);
        pthread_detach(s_worker_thread);
        s_worker_thread_started = true;
    }

    s_status = SCRIPT_SERVICE_STATUS_READY;
    ESP_LOGI(TAG, "Script service ready");
    return ESP_OK;
}

bool script_service_is_ready(void)
{
    return s_status == SCRIPT_SERVICE_STATUS_READY || s_status == SCRIPT_SERVICE_STATUS_BUSY;
}

script_service_status_t script_service_get_status(void)
{
    return s_status;
}

const char *script_service_status_name(script_service_status_t status)
{
    switch (status)
    {
    case SCRIPT_SERVICE_STATUS_UNINITIALIZED:
        return "uninitialized";
    case SCRIPT_SERVICE_STATUS_READY:
        return "ready";
    case SCRIPT_SERVICE_STATUS_BUSY:
        return "busy";
    case SCRIPT_SERVICE_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

size_t script_service_get_root_count(void)
{
    return sizeof(s_script_roots) / sizeof(s_script_roots[0]);
}

const char *script_service_get_root_label(size_t index)
{
    return index < script_service_get_root_count() ? s_script_roots[index].label : NULL;
}

const char *script_service_get_root_path(size_t index)
{
    return index < script_service_get_root_count() ? s_script_roots[index].path : NULL;
}

esp_err_t script_service_resolve_path(const char *path, char *out_path, size_t out_path_size)
{
    if (!path || path[0] == '\0' || !out_path || out_path_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (script_try_path_variants(path, out_path, out_path_size))
    {
        return ESP_OK;
    }

    if (script_try_labeled_root(path, out_path, out_path_size))
    {
        return ESP_OK;
    }

    for (size_t index = 0; index < script_service_get_root_count(); index++)
    {
        if (script_try_root(script_service_get_root_path(index), path, out_path, out_path_size))
        {
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t script_service_run(const char *path,
                             int argc,
                             const char *const *argv,
                             script_service_run_result_t *result)
{
    script_service_run_result_t *owned_result = NULL;
    script_run_context_t *context = NULL;
    esp_err_t err;

    if (!result)
    {
        owned_result = calloc(1, sizeof(*owned_result));
        if (!owned_result)
        {
            return ESP_ERR_NO_MEM;
        }
        result = owned_result;
    }
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;

    err = script_service_init();
    if (err != ESP_OK)
    {
        script_set_result_message(result, "script service init failed");
        free(owned_result);
        return err;
    }

    if (!path || path[0] == '\0')
    {
        script_set_result_message(result, "script path is required");
        free(owned_result);
        return ESP_ERR_INVALID_ARG;
    }

    context = calloc(1, sizeof(*context));
    if (!context)
    {
        script_set_result_message(result, "out of memory");
        free(owned_result);
        return ESP_ERR_NO_MEM;
    }

    context->completion_sem = xSemaphoreCreateBinaryStatic(&context->completion_sem_storage);
    if (!context->completion_sem)
    {
        script_set_result_message(result, "failed to create script completion semaphore");
        free(context);
        free(owned_result);
        return ESP_ERR_NO_MEM;
    }

    context->argc = 0;
    context->result.exit_code = -1;
    snprintf(context->requested_path, sizeof(context->requested_path), "%s", path);

    err = script_service_resolve_path(path,
                                      context->result.resolved_path,
                                      sizeof(context->result.resolved_path));
    if (err != ESP_OK)
    {
        script_set_result_message(result, "could not resolve %s", path);
        free(context);
        free(owned_result);
        return err;
    }

    ESP_LOGI(TAG, "dispatching script %s", context->result.resolved_path);

    context->argv = script_duplicate_argv(context->result.resolved_path,
                                          argc,
                                          argv,
                                          &context->argc);
    if (!context->argv)
    {
        script_set_result_message(result, "out of memory copying arguments");
        free(context);
        free(owned_result);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "preloading script %s", context->result.resolved_path);
    context->script_buffer = script_load_file(context->result.resolved_path,
                                              &context->script_buffer_size,
                                              &context->result);
    if (!context->script_buffer)
    {
        free(context->argv);
        free(context);
        free(owned_result);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    s_status = SCRIPT_SERVICE_STATUS_BUSY;
    s_active_run_context = context;

    s_pending_context = context;
    ESP_LOGI(TAG, "queued script %s", context->result.resolved_path);
    xSemaphoreGive(s_worker_request_sem);
    xSemaphoreTake(context->completion_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "script wait complete for %s", context->result.resolved_path);

    s_active_run_context = NULL;
    s_status = SCRIPT_SERVICE_STATUS_READY;
    xSemaphoreGive(s_runtime_mutex);

    *result = context->result;
    err = context->err;

    free(context->argv);
    free(context);
    free(owned_result);
    return err;
}