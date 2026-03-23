#include "py/lexer.h"
#include "py/runtime.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    (void)filename;
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("file reader not available"));
    return NULL;
}
