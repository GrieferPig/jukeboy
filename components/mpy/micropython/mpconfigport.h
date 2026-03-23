/* This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT)
 * Copyright (c) 2022-2023 Damien P. George
 */

// Include common MicroPython embed configuration.
#include "sdkconfig.h"
#include <port/mpconfigport_common.h>

#ifndef __ASSEMBLER__
struct _mp_obj_fun_builtin_var_t;
extern const struct _mp_obj_fun_builtin_var_t mp_embed_builtin_format_obj;
#endif

#define MP_STATE_PORT MP_STATE_VM

// Use the minimal starting configuration (disables all optional features).
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

// MicroPython configuration.
#define MICROPY_ENABLE_COMPILER (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (1)
#define MICROPY_ENABLE_GC (1)
#define MICROPY_PY_GC (1)
#define MICROPY_PY_SYS (1)
#define MICROPY_PY_SYS_PATH (1)
#define MICROPY_PY_SYS_PATH_ARGV_DEFAULTS (1)
#define MICROPY_PY_SYS_PLATFORM "jukeboy-embed"
#define MICROPY_MODULE_FROZEN_STR (1)
#define MICROPY_MODULE_FROZEN_MPY (1)
#define MICROPY_QSTR_EXTRA_POOL mp_qstr_frozen_const_pool
#define MICROPY_PERSISTENT_CODE_LOAD (1)
#define MICROPY_PERSISTENT_CODE_SAVE (1)

// object representation and NLR handling
#define MICROPY_OBJ_REPR (MICROPY_OBJ_REPR_A)
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#define MICROPY_NLR_SETJMP (1)
#endif

// memory allocation policies
#define MICROPY_ALLOC_PATH_MAX (128)

// emitters
#if CONFIG_IDF_TARGET_ARCH_RISCV
#if CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
#define MICROPY_EMIT_RV32 (0)
#else
#define MICROPY_EMIT_RV32 (1)
#endif
#else
#define MICROPY_EMIT_XTENSAWIN (1)
#endif

// optimisations
#ifndef MICROPY_OPT_COMPUTED_GOTO
#define MICROPY_OPT_COMPUTED_GOTO (1)
#endif

#define MICROPY_STACK_CHECK_MARGIN (256)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_ENABLE_SCHEDULER (1)
#define MICROPY_GCREGS_SETJMP (1)
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ERROR_REPORTING (MICROPY_ERROR_REPORTING_NORMAL) // Debugging Note: Increase the error reporting level to view
                                                                 // __FUNCTION__, __LINE__, __FILE__ in check_esp_err() exceptions
#define MICROPY_WARNINGS (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY (1)
#define MICROPY_PY_BUILTINS_ENUMERATE (1)
#define MICROPY_PY_BUILTINS_FILTER (1)
#define MICROPY_PY_BUILTINS_REVERSED (1)
#define MICROPY_PY_BUILTINS_SET (1)
#define MICROPY_PY_BUILTINS_FROZENSET (1)
#define MICROPY_PY_BUILTINS_SLICE (1)
#define MICROPY_PY_BUILTINS_SLICE_ATTRS (1)
#define MICROPY_PY_BUILTINS_SLICE_INDICES (1)
#define MICROPY_PY_BUILTINS_POW3 (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW (1)
#define MICROPY_PY_MATH (1)
#define MICROPY_PY_CMATH (1)
#define MICROPY_EPOCH_IS_1970 (1)
#define MICROPY_PY_TIME (1)
#define MICROPY_PY_SELECT (1)
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_STREAMS_POSIX_API (1)
#define MICROPY_USE_INTERNAL_ERRNO (0)  // errno.h from xtensa-esp32-elf/sys-include/sys
#define MICROPY_USE_INTERNAL_PRINTF (0) // ESP32 SDK requires its own printf
#define MICROPY_SCHEDULER_DEPTH (8)
#define MICROPY_SCHEDULER_STATIC_NODES (1)
#define MICROPY_PY_ASYNC_AWAIT (1)
#define MICROPY_PY_ASYNCIO (1)

// control over Python builtins
#define MICROPY_PY_STR_BYTES_CMP_WARN (1)
#define MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS (1)
#define MICROPY_PY_IO_BUFFEREDWRITER (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS (1)
#define MICROPY_PY_TIME_INCLUDEFILE "port/modtime_embed.c"
#define MICROPY_PY_IO (1)
#define MICROPY_PY_IO_BYTESIO (1)
#define MICROPY_PY_BUILTINS_MIN_MAX (1)
#define MICROPY_PORT_BUILTINS \
    {MP_ROM_QSTR(MP_QSTR_format), MP_ROM_PTR(&mp_embed_builtin_format_obj)},
#define MICROPY_GC_SPLIT_HEAP (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO (1)
#if MICROPY_GC_SPLIT_HEAP_AUTO && !defined(MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES)
#define MICROPY_EMBED_GC_SPLIT_HEAP_MAX_BYTES (64 * 1024)
#endif

#if MICROPY_GC_SPLIT_HEAP_AUTO
void *mp_embed_alloc_heap(size_t size);
void mp_embed_free_heap(void *ptr);
#define MP_PLAT_ALLOC_HEAP(size) mp_embed_alloc_heap(size)
#define MP_PLAT_FREE_HEAP(ptr) mp_embed_free_heap(ptr)
#endif