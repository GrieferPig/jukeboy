/*
Copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Original Author: Shay Gal-on
*/

#include <stdarg.h>
#include <stdio.h>

#include "coremark.h"

__attribute__((import_module("jukeboy")))
__attribute__((import_name("log")))
int jukeboy_log(const char *message);

__attribute__((import_module("jukeboy")))
__attribute__((import_name("get_uptime_ms")))
long long jukeboy_get_uptime_ms(void);

#define EE_TICKS_PER_SEC 1000U

static CORETIMETYPE start_time_val;
static CORETIMETYPE stop_time_val;

static CORETIMETYPE barebones_clock(void)
{
    long long uptime_ms = jukeboy_get_uptime_ms();
    if (uptime_ms < 0)
    {
        return 0;
    }
    return (CORETIMETYPE)uptime_ms;
}

int ee_printf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written > 0)
    {
        jukeboy_log(buffer);
    }

    return written;
}

void start_time(void)
{
    start_time_val = barebones_clock();
}

void stop_time(void)
{
    stop_time_val = barebones_clock();
}

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return (secs_ret)(ticks / EE_TICKS_PER_SEC);
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
    {
        ee_printf("ERROR! Please define ee_ptr_int to a type that holds a pointer!\n");
    }
    if (sizeof(ee_u32) != 4)
    {
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    }

    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    p->portable_id = 0;
}