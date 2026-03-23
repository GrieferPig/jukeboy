/* This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT)
 */

#include "py/runtime.h"
#include "py/objstr.h"

#ifndef STATIC
#define STATIC static
#endif

STATIC mp_obj_t mp_embed_builtin_format(size_t n_args, const mp_obj_t *args)
{
    mp_obj_t value = args[0];
    vstr_t pattern;
    vstr_init(&pattern, 8);
    vstr_add_char(&pattern, '{');
    if (n_args == 2 && args[1] != mp_const_none)
    {
        size_t spec_len;
        const char *spec_buf = mp_obj_str_get_data(args[1], &spec_len);
        if (spec_len != 0)
        {
            vstr_add_char(&pattern, ':');
            vstr_add_strn(&pattern, spec_buf, spec_len);
        }
    }
    vstr_add_char(&pattern, '}');
    mp_obj_t fmt = mp_obj_new_str_from_vstr(&pattern);
    mp_obj_t fmt_args[2] = {fmt, value};
    return mp_obj_str_format(MP_ARRAY_SIZE(fmt_args), fmt_args, NULL);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_embed_builtin_format_obj, 1, 2, mp_embed_builtin_format);
