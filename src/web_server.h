#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Forward declaration for the web server task
void webServerTask(void *pvParameter);

#endif // WEB_SERVER_H
