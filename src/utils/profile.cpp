#include "profile.h"
#include "esp_log.h"
#include "audio/audio_internals.h" // For task handles

void profiler_task(void *pvParameters)
{
    // Delay start if any or the task handle is not initialized
    char stats_buffer[2048]; // Buffer to hold the formatted stats string

    while (true)
    {
        // Wait for 5 seconds before printing stats again
        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_LOGI("Profiler", "--- TASK RUN-TIME STATS ---");
        vTaskGetRunTimeStats(stats_buffer);
        printf("%s\n", stats_buffer);
        ESP_LOGI("Profiler", "---------------------------\n");

        ESP_LOGI("Profiler", "--- TASK STACK HIGH WATER MARK (FREE STACK) ---");

        // Get the number of tasks
        UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
        TaskStatus_t *task_status_array = (TaskStatus_t *)pvPortMalloc(num_tasks * sizeof(TaskStatus_t));
        uint32_t total_runtime;

        if (task_status_array != NULL)
        {
            // Get the state of all tasks
            num_tasks = uxTaskGetSystemState(task_status_array, num_tasks, &total_runtime);

            for (UBaseType_t i = 0; i < num_tasks; i++)
            {
                ESP_LOGI("Profiler", "%-20s: %u bytes free",
                         task_status_array[i].pcTaskName,
                         task_status_array[i].usStackHighWaterMark);
            }

            vPortFree(task_status_array);
        }
        ESP_LOGI("Profiler", "--- HEAP MEMORY STATS ---");
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = esp_get_minimum_free_heap_size();
        size_t used_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT) - free_heap;
        ESP_LOGI("Profiler", "Free heap: %u bytes", free_heap);
        ESP_LOGI("Profiler", "Used heap: %u bytes", used_heap);
        ESP_LOGI("Profiler", "Min free heap (lowest point): %u bytes", total_heap);
        ESP_LOGI("Profiler", "-------------------------\n");
    }
}

BaseType_t profiler_start()
{
    // Create the profiler task
    return xTaskCreate(profiler_task, "Profiler", 4096, NULL, 5, &g_profileTaskHandle);
}
