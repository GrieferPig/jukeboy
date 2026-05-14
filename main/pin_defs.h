#pragma once

#include "driver/gpio.h"

/* I2S DAC pin assignments */
#define HAL_I2S_BCLK_PIN GPIO_NUM_19
#define HAL_I2S_WS_PIN GPIO_NUM_21
#define HAL_I2S_DATA_PIN GPIO_NUM_20

/* Power gating and LED assignments */
#define HAL_DAC_POWER_GATE_PIN GPIO_NUM_NC
#define HAL_DAC_MUTE_PIN GPIO_NUM_NC
#define HAL_LED_GATE_PIN GPIO_NUM_NC
#define HAL_LED_DATA_PIN GPIO_NUM_48

/* User inputs */
#define HAL_MAIN_BTN_PIN GPIO_NUM_1
#define HAL_MISC_BTN_PIN GPIO_NUM_4

/* Deferred inputs */
#define HAL_INS_SW_A_PIN GPIO_NUM_NC
#define HAL_INS_SW_B_PIN GPIO_NUM_NC
#define HAL_CHRG_PIN GPIO_NUM_NC

/* SDIO 1bit SD card pin assignments */
#define HAL_SD_DAT0_PIN GPIO_NUM_13
#define HAL_SD_CMD_PIN GPIO_NUM_11
#define HAL_SD_CLK_PIN GPIO_NUM_12
