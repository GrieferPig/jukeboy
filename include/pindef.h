#pragma once
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <driver/adc.h>

#define BTN1_GPIO GPIO_NUM_13          // Button 1 (GPIO 13)
#define BTN2_GPIO GPIO_NUM_14          // Button 2 (GPIO 14)
#define BTN3_GPIO GPIO_NUM_27          // Button 3 (GPIO 27)
#define BTN4_GPIO GPIO_NUM_26          // Button 4 (GPIO 26)
#define BTN5_GPIO GPIO_NUM_25          // Button 5 (GPIO 25)
#define BTN6_GPIO GPIO_NUM_32          // Button 6 (GPIO 32)
#define CART_PRESENCE_GPIO GPIO_NUM_35 // Cart Presence (GPIO 35)

#define WS2812_LED_GPIO GPIO_NUM_5 // WS2812 LED (GPIO 5)

#define LDO_EN_GPIO GPIO_NUM_15  // LDO Enable (GPIO 15)
#define BAT_ADC_GPIO GPIO_NUM_36 // Battery ADC (GPIO 36)

#define STDBY_STATUS_GPIO GPIO_NUM_39 // BMC Standby Status (GPIO 39) (USE ADC to read)
#define CHRG_STATUS_GPIO GPIO_NUM_34  // BMC Charge Status (GPIO 34)

#define BAT_ADC_CHANNEL ADC_CHANNEL_0
#define STDBY_ADC_CHANNEL ADC_CHANNEL_3

// ADC configuration parameters
#define ADC_UNIT ADC_UNIT_1
#define ADC_ATTEN ADC_ATTEN_DB_12
#define ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define ADC_POLL_INTERVAL_MS 3000
#define ADC_THRESHOLD 2700

#ifdef ESP32_WROOM
#define I2S_BCK_PIN GPIO_NUM_2
#define I2S_WS_PIN GPIO_NUM_4
#define I2S_DOUT_PIN GPIO_NUM_12
#define I2S_PORT I2S_NUM_1
#else
#define I2S_BCK_PIN GPIO_NUM_5
#define I2S_DOUT_PIN GPIO_NUM_6
#define I2S_WS_PIN GPIO_NUM_7
#endif

// SD Card SPI Pins (Update these to your board's configuration)
#ifdef ESP32_WROOM
#define SD_CS_PIN 33
#define SD_SCK_PIN 18
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#else
#define SD_CS_PIN 3
#define SD_SCK_PIN 1
#define SD_MOSI_PIN 2
#define SD_MISO_PIN 0
#endif