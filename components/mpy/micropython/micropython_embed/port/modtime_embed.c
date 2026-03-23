/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024
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
#include "py/mphal.h"
#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

static uint64_t mp_embed_time_seconds(void)
{
    return mp_hal_time_ns() / 1000000000ULL;
}

static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
    timeutils_seconds_since_epoch_to_struct_time(mp_embed_time_seconds(), tm);
}

static mp_obj_t mp_time_time_get(void)
{
#if MICROPY_PY_BUILTINS_FLOAT
    mp_float_t seconds = (mp_float_t)mp_hal_time_ns() / 1000000000.0f;
    return mp_obj_new_float(seconds);
#else
    return mp_obj_new_int_from_ull(mp_embed_time_seconds());
#endif
}
