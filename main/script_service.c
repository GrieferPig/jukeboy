#include "script_service.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "cartridge_service.h"
#include "player_service.h"
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "storage_paths.h"
#include "wasm_export.h"
#include "wm_wamr.h"

#define SCRIPT_SERVICE_RUNNER_STACK_SIZE 8192
#define SCRIPT_SERVICE_WASM_STACK_SIZE (32 * 1024)
#define SCRIPT_SERVICE_WASM_HEAP_SIZE (32 * 1024)

#define SCRIPT_SD_ROOT_PATH "/sdcard/scripts"

typedef struct
{
    const char *label;
    const char *path;
    bool ensure_on_init;
} script_root_t;

typedef struct
{
    TaskHandle_t waiter_task;
    char requested_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    char **argv;
    int argc;
    esp_err_t err;
    script_service_run_result_t result;
} script_run_context_t;

static const char *TAG = "script_svc";

static const script_root_t s_script_roots[] = {
    { "lfs", APP_LITTLEFS_MOUNT_PATH "/scripts", true },
    { "tmp", RAMDISK_SERVICE_MOUNT_PATH "/scripts", true },
    { "sd", SCRIPT_SD_ROOT_PATH, false },
};

static script_service_status_t s_status = SCRIPT_SERVICE_STATUS_UNINITIALIZED;
static StaticSemaphore_t s_runtime_mutex_storage;
static SemaphoreHandle_t s_runtime_mutex;
static bool s_natives_registered;

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

static bool script_is_regular_file(const char *path)
{
    struct stat st = { 0 };

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
    struct stat st = { 0 };

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

static char **script_duplicate_argv(int argc, const char *const *argv)
{
    size_t total_bytes = 0;
    char **argv_copy;
    char *storage;

    if (argc <= 0)
    {
        return NULL;
    }

    if (!argv)
    {
        return NULL;
    }

    for (int index = 0; index < argc; index++)
    {
        const char *arg = argv[index] ? argv[index] : "";
        total_bytes += strlen(arg) + 1;
    }

    argv_copy = malloc(sizeof(char *) * (size_t)(argc + 1) + total_bytes);
    if (!argv_copy)
    {
        return NULL;
    }

    storage = (char *)(argv_copy + argc + 1);
    for (int index = 0; index < argc; index++)
    {
        const char *arg = argv[index] ? argv[index] : "";
        size_t arg_len = strlen(arg) + 1;

        argv_copy[index] = storage;
        memcpy(storage, arg, arg_len);
        storage += arg_len;
    }
    argv_copy[argc] = NULL;

    return argv_copy;
}

static uint8_t *script_load_file(const char *path, uint32_t *size_out, script_service_run_result_t *result)
{
    struct stat st = { 0 };
    FILE *file = NULL;
    uint8_t *buffer = NULL;

    if (!path || !size_out)
    {
        return NULL;
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        script_set_result_message(result, "cannot stat %s", path);
        return NULL;
    }

    if (st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX)
    {
        script_set_result_message(result, "invalid script size for %s", path);
        return NULL;
    }

    file = fopen(path, "rb");
    if (!file)
    {
        script_set_result_message(result, "cannot open %s", path);
        return NULL;
    }

    buffer = malloc((size_t)st.st_size);
    if (!buffer)
    {
        fclose(file);
        script_set_result_message(result, "out of memory loading %s", path);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)st.st_size, file) != (size_t)st.st_size)
    {
        fclose(file);
        free(buffer);
        script_set_result_message(result, "failed to read %s", path);
        return NULL;
    }

    fclose(file);
    *size_out = (uint32_t)st.st_size;
    result->script_size_bytes = (uint32_t)st.st_size;
    return buffer;
}

static int log_wrapper(wasm_exec_env_t exec_env, const char *message)
{
    (void)exec_env;

    ESP_LOGI(TAG, "[wasm] %s", message ? message : "");
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

static NativeSymbol s_jukeboy_symbols[] = {
    { "log", log_wrapper, "($)i", NULL },
    { "next_track", next_track_wrapper, "()i", NULL },
    { "previous_track", previous_track_wrapper, "()i", NULL },
    { "pause_toggle", pause_toggle_wrapper, "()i", NULL },
    { "fast_forward", fast_forward_wrapper, "()i", NULL },
    { "rewind", rewind_wrapper, "()i", NULL },
    { "volume_up", volume_up_wrapper, "()i", NULL },
    { "volume_down", volume_down_wrapper, "()i", NULL },
    { "set_playback_mode", set_playback_mode_wrapper, "(i)i", NULL },
    { "get_playback_mode", get_playback_mode_wrapper, "()i", NULL },
    { "is_paused", is_paused_wrapper, "()i", NULL },
    { "get_volume_percent", get_volume_percent_wrapper, "()i", NULL },
    { "set_volume_percent", set_volume_percent_wrapper, "(i)i", NULL },
    { "sleep_ms", sleep_ms_wrapper, "(i)i", NULL },
};

static esp_err_t script_execute_module(script_run_context_t *context)
{
    uint8_t *buffer = NULL;
    uint32_t buffer_size = 0;
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    char error_buf[128] = { 0 };
    esp_err_t err = ESP_OK;

    buffer = script_load_file(context->result.resolved_path, &buffer_size, &context->result);
    if (!buffer)
    {
        return ESP_ERR_NOT_FOUND;
    }

    module = wasm_runtime_load(buffer, buffer_size, error_buf, sizeof(error_buf));
    if (!module)
    {
        script_set_result_message(&context->result, "%s", error_buf[0] ? error_buf : "wasm load failed");
        err = ESP_FAIL;
        goto cleanup;
    }

#if defined(CONFIG_WAMR_ENABLE_LIBC_WASI) && CONFIG_WAMR_ENABLE_LIBC_WASI != 0
    {
        const char *dir_list[3] = { APP_LITTLEFS_MOUNT_PATH, RAMDISK_SERVICE_MOUNT_PATH, NULL };
        const char *env_list[2] = {
            "JUKEBOY=1",
            app_is_running_in_qemu() ? "JUKEBOY_QEMU=1" : "JUKEBOY_QEMU=0",
        };
        uint32_t dir_count = 2;

        if (cartridge_service_is_mounted())
        {
            dir_list[dir_count++] = cartridge_service_get_mount_point();
        }

        wasm_runtime_set_wasi_args(module,
                                   dir_list,
                                   dir_count,
                                   NULL,
                                   0,
                                   env_list,
                                   sizeof(env_list) / sizeof(env_list[0]),
                                   context->argv,
                                   context->argc);
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

    if (!wasm_application_execute_main(module_inst, context->argc, context->argv))
    {
        const char *exception = wasm_runtime_get_exception(module_inst);
        script_set_result_message(&context->result,
                                  "%s",
                                  exception && exception[0] ? exception : "script execution failed");
        err = ESP_FAIL;
        goto cleanup;
    }

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
    free(buffer);
    return err;
}

static void script_runner_task(void *parameter)
{
    script_run_context_t *context = (script_run_context_t *)parameter;
    bool thread_env_initialized_here = false;

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

    context->err = script_execute_module(context);

done:
    if (thread_env_initialized_here)
    {
        wasm_runtime_destroy_thread_env();
    }

    xTaskNotifyGive(context->waiter_task);
    vTaskDelete(NULL);
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
    script_service_run_result_t local_result = { 0 };
    script_run_context_t *context = NULL;
    esp_err_t err;

    if (!result)
    {
        result = &local_result;
    }
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;

    err = script_service_init();
    if (err != ESP_OK)
    {
        script_set_result_message(result, "script service init failed");
        return err;
    }

    if (!path || path[0] == '\0')
    {
        script_set_result_message(result, "script path is required");
        return ESP_ERR_INVALID_ARG;
    }

    context = calloc(1, sizeof(*context));
    if (!context)
    {
        script_set_result_message(result, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    context->argc = argc;
    context->waiter_task = xTaskGetCurrentTaskHandle();
    context->result.exit_code = -1;
    snprintf(context->requested_path, sizeof(context->requested_path), "%s", path);

    err = script_service_resolve_path(path,
                                      context->result.resolved_path,
                                      sizeof(context->result.resolved_path));
    if (err != ESP_OK)
    {
        script_set_result_message(result, "could not resolve %s", path);
        free(context);
        return err;
    }

    if (argc > 0)
    {
        context->argv = script_duplicate_argv(argc, argv);
        if (!context->argv)
        {
            script_set_result_message(result, "out of memory copying arguments");
            free(context);
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    s_status = SCRIPT_SERVICE_STATUS_BUSY;
    ulTaskNotifyTake(pdTRUE, 0);

    if (xTaskCreatePinnedToCore(script_runner_task,
                                "ScriptRunner",
                                SCRIPT_SERVICE_RUNNER_STACK_SIZE,
                                context,
                                4,
                                NULL,
                                tskNO_AFFINITY) != pdPASS)
    {
        s_status = SCRIPT_SERVICE_STATUS_READY;
        xSemaphoreGive(s_runtime_mutex);
        script_set_result_message(result, "failed to start script runner task");
        free(context->argv);
        free(context);
        return ESP_ERR_NO_MEM;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    s_status = SCRIPT_SERVICE_STATUS_READY;
    xSemaphoreGive(s_runtime_mutex);

    *result = context->result;
    err = context->err;

    free(context->argv);
    free(context);
    return err;
}