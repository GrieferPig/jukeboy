#pragma once

#include "driver/gpio.h"

/* I2S DAC pin assignments */
#define HAL_I2S_BCLK_PIN GPIO_NUM_19
#define HAL_I2S_WS_PIN GPIO_NUM_21
#define HAL_I2S_DATA_PIN GPIO_NUM_20

/* ESP32 A2DP coprocessor control UART */
#define HAL_A2DP_UART_TX_PIN GPIO_NUM_6
#define HAL_A2DP_UART_RX_PIN GPIO_NUM_7
#define HAL_A2DP_EN_PIN GPIO_NUM_9

/* Power gating and LED assignments */
#define HAL_DAC_POWER_GATE_PIN GPIO_NUM_NC
#define HAL_DAC_MUTE_PIN GPIO_NUM_NC
#define HAL_LED_GATE_PIN GPIO_NUM_NC
#define HAL_LED_DATA_PIN GPIO_NUM_48

/* User inputs */
#define HAL_MAIN_BTN_PIN GPIO_NUM_1
#define HAL_MISC_BTN_PIN GPIO_NUM_4
#define HAL_SIDE_BTN_PIN GPIO_NUM_5

/* SH1106/SSD1306 SPI OLED pin assignments */
#define HAL_DISPLAY_SPI_CLK_PIN GPIO_NUM_8
#define HAL_DISPLAY_SPI_MOSI_PIN GPIO_NUM_18
#define HAL_DISPLAY_RESET_PIN GPIO_NUM_17
#define HAL_DISPLAY_DC_PIN GPIO_NUM_16
#define HAL_DISPLAY_CS_PIN GPIO_NUM_15

/* Deferred inputs */
#define HAL_INS_SW_A_PIN GPIO_NUM_NC
#define HAL_INS_SW_B_PIN GPIO_NUM_NC
#define HAL_CHRG_PIN GPIO_NUM_NC

/* SDIO 1bit SD card pin assignments */
#define HAL_SD_DAT0_PIN GPIO_NUM_13
#define HAL_SD_CMD_PIN GPIO_NUM_11
#define HAL_SD_CLK_PIN GPIO_NUM_12
