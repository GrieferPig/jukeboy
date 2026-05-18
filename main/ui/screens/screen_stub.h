#ifndef UI_SCREEN_STUB_H
#define UI_SCREEN_STUB_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"

    /**
     * Create a minimal stub screen showing @p name centred.
     * Returns the screen lv_obj_t (caller must not free it).
     */
    lv_obj_t *screen_stub_create(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* UI_SCREEN_STUB_H */
