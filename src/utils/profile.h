#pragma once
#include <freertos/FreeRTOS.h>

static TaskHandle_t g_profileTaskHandle = NULL; // Handle for the profile task

void profiler_task(void *pvParameters);

BaseType_t profiler_init();