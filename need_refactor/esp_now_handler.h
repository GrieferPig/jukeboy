#ifndef ESP_NOW_HANDLER_H
#define ESP_NOW_HANDLER_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_log.h" // Added for ESP_LOGx macros
#include <stdio.h>   // For snprintf
#include <string.h>  // For strlen, memcpy
#include <algorithm> // For std::min
#include <esp_wifi.h>
#include <stddef.h> // For offsetof
#include <time.h>   // For RTC date/time functions

// Define the number of times to send each broadcast message
#define BROADCAST_REPETITIONS 16
#define DELAY_BETWEEN_REPETITIONS_MS 10 // Delay between burst sends

// Define max number of unique encounters to store
#define MAX_STORED_ENCOUNTERS 50

// Define message types
typedef enum
{
    HELLO_MSG = 0x01,
    ACK_MSG = 0x02 // Kept for compatibility, though ACKs are not actively used
} ESPNOW_MSG_TYPE;

// Define the structure for messages
typedef struct espnow_message
{
    uint16_t crc16; // CRC16 checksum over type and content
    // uint8_t device_id[6]; // REMOVED - Sender's MAC address obtained from esp_now_info->src_addr
    uint8_t type;         // ESPNOW_MSG_TYPE_HELLO or ESPNOW_MSG_TYPE_ACK
    uint8_t content[247]; // Binary content (250 total - 2 crc - 1 type = 247)
} espnow_message_t;

// Structure to store information about received HELLO messages
typedef struct EncounterInfo
{
    uint8_t device_id[6];
    uint8_t content[sizeof(espnow_message_t::content)]; // Store the content of the message
    size_t content_length;                              // Actual length of the stored content
    uint16_t year;
    uint8_t month;
    uint8_t day;
} EncounterInfo_t;

// Static global variables for the ESP-NOW handler
static uint8_t espNowBroadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t espNowDeviceMac[6];  // Stores this device's MAC address in binary (used for logging own MAC)
static char espNowDeviceMacStr[18]; // Stores this device's MAC address as string for logging
static espnow_message_t espNowOutgoingData;
static espnow_message_t espNowIncomingData; // Used for full packet copy after early checks

// Storage for unique daily encounters
static EncounterInfo_t storedEncounters[MAX_STORED_ENCOUNTERS];
static int nextEncounterSlot = 0; // Index for circular buffer

// Define a TAG for ESP-NOW Handler logging
static const char *TAG_ESPNOW = "EspNowLib";

// Peer info for broadcast
static esp_now_peer_info_t espNowBroadcastPeer;

// Helper function to get current date from RTC
// IMPORTANT: ESP32's RTC must be initialized (e.g., via NTP or settimeofday) for this to be accurate.
static void getCurrentDate(uint16_t &year, uint8_t &month, uint8_t &day)
{
    time_t now_time;
    struct tm timeinfo;
    time(&now_time);                   // Get current time
    localtime_r(&now_time, &timeinfo); // Convert to local time structure

    year = timeinfo.tm_year + 1900; // tm_year is years since 1900
    month = timeinfo.tm_mon + 1;    // tm_mon is 0-11
    day = timeinfo.tm_mday;         // tm_mday is 1-31

    // // Basic check if RTC might not be set (e.g. year is too low)
    // if (year < 2023)
    // { // If year is before a reasonable date, RTC might not be set
    //     ESP_LOGW(TAG_ESPNOW, "RTC date seems uninitialized (%04d-%02d-%02d). Please ensure RTC is set.", year, month, day);
    //     // Default to a placeholder if RTC is not set to avoid issues, or handle error
    //     // For now, we'll proceed, but real applications should handle this robustly.
    // }
}

// CRC-16-CCITT (XMODEM) Calculation function
// Polynomial: 0x1021, Initial Value: 0x0000
static uint16_t calculate_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    const uint16_t poly = 0x1021;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ poly;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// Callback function when data is sent
static void OnDataSentEspNowInternal(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS)
    {
        // ESP_LOGW(TAG_ESPNOW, "Packet Send Status: Delivery Fail for %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
    }
}

// Callback function when data is received
static void OnDataRecvEspNowInternal(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingDataPtr, int len)
{
    char senderMacStr[18];
    const uint8_t *sender_mac_addr = esp_now_info->src_addr; // Use MAC from ESP-NOW info

    snprintf(senderMacStr, sizeof(senderMacStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             sender_mac_addr[0], sender_mac_addr[1], sender_mac_addr[2], sender_mac_addr[3], sender_mac_addr[4], sender_mac_addr[5]);

    if (incomingDataPtr == NULL || len == 0)
    {
        ESP_LOGW(TAG_ESPNOW, "Received empty or NULL data from %s.", senderMacStr);
        return;
    }

    if (len != sizeof(espnow_message_t))
    {
        ESP_LOGW(TAG_ESPNOW, "Incorrect packet length from %s. Expected %d, Got: %d. Discarding.", senderMacStr, sizeof(espnow_message_t), len);
        return;
    }

    // --- Early Check for HELLO_MSG Duplicates ---
    // Structure: crc16 (2 bytes), type (1 byte), content (...)
    // Offset for type is sizeof(uint16_t)
    uint8_t early_message_type = incomingDataPtr[sizeof(uint16_t)]; // Type is now the first field after CRC

    if (early_message_type == HELLO_MSG)
    {
        uint16_t current_year;
        uint8_t current_month, current_day;
        getCurrentDate(current_year, current_month, current_day);

        bool already_seen_today = false;
        for (int i = 0; i < MAX_STORED_ENCOUNTERS; ++i)
        {
            if (storedEncounters[i].year != 0 &&                                  // Check if slot has been used
                memcmp(storedEncounters[i].device_id, sender_mac_addr, 6) == 0 && // Compare with sender_mac_addr
                storedEncounters[i].year == current_year &&
                storedEncounters[i].month == current_month &&
                storedEncounters[i].day == current_day)
            {
                already_seen_today = true;
                break;
            }
        }

        if (already_seen_today)
        {
            // ESP_LOGI(TAG_ESPNOW, "HELLO from %s already recorded today (%04d-%02d-%02d). Ignoring.",
            //          senderMacStr, current_year, current_month, current_day);
            return; // Early exit
        }
    }
    // --- End of Early Check ---

    // Proceed with full packet copy and validation if not an early exit
    memcpy(&espNowIncomingData, incomingDataPtr, sizeof(espnow_message_t));

    uint16_t received_crc = espNowIncomingData.crc16;
    // CRC is now calculated over 'type' and 'content'
    const uint8_t *payload_to_verify_ptr = &espNowIncomingData.type; // Start of data for CRC is the 'type' field
    size_t payload_to_verify_len = sizeof(espNowIncomingData.type) + sizeof(espNowIncomingData.content);
    uint16_t calculated_crc = calculate_crc16_ccitt(payload_to_verify_ptr, payload_to_verify_len);

    if (received_crc != calculated_crc)
    {
        ESP_LOGW(TAG_ESPNOW, "CRC mismatch from %s! Received: 0x%04X, Calculated: 0x%04X. Discarding packet.", senderMacStr, received_crc, calculated_crc);
        return;
    }
    ESP_LOGI(TAG_ESPNOW, "CRC OK (0x%04X) for packet from %s (RSSI: %d)", received_crc, senderMacStr, esp_now_info->rx_ctrl->rssi);

    // Process based on type from the fully copied and validated espNowIncomingData
    if (espNowIncomingData.type == HELLO_MSG)
    {
        // At this point, we know it's a HELLO_MSG and it's NOT a duplicate for today (due to the early check).
        // So, we just store it.
        uint16_t current_year;
        uint8_t current_month, current_day;
        getCurrentDate(current_year, current_month, current_day); // Get date again, or pass from early check if optimized

        ESP_LOGI(TAG_ESPNOW, "New HELLO encounter from %s on %04d-%02d-%02d. Storing.",
                 senderMacStr, current_year, current_month, current_day);

        EncounterInfo_t *slot = &storedEncounters[nextEncounterSlot];
        memcpy(slot->device_id, sender_mac_addr, 6); // Store sender's MAC from esp_now_info

        size_t incoming_content_len = strnlen((char *)espNowIncomingData.content, sizeof(espNowIncomingData.content));
        memcpy(slot->content, espNowIncomingData.content, incoming_content_len);
        if (incoming_content_len < sizeof(slot->content))
        {
            slot->content[incoming_content_len] = '\0';
        }
        slot->content_length = incoming_content_len;

        slot->year = current_year;
        slot->month = current_month;
        slot->day = current_day;

        ESP_LOGD(TAG_ESPNOW, "Stored content from %s: \"%.*s\"", senderMacStr, (int)slot->content_length, (char *)slot->content);
        nextEncounterSlot = (nextEncounterSlot + 1) % MAX_STORED_ENCOUNTERS;
    }
    else if (espNowIncomingData.type == ACK_MSG)
    {
        ESP_LOGI(TAG_ESPNOW, "Received ACK from %s. (Note: This device does not process ACKs). Content: \"%.*s\"", senderMacStr,
                 (int)strnlen((char *)espNowIncomingData.content, sizeof(espNowIncomingData.content)),
                 (char *)espNowIncomingData.content);
    }
    else
    {
        ESP_LOGW(TAG_ESPNOW, "Received unknown message type (0x%02X) from %s.", espNowIncomingData.type, senderMacStr);
    }
}

// FreeRTOS Task to send ESP-NOW messages (HELLO messages only)
static void espNowSendTaskHandler(void *pvParameters)
{
    ESP_LOGI(TAG_ESPNOW, "espNowSendTaskHandler started. Broadcasting HELLO %d times (delay %dms) every 5s.", BROADCAST_REPETITIONS, DELAY_BETWEEN_REPETITIONS_MS);

    espNowOutgoingData.type = HELLO_MSG;

    memset(espNowOutgoingData.content, 0, sizeof(espNowOutgoingData.content));
    const char *hello_content_str = "Hello ESP-NOW!";
    strncpy((char *)espNowOutgoingData.content, hello_content_str, sizeof(espNowOutgoingData.content) - 1);
    espNowOutgoingData.content[sizeof(espNowOutgoingData.content) - 1] = '\0';

    for (;;)
    {
        ESP_LOGI(TAG_ESPNOW, "Broadcasting HELLO burst (%d repetitions)...", BROADCAST_REPETITIONS);

        // CRC is now calculated over 'type' and 'content'
        const uint8_t *payload_ptr = &espNowOutgoingData.type; // Start of data for CRC is the 'type' field
        size_t payload_len = sizeof(espNowOutgoingData.type) + sizeof(espNowOutgoingData.content);
        espNowOutgoingData.crc16 = calculate_crc16_ccitt(payload_ptr, payload_len);

        int success_sends = 0;
        for (int i = 0; i < BROADCAST_REPETITIONS; ++i)
        {
            esp_err_t result = esp_now_send(espNowBroadcastAddress, (uint8_t *)&espNowOutgoingData, sizeof(espNowOutgoingData));
            if (result == ESP_OK)
            {
                success_sends++;
            }
            else
            {
                ESP_LOGW(TAG_ESPNOW, "HELLO broadcast %d/%d failed: %s", i + 1, BROADCAST_REPETITIONS, esp_err_to_name(result));
            }
            vTaskDelay(pdMS_TO_TICKS(DELAY_BETWEEN_REPETITIONS_MS));
        }
        ESP_LOGI(TAG_ESPNOW, "HELLO burst complete. Successful sends: %d/%d", success_sends, BROADCAST_REPETITIONS);

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// Initialization function for the ESP-NOW Communications
static bool initEspNowComms()
{
    for (int i = 0; i < MAX_STORED_ENCOUNTERS; ++i)
    {
        memset(&storedEncounters[i], 0, sizeof(EncounterInfo_t));
    }
    nextEncounterSlot = 0;

    ESP_LOGI(TAG_ESPNOW, "RTC should be initialized for correct date stamping of encounters.");

    WiFi.macAddress(espNowDeviceMac);
    snprintf(espNowDeviceMacStr, sizeof(espNowDeviceMacStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             espNowDeviceMac[0], espNowDeviceMac[1], espNowDeviceMac[2], espNowDeviceMac[3], espNowDeviceMac[4], espNowDeviceMac[5]);

    ESP_LOGI(TAG_ESPNOW, "Initializing. This device's MAC Address: %s", espNowDeviceMacStr);

    if (esp_now_init() != ESP_OK)
    {
        ESP_LOGE(TAG_ESPNOW, "Error initializing ESP-NOW");
        return false;
    }
    ESP_LOGI(TAG_ESPNOW, "ESP-NOW Initialized.");

    esp_err_t lr_set_result = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (lr_set_result == ESP_OK)
    {
        ESP_LOGI(TAG_ESPNOW, "Wi-Fi Long Range protocol set successfully for STA interface.");
    }
    else
    {
        ESP_LOGW(TAG_ESPNOW, "Failed to set Wi-Fi Long Range protocol for STA interface: %s.", esp_err_to_name(lr_set_result));
    }

    if (esp_now_register_send_cb(OnDataSentEspNowInternal) != ESP_OK)
    {
        ESP_LOGE(TAG_ESPNOW, "Error registering send cb");
        return false;
    }
    if (esp_now_register_recv_cb(OnDataRecvEspNowInternal) != ESP_OK)
    {
        ESP_LOGE(TAG_ESPNOW, "Error registering recv cb");
        return false;
    }

    memset(&espNowBroadcastPeer, 0, sizeof(espNowBroadcastPeer));
    memcpy(espNowBroadcastPeer.peer_addr, espNowBroadcastAddress, 6);
    espNowBroadcastPeer.channel = 0;
    espNowBroadcastPeer.ifidx = WIFI_IF_STA;
    espNowBroadcastPeer.encrypt = false;

    if (esp_now_add_peer(&espNowBroadcastPeer) != ESP_OK)
    {
        ESP_LOGE(TAG_ESPNOW, "Failed to add broadcast peer");
        return false;
    }
    ESP_LOGI(TAG_ESPNOW, "Broadcast peer added.");

    BaseType_t sendTaskCreated = xTaskCreate(
        espNowSendTaskHandler,
        "EspNowSendTaskLib",
        4096,
        NULL,
        1,
        NULL);
    if (sendTaskCreated != pdPASS)
    {
        ESP_LOGE(TAG_ESPNOW, "Failed to create send task");
        return false;
    }
    ESP_LOGI(TAG_ESPNOW, "ESPNowSendTask created successfully.");

    ESP_LOGI(TAG_ESPNOW, "Initialization complete. ESP-NOW Handler (MAC from Header, Optimized Storage) is running.");
    return true;
}

#endif // ESP_NOW_HANDLER_H
