/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2023 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "py/mphal.h"

// Send string of given length to stdout, converting \n to \r\n.
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
    printf("%.*s", (int)len, str);
}

#define MP_NS_PER_SECOND (1000000000ULL)
#define MP_NS_PER_MS (1000000ULL)
#define MP_NS_PER_US (1000ULL)

static void mp_embed_sleep_ns(uint64_t ns)
{
    uint64_t us = ns / MP_NS_PER_US;
    if (us == 0 && ns > 0)
    {
        us = 1;
    }

    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        const uint64_t us_per_tick = (uint64_t)portTICK_PERIOD_MS * 1000ULL;
        TickType_t ticks = (TickType_t)((us + us_per_tick - 1) / us_per_tick);
        if (ticks > 0)
        {
            vTaskDelay(ticks);
            return;
        }
    }

    esp_rom_delay_us((uint32_t)us);
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    mp_embed_sleep_ns((uint64_t)ms * MP_NS_PER_MS);
}

void mp_hal_delay_us(mp_uint_t us)
{
    mp_embed_sleep_ns((uint64_t)us * MP_NS_PER_US);
}

static uint64_t mp_embed_ticks_ns(void)
{
    return (uint64_t)esp_timer_get_time() * MP_NS_PER_US;
}

mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t)(mp_embed_ticks_ns() / MP_NS_PER_MS);
}

mp_uint_t mp_hal_ticks_us(void)
{
    return (mp_uint_t)(mp_embed_ticks_ns() / MP_NS_PER_US);
}

mp_uint_t mp_hal_ticks_cpu(void)
{
    return mp_hal_ticks_us();
}

uint64_t mp_hal_time_ns(void)
{
    return mp_embed_ticks_ns();
}
