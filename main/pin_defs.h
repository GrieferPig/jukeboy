#pragma once

#include "driver/gpio.h"

/* I2S DAC pin assignments */
#define HAL_I2S_BCLK_PIN GPIO_NUM_5
#define HAL_I2S_WS_PIN GPIO_NUM_4
#define HAL_I2S_DATA_PIN GPIO_NUM_23

/* Power gating and LED assignments */
#define HAL_DAC_POWER_GATE_PIN GPIO_NUM_33
#define HAL_DAC_MUTE_PIN GPIO_NUM_12
#define HAL_LED_GATE_PIN GPIO_NUM_21
#define HAL_LED_DATA_PIN GPIO_NUM_19

/* User inputs */
#define HAL_MAIN_BTN_PIN GPIO_NUM_35
#define HAL_MISC_BTN_PIN GPIO_NUM_34

/* Deferred inputs */
#define HAL_INS_SW_A_PIN GPIO_NUM_27
#define HAL_INS_SW_B_PIN GPIO_NUM_18
#define HAL_CHRG_PIN GPIO_NUM_39

/* SPI SD card pin assignments */
#define HAL_SD_SPI_MOSI_PIN GPIO_NUM_23
#define HAL_SD_SPI_MISO_PIN GPIO_NUM_19
#define HAL_SD_SPI_CLK_PIN GPIO_NUM_18
#define HAL_SD_SPI_CS_PIN GPIO_NUM_5
