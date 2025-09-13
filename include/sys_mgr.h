#pragma once

#include <esp_err.h>

// Initializes the system manager task. The task reads eFuse BLOCK3
// on startup, parses the custom metadata, logs it, then self-deletes.
esp_err_t sys_mgr_init(void);
