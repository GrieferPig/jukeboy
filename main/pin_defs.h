#pragma once

#include "driver/gpio.h"

/* I2S DAC pin assignments */
#define HAL_I2S_BCLK_PIN GPIO_NUM_26
#define HAL_I2S_WS_PIN GPIO_NUM_25
#define HAL_I2S_DATA_PIN GPIO_NUM_22

/* SPI SD card pin assignments */
#define HAL_SD_SPI_MOSI_PIN GPIO_NUM_23
#define HAL_SD_SPI_MISO_PIN GPIO_NUM_19
#define HAL_SD_SPI_CLK_PIN GPIO_NUM_18
#define HAL_SD_SPI_CS_PIN GPIO_NUM_5
