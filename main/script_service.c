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

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cartridge_service.h"
#include "player_service.h"
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "script_socket_env.h"
#include "storage_paths.h"
#include "wifi_service.h"
#include "wasm_export.h"
#include "wm_wamr.h"

#define SCRIPT_SERVICE_RUNNER_STACK_SIZE (16 * 1024)
/* Keep the CPU-bound guest runner at idle priority so it cannot starve the
 * FreeRTOS idle tasks and trip the task watchdog during long script runs. */
#define SCRIPT_SERVICE_RUNNER_PRIORITY tskIDLE_PRIORITY
/* Managed WASM stack and heap allocations flow through wm_wamr.c, which places
 * them in PSRAM when external RAM support is enabled. */
#define SCRIPT_SERVICE_WASM_STACK_SIZE (128 * 1024)
#define SCRIPT_SERVICE_WASM_HEAP_SIZE (128 * 1024)
#define SCRIPT_SERVICE_MAX_SCRIPT_SIZE (512 * 1024)
#define SCRIPT_SERVICE_MAX_ARGC 32
#define SCRIPT_SERVICE_MAX_ARG_LEN 256
#define SCRIPT_SERVICE_MAX_ARG_STORAGE_BYTES \
    ((SCRIPT_SERVICE_MAX_ARGC + 1U) * (SCRIPT_SERVICE_MAX_ARG_LEN + 1U))
#define SCRIPT_SERVICE_MAX_PRINTF_CAPTURE_LEN 512

#define SCRIPT_LFS_ROOT_PATH APP_LITTLEFS_MOUNT_PATH "/scripts"
#define SCRIPT_CWASM_EXTENSION ".cwasm"
#define SCRIPT_LOG_ROOT_PATH RAMDISK_SERVICE_MOUNT_PATH "/script-logs"

typedef struct
{
    char requested_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    char log_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    char **argv;
    int argc;
    bool detach_on_finish;
    script_service_run_mode_t mode;
    uint8_t *script_buffer;
    uint32_t script_buffer_size;
    int log_fd;
    esp_err_t err;
    SemaphoreHandle_t completion_sem;
    StaticSemaphore_t completion_sem_storage;
    script_service_run_result_t result;
} script_run_context_t;

static const char *TAG = "script_svc";

static script_service_status_t s_status = SCRIPT_SERVICE_STATUS_UNINITIALIZED;
static StaticSemaphore_t s_state_mutex_storage;
static SemaphoreHandle_t s_state_mutex;
static StaticSemaphore_t s_worker_request_sem_storage;
static SemaphoreHandle_t s_worker_request_sem;
static bool s_natives_registered;
static script_run_context_t *s_active_run_context;
static script_run_context_t *s_log_capture_context;
static script_run_context_t *s_pending_context;
static pthread_t s_worker_thread;
static bool s_worker_thread_started;
static bool s_last_result_valid;
static script_service_run_result_t s_last_result;
static esp_err_t s_last_run_err;
static int64_t s_active_run_started_us;
static int64_t s_last_run_finished_us;
static uint32_t s_next_run_id = 1;
EXT_RAM_BSS_ATTR static char s_log_trim_buffer[SCRIPT_SERVICE_LOG_MAX_SIZE];

static esp_err_t script_validate_guest_buffer_from_native(wasm_exec_env_t exec_env,
                                                          void *native_ptr,
                                                          uint32_t size,
                                                          bool allow_null,
                                                          void **out_ptr)
{
    wasm_module_inst_t module_inst;
    uint32_t offset;

    if (!out_ptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ptr = NULL;

    if (!exec_env)
    {
        return ESP_ERR_INVALID_ARG;
    }

    module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!native_ptr)
    {
        return allow_null ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    if (!wasm_runtime_validate_native_addr(module_inst, native_ptr, size == 0 ? 1 : size))
    {
        return ESP_ERR_INVALID_ARG;
    }

    offset = wasm_runtime_addr_native_to_app(module_inst, native_ptr);
    if (offset == 0)
    {
        return allow_null ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    return script_socket_get_guest_buffer(module_inst, offset, size, allow_null, out_ptr) ==
                   SCRIPT_SOCKET_WASI_ESUCCESS
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

static esp_err_t script_validate_guest_string_from_native(wasm_exec_env_t exec_env,
                                                          const char *native_ptr,
                                                          bool allow_null,
                                                          const char **out_ptr)
{
    wasm_module_inst_t module_inst;
    uint32_t offset;

    if (!out_ptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ptr = NULL;

    if (!exec_env)
    {
        return ESP_ERR_INVALID_ARG;
    }

    module_inst = wasm_runtime_get_module_inst(exec_env);
    if (!module_inst)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!native_ptr)
    {
        return allow_null ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    if (!wasm_runtime_validate_native_addr(module_inst, (void *)native_ptr, 1))
    {
        return ESP_ERR_INVALID_ARG;
    }

    offset = wasm_runtime_addr_native_to_app(module_inst, (void *)native_ptr);
    if (offset == 0)
    {
        return allow_null ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    return script_socket_get_guest_string(module_inst, offset, allow_null, out_ptr) ==
                   SCRIPT_SOCKET_WASI_ESUCCESS
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

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

static void script_init_result(script_service_run_result_t *result)
{
    if (!result)
    {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->mode = SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN;
    result->exit_code = -1;
}

static void script_free_context(script_run_context_t *context)
{
    if (!context)
    {
        return;
    }

    if (context->log_fd >= 0)
    {
        close(context->log_fd);
        context->log_fd = -1;
    }

    free(context->script_buffer);
    context->script_buffer = NULL;
    context->script_buffer_size = 0;
    free(context->argv);
    context->argv = NULL;
    free(context);
}

static void script_capture_last_result_locked(const script_run_context_t *context)
{
    if (!context)
    {
        return;
    }

    s_last_result = context->result;
    s_last_run_err = context->err;
    s_last_result_valid = true;
    s_last_run_finished_us = esp_timer_get_time();
}

static void script_ensure_directory(const char *path);

static esp_err_t script_build_log_path_from_resolved_path(const char *resolved_path,
                                                          char *out_path,
                                                          size_t out_path_size)
{
    const char *filename;
    int written;

    if (!resolved_path || resolved_path[0] == '\0' || !out_path || out_path_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    filename = strrchr(resolved_path, '/');
    filename = filename ? filename + 1 : resolved_path;
    if (filename[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_path, out_path_size, "%s/%s.log", SCRIPT_LOG_ROOT_PATH, filename);
    if (written <= 0 || (size_t)written >= out_path_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t script_trim_log_fd(int log_fd)
{
    off_t log_size;
    off_t read_offset;
    ssize_t bytes_read;
    size_t keep_offset = 0;
    esp_err_t err = ESP_OK;

    if (log_fd < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    log_size = lseek(log_fd, 0, SEEK_END);
    if (log_size < 0)
    {
        return ESP_FAIL;
    }

    if ((size_t)log_size <= SCRIPT_SERVICE_LOG_MAX_SIZE)
    {
        return ESP_OK;
    }

    read_offset = log_size - SCRIPT_SERVICE_LOG_MAX_SIZE;
    if (read_offset < 0)
    {
        read_offset = 0;
    }

    if (lseek(log_fd, read_offset, SEEK_SET) < 0)
    {
        err = ESP_FAIL;
        goto done;
    }

    bytes_read = read(log_fd, s_log_trim_buffer, sizeof(s_log_trim_buffer));
    if (bytes_read < 0)
    {
        err = ESP_FAIL;
        goto done;
    }

    if (read_offset > 0)
    {
        char *newline = memchr(s_log_trim_buffer, '\n', (size_t)bytes_read);

        if (newline)
        {
            keep_offset = (size_t)(newline - s_log_trim_buffer) + 1;
        }
    }

    if (lseek(log_fd, 0, SEEK_SET) < 0)
    {
        err = ESP_FAIL;
        goto done;
    }

    if (keep_offset < (size_t)bytes_read)
    {
        size_t kept_size = (size_t)bytes_read - keep_offset;

        if (write(log_fd, s_log_trim_buffer + keep_offset, kept_size) != (ssize_t)kept_size)
        {
            err = ESP_FAIL;
            goto done;
        }

        if (ftruncate(log_fd, (off_t)kept_size) != 0)
        {
            err = ESP_FAIL;
            goto done;
        }
    }
    else if (ftruncate(log_fd, 0) != 0)
    {
        err = ESP_FAIL;
        goto done;
    }

    if (lseek(log_fd, 0, SEEK_END) < 0)
    {
        err = ESP_FAIL;
        goto done;
    }

done:
    return err;
}

static esp_err_t script_append_log_data(script_run_context_t *context,
                                        const char *data,
                                        size_t data_len)
{
    if (!context || context->log_fd < 0 || !data || data_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (write(context->log_fd, data, data_len) != (ssize_t)data_len)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

int wamr_host_vprintf_hook(const char *format, va_list ap)
{
    script_run_context_t *context = NULL;
    va_list console_args;
    va_list format_args;
    int console_result;
    int formatted_len = -1;
    char buffer[SCRIPT_SERVICE_MAX_PRINTF_CAPTURE_LEN];

    if (!format)
    {
        return -1;
    }

    va_copy(console_args, ap);
    console_result = vprintf(format, console_args);
    va_end(console_args);

    if (s_state_mutex)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        context = s_log_capture_context;
        if (context && context->log_fd >= 0)
        {
            va_copy(format_args, ap);
            formatted_len = vsnprintf(buffer, sizeof(buffer), format, format_args);
            va_end(format_args);

            if (formatted_len > 0)
            {
                size_t copy_len = (size_t)formatted_len;

                if (copy_len >= sizeof(buffer))
                {
                    copy_len = sizeof(buffer) - 1;
                }

                if (script_append_log_data(context, buffer, copy_len) != ESP_OK)
                {
                    formatted_len = -1;
                }
            }
        }
        xSemaphoreGive(s_state_mutex);
    }

    return console_result >= 0 ? console_result : formatted_len;
}

static esp_err_t script_prepare_log_file(script_run_context_t *context)
{
    int fd;

    if (!context)
    {
        return ESP_ERR_INVALID_ARG;
    }

    script_ensure_directory(SCRIPT_LOG_ROOT_PATH);

    if (script_build_log_path_from_resolved_path(context->result.resolved_path,
                                                 context->log_path,
                                                 sizeof(context->log_path)) != ESP_OK)
    {
        script_set_result_message(&context->result, "failed to prepare log path for %s",
                                  context->result.resolved_path);
        return ESP_ERR_INVALID_SIZE;
    }

    fd = open(context->log_path, O_CREAT | O_TRUNC | O_RDWR, 0664);
    if (fd < 0)
    {
        script_set_result_message(&context->result, "failed to open %s", context->log_path);
        return ESP_FAIL;
    }

    context->log_fd = fd;
    return ESP_OK;
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

static bool script_is_name_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' ||
           ch == '-';
}

static bool script_is_supported_extension(const char *extension)
{
    if (!extension)
    {
        return false;
    }

    if (strcmp(extension, ".wasm") == 0 || strcmp(extension, SCRIPT_CWASM_EXTENSION) == 0)
    {
        return true;
    }

#if defined(CONFIG_WAMR_ENABLE_AOT) && CONFIG_WAMR_ENABLE_AOT != 0
    if (strcmp(extension, ".aot") == 0)
    {
        return true;
    }
#endif

    return false;
}

static esp_err_t script_parse_name(const char *input,
                                   char *script_name,
                                   size_t script_name_size,
                                   const char **extension_out)
{
    const char *extension = NULL;
    size_t name_len;

    if (!input || input[0] == '\0' || !script_name || script_name_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (input[0] == '/' || strchr(input, '/') || strchr(input, '\\'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(input, ".") == 0 || strcmp(input, "..") == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    extension = strrchr(input, '.');
    name_len = extension ? (size_t)(extension - input) : strlen(input);
    if (name_len == 0 || name_len + 1 > script_name_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < name_len; index++)
    {
        if (!script_is_name_char(input[index]))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (extension && !script_is_supported_extension(extension))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(script_name, input, name_len);
    script_name[name_len] = '\0';

    if (extension_out)
    {
        *extension_out = extension;
    }

    return ESP_OK;
}

static esp_err_t script_build_script_directory(const char *name,
                                               char *out_path,
                                               size_t out_path_size)
{
    char script_name[SCRIPT_SERVICE_MAX_PATH_LEN];
    int written;
    esp_err_t err = script_parse_name(name, script_name, sizeof(script_name), NULL);

    if (err != ESP_OK)
    {
        return err;
    }

    written = snprintf(out_path, out_path_size, "%s/%s", SCRIPT_LFS_ROOT_PATH, script_name);
    if (written <= 0 || (size_t)written >= out_path_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t script_build_resolve_base_path(const char *name,
                                                char *out_path,
                                                size_t out_path_size,
                                                bool *has_extension_out)
{
    char script_name[SCRIPT_SERVICE_MAX_PATH_LEN];
    const char *extension = NULL;
    int written;
    esp_err_t err = script_parse_name(name, script_name, sizeof(script_name), &extension);

    if (err != ESP_OK)
    {
        return err;
    }

    written = snprintf(out_path,
                       out_path_size,
                       "%s/%s/%s%s",
                       SCRIPT_LFS_ROOT_PATH,
                       script_name,
                       script_name,
                       extension ? extension : "");
    if (written <= 0 || (size_t)written >= out_path_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (has_extension_out)
    {
        *has_extension_out = extension != NULL;
    }

    return ESP_OK;
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

    written = snprintf(candidate, sizeof(candidate), "%s%s", base_path, SCRIPT_CWASM_EXTENSION);
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

static script_service_run_mode_t script_detect_run_mode(const char *resolved_path)
{
    (void)resolved_path;
    return SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN;
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
                                    script_service_run_result_t *result,
                                    int *argc_out)
{
    size_t total_bytes = 0;
    int total_argc = argc + 1;
    char **argv_copy;
    char *storage;
    const char *effective_program_path = program_path && program_path[0] ? program_path : "script";
    size_t program_len;

    if (argc_out)
    {
        *argc_out = 0;
    }

    if (argc < 0 || argc > SCRIPT_SERVICE_MAX_ARGC)
    {
        script_set_result_message(result,
                                  "too many script arguments (max %u)",
                                  (unsigned)SCRIPT_SERVICE_MAX_ARGC);
        return NULL;
    }

    if (argc > 0 && !argv)
    {
        script_set_result_message(result, "missing script arguments");
        return NULL;
    }

    program_len = strnlen(effective_program_path, SCRIPT_SERVICE_MAX_ARG_LEN);
    if (program_len == 0 || program_len >= SCRIPT_SERVICE_MAX_ARG_LEN)
    {
        script_set_result_message(result,
                                  "script path exceeds max length (%u)",
                                  (unsigned)SCRIPT_SERVICE_MAX_ARG_LEN - 1);
        return NULL;
    }

    total_bytes += program_len + 1;

    for (int index = 0; index < argc; index++)
    {
        const char *arg = argv[index] ? argv[index] : "";
        size_t arg_len = strnlen(arg, SCRIPT_SERVICE_MAX_ARG_LEN);

        if (arg_len >= SCRIPT_SERVICE_MAX_ARG_LEN)
        {
            script_set_result_message(result,
                                      "script argument %d exceeds max length (%u)",
                                      index,
                                      (unsigned)SCRIPT_SERVICE_MAX_ARG_LEN - 1);
            return NULL;
        }

        total_bytes += arg_len + 1;
        if (total_bytes > SCRIPT_SERVICE_MAX_ARG_STORAGE_BYTES)
        {
            script_set_result_message(result,
                                      "script arguments exceed max storage (%u bytes)",
                                      (unsigned)SCRIPT_SERVICE_MAX_ARG_STORAGE_BYTES);
            return NULL;
        }
    }

    argv_copy = malloc(sizeof(char *) * (size_t)(total_argc + 1) + total_bytes);
    if (!argv_copy)
    {
        return NULL;
    }

    storage = (char *)(argv_copy + total_argc + 1);
    argv_copy[0] = storage;
    memcpy(storage, effective_program_path, program_len + 1);
    storage += program_len + 1;

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

    if ((size_t)file_size > SCRIPT_SERVICE_MAX_SCRIPT_SIZE)
    {
        fclose(file);
        script_set_result_message(result,
                                  "script exceeds max size (%u bytes)",
                                  (unsigned)SCRIPT_SERVICE_MAX_SCRIPT_SIZE);
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
    const char *guest_message = NULL;
    script_run_context_t *context = NULL;

    if (script_validate_guest_string_from_native(exec_env, message, false, &guest_message) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state_mutex)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        context = s_log_capture_context;
        if (context)
        {
            script_append_output(&context->result, "[wasm] ");
            script_append_output(&context->result, guest_message);
            script_append_output(&context->result, "\n");
            if (context->log_fd >= 0)
            {
                static const char prefix[] = "[wasm] ";
                static const char newline[] = "\n";

                if (script_append_log_data(context, prefix, sizeof(prefix) - 1) == ESP_OK)
                {
                    size_t message_len = strlen(guest_message);

                    if (message_len > 0)
                    {
                        (void)script_append_log_data(context, guest_message, message_len);
                    }
                    (void)script_append_log_data(context, newline, sizeof(newline) - 1);
                }
            }
        }
        xSemaphoreGive(s_state_mutex);
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
    char *guest_buf = NULL;
    const char *title;
    size_t title_len;

    if (!buf || buf_len == 0 || index < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (script_validate_guest_buffer_from_native(exec_env, buf, buf_len, false, (void **)&guest_buf) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    title = cartridge_service_get_track_name((size_t)index);
    if (!title)
    {
        guest_buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    title_len = strlen(title);
    if (title_len >= buf_len)
    {
        title_len = buf_len - 1;
    }

    memcpy(guest_buf, title, title_len);
    guest_buf[title_len] = '\0';
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

static esp_err_t script_prepare_builtin_argv(script_run_context_t *context,
                                             wasm_module_inst_t module_inst,
                                             uint64_t *argv_offset_out)
{
    uint32_t *argv_offsets = NULL;
    char *string_storage;
    uint64_t total_size = (uint64_t)sizeof(uint32_t) * (uint64_t)(context->argc + 1);
    uint64_t argv_offset;
    uint32_t next_string_offset;

    if (!argv_offset_out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *argv_offset_out = 0;

    for (int index = 0; index < context->argc; index++)
    {
        total_size += (uint64_t)strlen(context->argv[index]) + 1;
    }

    argv_offset = wasm_runtime_module_malloc(module_inst, total_size, (void **)&argv_offsets);
    if (argv_offset == 0 || !argv_offsets)
    {
        script_set_result_message(&context->result, "failed to allocate builtin argv");
        return ESP_ERR_NO_MEM;
    }

    string_storage = (char *)(argv_offsets + context->argc + 1);
    next_string_offset = (uint32_t)(argv_offset + (uint64_t)sizeof(uint32_t) * (uint64_t)(context->argc + 1));

    for (int index = 0; index < context->argc; index++)
    {
        size_t arg_len = strlen(context->argv[index]) + 1;

        argv_offsets[index] = next_string_offset;
        memcpy(string_storage, context->argv[index], arg_len);
        string_storage += arg_len;
        next_string_offset += (uint32_t)arg_len;
    }
    argv_offsets[context->argc] = 0;

    *argv_offset_out = argv_offset;
    return ESP_OK;
}

static void script_yield_task(void *pvParameters)
{
    TaskHandle_t target_task = (TaskHandle_t)pvParameters;

    while (1)
    {
        // Run every 1000 ms to give the watchdog a breather
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Stop the heavy WAMR execution
        vTaskSuspend(target_task);

        // Sleep for exactly 1 OS tick (10ms on ESP32 by default) to let IDLE1 run
        vTaskDelay(1);

        // Restart the WAMR execution
        vTaskResume(target_task);
    }
}

static esp_err_t script_execute_builtin_module(script_run_context_t *context,
                                               wasm_module_inst_t module_inst)
{
    wasm_function_inst_t main_function;
    const char *entry_name = "main";
    wasm_exec_env_t exec_env = NULL;
    bool owns_exec_env = false;
    wasm_valkind_t param_types[2] = {0};
    wasm_valkind_t result_types[1] = {0};
    wasm_val_t args[2] = {0};
    wasm_val_t results[1] = {0};
    uint64_t argv_offset = 0;
    uint32_t param_count;
    uint32_t result_count;
    TaskHandle_t yield_task_handle = NULL;
    esp_err_t err = ESP_OK;

    main_function = wasm_runtime_lookup_function(module_inst, "main");
    if (!main_function)
    {
        entry_name = "__main_argc_argv";
        main_function = wasm_runtime_lookup_function(module_inst, entry_name);
    }
    if (!main_function)
    {
        script_set_result_message(&context->result,
                                  "builtin script does not export main or __main_argc_argv");
        return ESP_ERR_NOT_FOUND;
    }

    param_count = wasm_func_get_param_count(main_function, module_inst);
    if (param_count != 0 && param_count != 2)
    {
        script_set_result_message(&context->result,
                                  "builtin entry %s must use () or (int, char **)",
                                  entry_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (param_count > 0)
    {
        wasm_func_get_param_types(main_function, module_inst, param_types);
        if (param_types[0] != WASM_I32 || param_types[1] != WASM_I32)
        {
            script_set_result_message(&context->result,
                                      "builtin entry %s arguments must be 32-bit integers",
                                      entry_name);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    result_count = wasm_func_get_result_count(main_function, module_inst);
    if (result_count > 1)
    {
        script_set_result_message(&context->result,
                                  "builtin entry %s must return at most one result",
                                  entry_name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (result_count > 0)
    {
        wasm_func_get_result_types(main_function, module_inst, result_types);
        if (result_types[0] != WASM_I32)
        {
            script_set_result_message(&context->result,
                                      "builtin entry %s must return a 32-bit integer",
                                      entry_name);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    if (param_count == 2)
    {
        err = script_prepare_builtin_argv(context, module_inst, &argv_offset);
        if (err != ESP_OK)
        {
            goto cleanup;
        }

        args[0].kind = WASM_I32;
        args[0].of.i32 = context->argc;
        args[1].kind = WASM_I32;
        args[1].of.i32 = (int32_t)argv_offset;
    }

    exec_env = wasm_runtime_get_exec_env_singleton(module_inst);
    if (!exec_env)
    {
        exec_env = wasm_runtime_create_exec_env(module_inst, SCRIPT_SERVICE_WASM_STACK_SIZE);
        owns_exec_env = exec_env != NULL;
    }
    if (!exec_env)
    {
        script_set_result_message(&context->result, "failed to create builtin execution environment");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // Create the high priority task that will only exist while WAMR runs
    // PRIORITY = tskIDLE_PRIORITY + 1 ensures it preempts the runner.
    xTaskCreatePinnedToCore(script_yield_task,
                            "ScriptYield",
                            2048,
                            (void *)xTaskGetCurrentTaskHandle(),
                            24,
                            &yield_task_handle,
                            1); // Ensure we pin to exactly the same core as ScriptRunner

    if (!wasm_runtime_call_wasm_a(exec_env,
                                  main_function,
                                  result_count,
                                  result_count > 0 ? results : NULL,
                                  param_count,
                                  param_count > 0 ? args : NULL))
    {
        const char *exception = wasm_runtime_get_exception(module_inst);

        script_set_result_message(&context->result,
                                  "%s",
                                  exception && exception[0] ? exception : "builtin script execution failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    context->result.exit_code = result_count > 0 ? results[0].of.i32 : 0;

cleanup:
    if (yield_task_handle != NULL)
    {
        // Delete the yielding task as soon as WAMR completes or fails
        vTaskDelete(yield_task_handle);
    }

    if (argv_offset != 0)
    {
        wasm_runtime_module_free(module_inst, argv_offset);
    }
    if (owns_exec_env)
    {
        wasm_runtime_destroy_exec_env(exec_env);
    }
    return err;
}

static esp_err_t script_execute_module(script_run_context_t *context)
{
    uint8_t *buffer = context->script_buffer;
    uint32_t buffer_size = context->script_buffer_size;
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    char error_buf[128] = {0};
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

    err = script_execute_builtin_module(context, module_inst);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    ESP_EARLY_LOGI(TAG, "exec main returned");

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

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_log_capture_context = context;
    xSemaphoreGive(s_state_mutex);
    context->err = script_execute_module(context);

done:
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_log_capture_context == context)
    {
        s_log_capture_context = NULL;
    }
    xSemaphoreGive(s_state_mutex);

    if (context->log_fd >= 0)
    {
        if (script_trim_log_fd(context->log_fd) != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to trim %s", context->log_path);
        }
    }

    if (thread_env_initialized_here)
    {
        wasm_runtime_destroy_thread_env();
    }

    return NULL;
}

static esp_err_t script_prepare_context(const char *path,
                                        int argc,
                                        const char *const *argv,
                                        script_run_context_t **context_out,
                                        script_service_run_result_t *result)
{
    script_run_context_t *context = NULL;
    esp_err_t err;

    if (context_out)
    {
        *context_out = NULL;
    }

    script_init_result(result);

    err = script_service_init();
    if (err != ESP_OK)
    {
        script_set_result_message(result, "script service init failed");
        return err;
    }

    if (!path || path[0] == '\0')
    {
        script_set_result_message(result, "script name is required");
        return ESP_ERR_INVALID_ARG;
    }

    context = calloc(1, sizeof(*context));
    if (!context)
    {
        script_set_result_message(result, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    context->completion_sem = xSemaphoreCreateBinaryStatic(&context->completion_sem_storage);
    if (!context->completion_sem)
    {
        script_set_result_message(result, "failed to create script completion semaphore");
        script_free_context(context);
        return ESP_ERR_NO_MEM;
    }

    context->argc = 0;
    context->mode = SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN;
    context->log_fd = -1;
    context->err = ESP_OK;
    script_init_result(&context->result);
    snprintf(context->requested_path, sizeof(context->requested_path), "%s", path);

    err = script_service_resolve_path(path,
                                      context->result.resolved_path,
                                      sizeof(context->result.resolved_path));
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_INVALID_ARG)
        {
            script_set_result_message(&context->result,
                                      "script name must be a bare filename under %s",
                                      script_service_get_root_path());
        }
        else
        {
            script_set_result_message(&context->result, "could not resolve %s", path);
        }
        if (result)
        {
            *result = context->result;
        }
        script_free_context(context);
        return err;
    }

    ESP_LOGI(TAG, "dispatching script %s", context->result.resolved_path);

    context->mode = script_detect_run_mode(context->result.resolved_path);
    context->result.mode = context->mode;

    err = script_prepare_log_file(context);
    if (err != ESP_OK)
    {
        if (result)
        {
            *result = context->result;
        }
        script_free_context(context);
        return err;
    }

    context->argv = script_duplicate_argv(context->result.resolved_path,
                                          argc,
                                          argv,
                                          &context->result,
                                          &context->argc);
    if (!context->argv)
    {
        script_set_result_message(&context->result, "out of memory copying arguments");
        if (result)
        {
            *result = context->result;
        }
        script_free_context(context);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "preloading script %s", context->result.resolved_path);
    context->script_buffer = script_load_file(context->result.resolved_path,
                                              &context->script_buffer_size,
                                              &context->result);
    if (!context->script_buffer)
    {
        if (result)
        {
            *result = context->result;
        }
        script_free_context(context);
        return ESP_FAIL;
    }

    if (result)
    {
        *result = context->result;
    }

    if (context_out)
    {
        *context_out = context;
    }

    return ESP_OK;
}

static esp_err_t script_queue_context(script_run_context_t *context)
{
    uint32_t run_id;

    if (!context)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_status == SCRIPT_SERVICE_STATUS_BUSY || s_active_run_context || s_pending_context)
    {
        if (s_active_run_context && s_active_run_context->result.resolved_path[0] != '\0')
        {
            script_set_result_message(&context->result,
                                      "script service is already running %s",
                                      s_active_run_context->result.resolved_path);
        }
        else
        {
            script_set_result_message(&context->result, "script service is already busy");
        }
        context->err = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    run_id = s_next_run_id++;
    if (s_next_run_id == 0)
    {
        s_next_run_id = 1;
    }

    context->result.run_id = run_id;
    s_status = SCRIPT_SERVICE_STATUS_BUSY;
    s_active_run_context = context;
    s_pending_context = context;
    s_active_run_started_us = esp_timer_get_time();
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "queued script %s", context->result.resolved_path);
    xSemaphoreGive(s_worker_request_sem);
    return ESP_OK;
}

static void *script_worker_task(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        script_run_context_t *context = NULL;

        xSemaphoreTake(s_worker_request_sem, portMAX_DELAY);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        context = s_pending_context;
        s_pending_context = NULL;
        xSemaphoreGive(s_state_mutex);

        if (context)
        {
            ESP_LOGI(TAG, "worker picked up %s", context->result.resolved_path);
            script_runner_task(context);

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            script_capture_last_result_locked(context);
            if (s_active_run_context == context)
            {
                s_active_run_context = NULL;
            }
            s_status = SCRIPT_SERVICE_STATUS_READY;
            xSemaphoreGive(s_state_mutex);

            xSemaphoreGive(context->completion_sem);
            ESP_LOGI(TAG, "worker completed %s", context->result.resolved_path);

            if (context->detach_on_finish)
            {
                script_free_context(context);
            }
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

    s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_storage);
    if (!s_state_mutex)
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

        if (!script_socket_env_register())
        {
            s_status = SCRIPT_SERVICE_STATUS_ERROR;
            return ESP_FAIL;
        }

        s_natives_registered = true;
    }

    script_ensure_directory(SCRIPT_LFS_ROOT_PATH);

    if (!s_worker_thread_started)
    {
        esp_pthread_cfg_t worker_cfg = esp_pthread_get_default_config();
        esp_pthread_cfg_t restore_cfg = esp_pthread_get_default_config();

        worker_cfg.stack_size = SCRIPT_SERVICE_RUNNER_STACK_SIZE;
        worker_cfg.prio = SCRIPT_SERVICE_RUNNER_PRIORITY;
        worker_cfg.inherit_cfg = false;
        worker_cfg.thread_name = "ScriptRunner";
        worker_cfg.pin_to_core = 1;

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
    return script_service_get_status() == SCRIPT_SERVICE_STATUS_READY ||
           script_service_get_status() == SCRIPT_SERVICE_STATUS_BUSY;
}

script_service_status_t script_service_get_status(void)
{
    script_service_status_t status;

    if (!s_state_mutex)
    {
        return s_status;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    status = s_status;
    xSemaphoreGive(s_state_mutex);
    return status;
}

esp_err_t script_service_get_status_snapshot(script_service_status_snapshot_t *snapshot)
{
    if (!snapshot)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->status = s_status;

    if (!s_state_mutex)
    {
        return ESP_OK;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot->status = s_status;
    if (s_active_run_context)
    {
        snapshot->has_active_run = true;
        snapshot->active_run = s_active_run_context->result;
        snapshot->active_run_started_ms = s_active_run_started_us / 1000;
    }
    if (s_last_result_valid)
    {
        snapshot->has_last_run = true;
        snapshot->last_run = s_last_result;
        snapshot->last_run_err = s_last_run_err;
        snapshot->last_run_finished_ms = s_last_run_finished_us / 1000;
    }
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
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

const char *script_service_run_mode_name(script_service_run_mode_t mode)
{
    switch (mode)
    {
    case SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN:
        return "libc-builtin";
    default:
        return "unknown";
    }
}

const char *script_service_get_root_path(void)
{
    return SCRIPT_LFS_ROOT_PATH;
}

esp_err_t script_service_get_log_path(const char *path, char *out_path, size_t out_path_size)
{
    char resolved_path[SCRIPT_SERVICE_MAX_PATH_LEN];
    esp_err_t err;

    if (!path || !out_path || out_path_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = script_service_resolve_path(path, resolved_path, sizeof(resolved_path));
    if (err != ESP_OK)
    {
        return err;
    }

    return script_build_log_path_from_resolved_path(resolved_path, out_path, out_path_size);
}

esp_err_t script_service_get_script_directory(const char *name,
                                              char *out_path,
                                              size_t out_path_size)
{
    return script_build_script_directory(name, out_path, out_path_size);
}

esp_err_t script_service_resolve_path(const char *path, char *out_path, size_t out_path_size)
{
    char candidate[SCRIPT_SERVICE_MAX_PATH_LEN];
    bool has_extension = false;
    esp_err_t err;

    if (!path || path[0] == '\0' || !out_path || out_path_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = script_build_resolve_base_path(path, candidate, sizeof(candidate), &has_extension);
    if (err != ESP_OK)
    {
        return err;
    }

    if (has_extension)
    {
        return script_copy_candidate(candidate, out_path, out_path_size) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    return script_try_path_variants(candidate, out_path, out_path_size) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t script_service_start(const char *path,
                               int argc,
                               const char *const *argv,
                               script_service_run_result_t *result)
{
    script_service_run_result_t local_result;
    script_service_run_result_t *effective_result = result ? result : &local_result;
    script_run_context_t *context = NULL;
    esp_err_t err;

    err = script_prepare_context(path, argc, argv, &context, effective_result);
    if (err != ESP_OK)
    {
        return err;
    }

    context->detach_on_finish = true;
    err = script_queue_context(context);
    if (err != ESP_OK)
    {
        *effective_result = context->result;
        script_free_context(context);
        return err;
    }

    *effective_result = context->result;
    return ESP_OK;
}

esp_err_t script_service_run(const char *path,
                             int argc,
                             const char *const *argv,
                             script_service_run_result_t *result)
{
    script_service_run_result_t local_result;
    script_service_run_result_t *effective_result = result ? result : &local_result;
    script_run_context_t *context = NULL;
    esp_err_t err;

    err = script_prepare_context(path, argc, argv, &context, effective_result);
    if (err != ESP_OK)
    {
        return err;
    }

    context->detach_on_finish = false;
    err = script_queue_context(context);
    if (err != ESP_OK)
    {
        *effective_result = context->result;
        script_free_context(context);
        return err;
    }

    xSemaphoreTake(context->completion_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "script wait complete for %s", context->result.resolved_path);

    *effective_result = context->result;
    err = context->err;
    script_free_context(context);
    return err;
}