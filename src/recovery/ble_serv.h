#ifdef BUILD_FACTORY_APP

#ifndef BLE_SERV_H
#define BLE_SERV_H

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef __cplusplus
extern "C"
{
#endif

    extern SemaphoreHandle_t cmd_done_sem; // 用于命令完成的信号量

    /**
     * @brief Initialize the BLE service for OTA functionality
     *
     * This function initializes the NimBLE stack, sets up GATT services,
     * and starts advertising the BLE service for OTA data reception.
     * The service creates a 128KB buffer for receiving OTA data chunks
     * via BLE characteristics.
     */
    void ble_serv_init(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_SERV_H

#endif