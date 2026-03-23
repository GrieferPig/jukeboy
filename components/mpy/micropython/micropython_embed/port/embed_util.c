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

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "py/builtin.h"
#include "py/compile.h"
#include "py/mperrno.h"
#include "py/gc.h"
#include "py/persistentcode.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/gchelper.h"
#include "port/micropython_embed.h"

mp_obj_t mp_embed_new_int(mp_int_t value)
{
    return mp_obj_new_int(value);
}

mp_obj_t mp_embed_new_str(const char *str)
{
    return mp_obj_new_str(str, strlen(str));
}

mp_obj_t mp_embed_new_bytes(const void *data, size_t len)
{
    return mp_obj_new_bytes(data, len);
}

void mp_embed_set_global(const char *name, mp_obj_t value)
{
    mp_obj_t key = mp_obj_new_str(name, strlen(name));
    mp_obj_t globals_dict = MP_OBJ_FROM_PTR(mp_globals_get());
    mp_obj_dict_store(globals_dict, key, value);
}

#if MICROPY_GC_SPLIT_HEAP_AUTO

typedef struct _mp_embed_heap_area_t
{
    struct _mp_embed_heap_area_t *next;
    void *ptr;
    size_t size;
} mp_embed_heap_area_t;

static mp_embed_heap_area_t *mp_embed_heap_areas;
static size_t mp_embed_heap_total_bytes;

static void mp_embed_heap_free_all(void)
{
    while (mp_embed_heap_areas != NULL)
    {
        mp_embed_heap_area_t *entry = mp_embed_heap_areas;
        mp_embed_heap_areas = entry->next;
        free(entry->ptr);
        free(entry);
    }
    mp_embed_heap_total_bytes = 0;
}

#if !defined(MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES)
#define MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES (SIZE_MAX)
#endif

size_t gc_get_max_new_split(void)
{
    if (MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES <= mp_embed_heap_total_bytes)
    {
        return 0;
    }
    return MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES - mp_embed_heap_total_bytes;
}

void *mp_embed_alloc_heap(size_t size)
{
    if (size == 0 || gc_get_max_new_split() < size)
    {
        return NULL;
    }

    mp_embed_heap_area_t *area = malloc(sizeof(*area));
    if (area == NULL)
    {
        return NULL;
    }

    void *ptr = malloc(size);
    if (ptr == NULL)
    {
        free(area);
        return NULL;
    }

    area->ptr = ptr;
    area->size = size;
    area->next = mp_embed_heap_areas;
    mp_embed_heap_areas = area;
    mp_embed_heap_total_bytes += size;
    return ptr;
}

void mp_embed_free_heap(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    mp_embed_heap_area_t **area = &mp_embed_heap_areas;
    while (*area != NULL)
    {
        if ((*area)->ptr == ptr)
        {
            mp_embed_heap_area_t *entry = *area;
            *area = entry->next;
            mp_embed_heap_total_bytes -= entry->size;
            free(entry->ptr);
            free(entry);
            return;
        }
        area = &(*area)->next;
    }

    free(ptr);
}

#endif // MICROPY_GC_SPLIT_HEAP_AUTO

// Initialise the runtime.
void mp_embed_init(void *gc_heap, size_t gc_heap_size, void *stack_top)
{
#if MICROPY_GC_SPLIT_HEAP_AUTO
    mp_embed_heap_free_all();
#endif
    mp_stack_set_top(stack_top);
    gc_init(gc_heap, (uint8_t *)gc_heap + gc_heap_size);
    mp_init();
}

#if MICROPY_ENABLE_COMPILER
// Compile and execute the given source script (Python text).
void mp_embed_exec_str_with_filename(const char *src, const char *filename)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0)
    {
        // Compile, parse and execute the given string.
        qstr source_name = filename ? qstr_from_str(filename) : MP_QSTR__lt_stdin_gt_;
        mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, src, strlen(src), 0);
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
    }
    else
    {
        // Uncaught exception: print it out.
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}

void mp_embed_exec_str(const char *src)
{
    mp_embed_exec_str_with_filename(src, NULL);
}
#endif

#if MICROPY_PERSISTENT_CODE_LOAD
void mp_embed_exec_mpy(const uint8_t *mpy, size_t len)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0)
    {
        // Execute the given .mpy data.
        mp_module_context_t *ctx = m_new_obj(mp_module_context_t);
        ctx->module.globals = mp_globals_get();
        mp_compiled_module_t cm;
        cm.context = ctx;
        mp_raw_code_load_mem(mpy, len, &cm);
        mp_obj_t f = mp_make_function_from_proto_fun(cm.rc, ctx, MP_OBJ_NULL);
        mp_call_function_0(f);
        nlr_pop();
    }
    else
    {
        // Uncaught exception: print it out.
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}
#endif

#if !MICROPY_VFS

mp_import_stat_t mp_import_stat(const char *path)
{
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

static mp_obj_t mp_embed_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    (void)n_args;
    (void)args;
    (void)kwargs;
    mp_raise_OSError(MP_EOPNOTSUPP);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_embed_builtin_open);

#endif

// Deinitialise the runtime.
void mp_embed_deinit(void)
{
    mp_deinit();
#if MICROPY_GC_SPLIT_HEAP_AUTO
    mp_embed_heap_free_all();
#endif
}

#if MICROPY_ENABLE_GC
// Run a garbage collection cycle.
void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}
#endif

// Called if an exception is raised outside all C exception-catching handlers.
void nlr_jump_fail(void *val)
{
    for (;;)
    {
    }
}

#if !defined(__XTENSA__) && !defined(__riscv)
#ifndef NDEBUG
// Used when debugging is enabled.
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    for (;;)
    {
    }
}
#endif
#endif
