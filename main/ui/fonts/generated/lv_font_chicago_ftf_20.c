/*******************************************************************************
 * Size: 20 px
 * Bpp: 1
 * Opts: --no-compress --no-prefilter --no-kerning --bpp 1 --size 20 --font .\fonts\chicago-ftf.otf -r 0x20-0x7F --format lvgl -o .\fonts\generated\lv_font_chicago_ftf_20.c --lv-font-name lv_font_chicago_ftf_20 --force-fast-kern-format
 ******************************************************************************/

#include "lvgl.h"

#ifndef LV_FONT_CHICAGO_FTF_20
#define LV_FONT_CHICAGO_FTF_20 1
#endif

#if LV_FONT_CHICAGO_FTF_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xff, 0xff, 0x3, 0xfe,

    /* U+0022 "\"" */
    0x99, 0x99,

    /* U+0024 "$" */
    0x10, 0xf2, 0x4c, 0x8f, 0x1e, 0x1c, 0x3c, 0x1e,
    0x3c, 0x7c, 0x97, 0x82, 0x4, 0x0,

    /* U+0026 "&" */
    0x7c, 0xc, 0x83, 0x9c, 0x73, 0x9e, 0x72, 0xe0,
    0x79, 0xf7, 0x38, 0xe7, 0x1c, 0xe3, 0x9c, 0x32,
    0x7, 0xc0,

    /* U+0027 "'" */
    0xf0,

    /* U+0028 "(" */
    0x8, 0x9, 0xce, 0x73, 0x9c, 0xe7, 0x8, 0x40,
    0x80,

    /* U+0029 ")" */
    0x80, 0x23, 0x33, 0x33, 0x33, 0x22, 0x80,

    /* U+002A "*" */
    0x88, 0x9, 0x10,

    /* U+002B "+" */
    0x10, 0x20, 0x47, 0xf1, 0x2, 0x4, 0x0,

    /* U+002C "," */
    0xff, 0x93, 0x0,

    /* U+002D "-" */
    0xfe,

    /* U+002E "." */
    0xff, 0x80,

    /* U+002F "/" */
    0x2, 0x0, 0x20, 0x41, 0x2, 0x4, 0x10, 0x20,
    0x42, 0x4, 0x8, 0x0,

    /* U+0030 "0" */
    0x3e, 0x26, 0xe7, 0xe7, 0xef, 0xef, 0xff, 0xf7,
    0xf7, 0xe7, 0xe7, 0xe7, 0x3e,

    /* U+0031 "1" */
    0x77, 0xf7, 0x77, 0x77, 0x77, 0x77, 0x70,

    /* U+0032 "2" */
    0x7e, 0x2, 0x83, 0x3, 0x3, 0x3, 0xe, 0x1c,
    0x1c, 0x70, 0xe0, 0xe0, 0xff,

    /* U+0033 "3" */
    0xff, 0xe, 0xe, 0x1c, 0x1c, 0x3e, 0x3, 0x3,
    0x3, 0x3, 0x83, 0x2, 0x3e,

    /* U+0034 "4" */
    0x7, 0x3, 0x83, 0xc2, 0xe0, 0x71, 0x3a, 0x1d,
    0xff, 0x7, 0x3, 0x81, 0xc0, 0xe0, 0x70,

    /* U+0035 "5" */
    0xff, 0xe0, 0xe0, 0xe0, 0xe0, 0xfe, 0x7, 0x7,
    0x7, 0x7, 0x87, 0x6, 0x3e,

    /* U+0036 "6" */
    0x1e, 0x70, 0x70, 0xe0, 0xe0, 0xfe, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0x66, 0x7e,

    /* U+0037 "7" */
    0xff, 0x81, 0xc0, 0xe0, 0x70, 0x38, 0x1c, 0x38,
    0x1c, 0x1c, 0xe, 0x7, 0x3, 0x81, 0xc0,

    /* U+0038 "8" */
    0x3e, 0x26, 0xe7, 0xe7, 0xe7, 0x3e, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0x26, 0x3e,

    /* U+0039 "9" */
    0x7e, 0x66, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0x7f,
    0x7, 0x7, 0xe, 0xe, 0x78,

    /* U+003A ":" */
    0xff, 0x8f, 0xf8,

    /* U+003B ";" */
    0xff, 0x8f, 0xf9, 0x30,

    /* U+003D "=" */
    0xfe, 0x0, 0x7, 0xf0,

    /* U+003F "?" */
    0x3e, 0x2, 0x83, 0x3, 0xe, 0xe, 0x1c, 0x1c,
    0x1c, 0x0, 0x1c, 0x1c, 0x1c,

    /* U+0040 "@" */
    0x7f, 0x0, 0x20, 0x31, 0xd9, 0x2c, 0x96, 0x4b,
    0x3e, 0x80, 0x40, 0x20, 0x20, 0x7, 0xf0,

    /* U+0041 "A" */
    0x3e, 0x26, 0xe7, 0xe7, 0xe7, 0xe7, 0xff, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7,

    /* U+0042 "B" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xfc, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe4, 0xfc,

    /* U+0043 "C" */
    0x3e, 0x20, 0xe1, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0,
    0xe0, 0xe0, 0xe1, 0x20, 0x3e,

    /* U+0044 "D" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0xfc,

    /* U+0045 "E" */
    0xff, 0xc3, 0x87, 0xe, 0x1c, 0x3f, 0x70, 0xe1,
    0xc3, 0x87, 0xf, 0xe0,

    /* U+0046 "F" */
    0xff, 0xc3, 0x87, 0xe, 0x1c, 0x3f, 0x70, 0xe1,
    0xc3, 0x87, 0xe, 0x0,

    /* U+0047 "G" */
    0x3e, 0x20, 0xe1, 0xe0, 0xe0, 0xe0, 0xef, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0x3e,

    /* U+0048 "H" */
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xff, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7,

    /* U+0049 "I" */
    0xff, 0xff, 0xff, 0xff, 0xfe,

    /* U+004A "J" */
    0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0xe7,
    0xe7, 0xe7, 0xe7, 0x26, 0x3e,

    /* U+004B "K" */
    0xe1, 0xf8, 0x4e, 0x73, 0xb8, 0xee, 0x3e, 0xf,
    0x3, 0xe0, 0xf8, 0x3b, 0x8e, 0x73, 0x9c, 0xe1,
    0xc0,

    /* U+004C "L" */
    0xe1, 0xc3, 0x87, 0xe, 0x1c, 0x38, 0x70, 0xe1,
    0xc3, 0x87, 0xf, 0xe0,

    /* U+004D "M" */
    0x80, 0x6, 0x0, 0x1e, 0x1, 0xfc, 0xf, 0xfc,
    0xff, 0xf3, 0xf9, 0xfd, 0xe3, 0xc7, 0x8f, 0x1e,
    0x8, 0x78, 0x1, 0xe0, 0x7, 0x80, 0x1c,

    /* U+004E "N" */
    0x80, 0xc0, 0x78, 0x3e, 0x1f, 0xf, 0xe6, 0x7b,
    0x1f, 0x8f, 0xc1, 0xe0, 0x70, 0x18, 0x8,

    /* U+004F "O" */
    0x3e, 0x26, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0x3e,

    /* U+0050 "P" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xfc, 0xe0,
    0xe0, 0xe0, 0xe0, 0xe0, 0xe0,

    /* U+0051 "Q" */
    0x3e, 0x26, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0x3e, 0x7,

    /* U+0052 "R" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xfc, 0xe4,
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7,

    /* U+0053 "S" */
    0x78, 0xc3, 0x8f, 0xe, 0x1e, 0x1e, 0xf, 0x1e,
    0x1e, 0x38, 0x47, 0x80,

    /* U+0054 "T" */
    0xff, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x1c,
    0x1c, 0x1c, 0x1c, 0x1c, 0x1c,

    /* U+0055 "U" */
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0x26, 0x3e,

    /* U+0056 "V" */
    0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3,
    0xe3, 0xe3, 0xe2, 0xe2, 0xfc,

    /* U+0057 "W" */
    0xe7, 0x1f, 0x9c, 0x7e, 0x71, 0xf9, 0xc7, 0xe7,
    0x1f, 0x9c, 0x7e, 0x71, 0xf9, 0xc7, 0xe7, 0x1f,
    0x9c, 0x7e, 0x71, 0x39, 0xc4, 0xff, 0xe0,

    /* U+0058 "X" */
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0x26, 0x3e, 0x26,
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7,

    /* U+0059 "Y" */
    0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0x22, 0x3e, 0x1c,
    0x1c, 0x1c, 0x1c, 0x1c, 0x1c,

    /* U+005A "Z" */
    0xff, 0x3, 0x3, 0x3, 0xe, 0xe, 0x1c, 0x70,
    0x70, 0xe0, 0xe0, 0xe0, 0xff,

    /* U+005B "[" */
    0xfe, 0xee, 0xee, 0xee, 0xee, 0xee, 0xf0,

    /* U+005C "\\" */
    0x80, 0x0, 0x81, 0x1, 0x2, 0x4, 0x4, 0x8,
    0x10, 0x8, 0x10, 0x20,

    /* U+005D "]" */
    0xf7, 0x77, 0x77, 0x77, 0x77, 0x77, 0xf0,

    /* U+005F "_" */
    0xf8,

    /* U+0060 "`" */
    0x81, 0x0, 0x10,

    /* U+0061 "a" */
    0x3e, 0x6, 0x87, 0x7, 0x7f, 0x67, 0xe7, 0xe7,
    0xe7, 0x7f,

    /* U+0062 "b" */
    0xe0, 0xe0, 0xe0, 0xfc, 0xe4, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0xfc,

    /* U+0063 "c" */
    0x78, 0xc3, 0x8f, 0xe, 0x1c, 0x38, 0x71, 0x60,
    0xf0,

    /* U+0064 "d" */
    0x7, 0x7, 0x7, 0x7f, 0x67, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0x7f,

    /* U+0065 "e" */
    0x3e, 0xe7, 0xe7, 0xe7, 0xff, 0xe0, 0xe0, 0xe1,
    0x20, 0x3e,

    /* U+0066 "f" */
    0x3c, 0x8e, 0x3e, 0xe3, 0x8e, 0x38, 0xe3, 0x8e,
    0x38, 0xe0,

    /* U+0067 "g" */
    0x7f, 0x67, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0x7f, 0x7, 0x7, 0x87, 0x3e,

    /* U+0068 "h" */
    0xe0, 0xe0, 0xe0, 0xfc, 0xe4, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7,

    /* U+0069 "i" */
    0xe0, 0x7f, 0xff, 0xff, 0xfe,

    /* U+006A "j" */
    0xe, 0x0, 0x0, 0x70, 0xe1, 0xc3, 0x87, 0xe,
    0x1c, 0x38, 0x70, 0xf1, 0xc2, 0x3c,

    /* U+006B "k" */
    0xe0, 0xe0, 0xe0, 0xe7, 0xee, 0xee, 0xf8, 0xf0,
    0xf0, 0xf8, 0xee, 0xee, 0xe7,

    /* U+006C "l" */
    0xff, 0xff, 0xff, 0xff, 0xfe,

    /* U+006D "m" */
    0xfc, 0xf3, 0x90, 0x4e, 0x71, 0xf9, 0xc7, 0xe7,
    0x1f, 0x9c, 0x7e, 0x71, 0xf9, 0xc7, 0xe7, 0x1f,
    0x9c, 0x70,

    /* U+006E "n" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xe7,

    /* U+006F "o" */
    0x3e, 0x26, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0x3e,

    /* U+0070 "p" */
    0xfc, 0xe4, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0xfc, 0xe0, 0xe0, 0xe0,

    /* U+0071 "q" */
    0x7f, 0x67, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0xe7, 0x7f, 0x7, 0x7, 0x7,

    /* U+0072 "r" */
    0xef, 0xdf, 0xff, 0xe, 0x1c, 0x38, 0x70, 0xe1,
    0xc0,

    /* U+0073 "s" */
    0x79, 0xc7, 0x87, 0x87, 0x83, 0xc7, 0xc7, 0x8,
    0xf0,

    /* U+0074 "t" */
    0x71, 0xc7, 0x3f, 0x71, 0xc7, 0x1c, 0x71, 0xc7,
    0x4, 0x1c,

    /* U+0075 "u" */
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0x67, 0x7f,

    /* U+0076 "v" */
    0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe3, 0xe2,
    0xe2, 0xfc,

    /* U+0077 "w" */
    0xe7, 0x1f, 0x9c, 0x7e, 0x71, 0xf9, 0xc7, 0xe7,
    0x1f, 0x9c, 0x7e, 0x71, 0xf9, 0xc4, 0xe7, 0x13,
    0xff, 0x80,

    /* U+0078 "x" */
    0xe3, 0xe3, 0xe3, 0xe3, 0x1c, 0x0, 0xe3, 0xe3,
    0xe3, 0xe3,

    /* U+0079 "y" */
    0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
    0x67, 0x7f, 0x7, 0x7, 0x87, 0x3e,

    /* U+007A "z" */
    0xff, 0x81, 0xc0, 0xe0, 0x80, 0x81, 0x0, 0x81,
    0xc0, 0xe0, 0x7f, 0xc0,

    /* U+007B "{" */
    0x1c, 0x47, 0x1c, 0x71, 0xce, 0x1c, 0x71, 0xc7,
    0x1c, 0x1c,

    /* U+007D "}" */
    0xe1, 0x87, 0x1c, 0x71, 0xc1, 0xdc, 0x71, 0xc7,
    0x1c, 0xe0};

/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 79, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 76, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 6, .adv_w = 98, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 8, .adv_w = 142, .box_w = 7, .box_h = 15, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 22, .adv_w = 208, .box_w = 11, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 40, .adv_w = 54, .box_w = 1, .box_h = 4, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 41, .adv_w = 98, .box_w = 5, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 50, .adv_w = 98, .box_w = 4, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 57, .adv_w = 98, .box_w = 5, .box_h = 4, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 60, .adv_w = 142, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 67, .adv_w = 76, .box_w = 3, .box_h = 6, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 70, .adv_w = 142, .box_w = 7, .box_h = 1, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 71, .adv_w = 76, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 73, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 85, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 98, .adv_w = 98, .box_w = 4, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 105, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 118, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 131, .adv_w = 186, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 159, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 172, .adv_w = 164, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 187, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 200, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 213, .adv_w = 76, .box_w = 3, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 216, .adv_w = 76, .box_w = 3, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 220, .adv_w = 142, .box_w = 7, .box_h = 4, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 224, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 237, .adv_w = 186, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 252, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 278, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 291, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 304, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 316, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 328, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 341, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 354, .adv_w = 76, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 359, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 372, .adv_w = 186, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 389, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 401, .adv_w = 252, .box_w = 14, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 424, .adv_w = 186, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 439, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 452, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 465, .adv_w = 164, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 479, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 492, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 504, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 517, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 530, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 543, .adv_w = 252, .box_w = 14, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 566, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 579, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 592, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 605, .adv_w = 98, .box_w = 4, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 612, .adv_w = 142, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 624, .adv_w = 98, .box_w = 4, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 631, .adv_w = 120, .box_w = 5, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 632, .adv_w = 98, .box_w = 5, .box_h = 4, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 635, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 645, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 658, .adv_w = 142, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 667, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 680, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 690, .adv_w = 114, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 700, .adv_w = 164, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 714, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 727, .adv_w = 76, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 732, .adv_w = 105, .box_w = 7, .box_h = 16, .ofs_x = -1, .ofs_y = -3},
    {.bitmap_index = 746, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 759, .adv_w = 76, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 764, .adv_w = 252, .box_w = 14, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 782, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 792, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 802, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 815, .adv_w = 164, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 828, .adv_w = 139, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 837, .adv_w = 142, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 846, .adv_w = 110, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 856, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 866, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 876, .adv_w = 252, .box_w = 14, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 894, .adv_w = 164, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 904, .adv_w = 164, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 918, .adv_w = 164, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 930, .adv_w = 120, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 940, .adv_w = 120, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 0}};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint8_t glyph_id_ofs_list_0[] = {
    0, 1, 2, 0, 3, 0, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 0, 26};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
    {
        {.range_start = 32, .range_length = 30, .glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = glyph_id_ofs_list_0, .list_length = 30, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL},
        {.range_start = 63, .range_length = 31, .glyph_id_start = 28, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
        {.range_start = 95, .range_length = 29, .glyph_id_start = 59, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
        {.range_start = 125, .range_length = 1, .glyph_id_start = 88, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 4,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
};

/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
const lv_font_t lv_font_chicago_ftf_20 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt, /*Function pointer to get glyph's bitmap*/
    .line_height = 19,                              /*The maximum line height required by the font*/
    .base_line = 4,                                 /*Baseline measured from the bottom of the line*/
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = &font_dsc, /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
    .fallback = NULL,
    .user_data = NULL,
};

#endif /*#if LV_FONT_CHICAGO_FTF_20*/
