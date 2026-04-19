#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

#define SCRIPT_SOCKET_WASI_ESUCCESS (0)

#ifdef __cplusplus
extern "C"
{
#endif

    uint16_t script_socket_get_guest_buffer(wasm_module_inst_t module_inst,
                                            uint32_t offset,
                                            uint32_t size,
                                            bool allow_null,
                                            void **out_ptr);

    uint16_t script_socket_get_guest_string(wasm_module_inst_t module_inst,
                                            uint32_t offset,
                                            bool allow_null,
                                            const char **out_ptr);

    bool script_socket_env_register(void);

#ifdef __cplusplus
}
#endif