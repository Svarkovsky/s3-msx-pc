#include "../rg_gui.h"

extern const rg_font_t font_basic8x8;
extern const rg_font_t font_VeraBold14;

enum {
    RG_FONT_BASIC_8 = 0,
    RG_FONT_VERA_14 = 1,
    RG_FONT_MAX
};

static const rg_font_t *fonts[RG_FONT_MAX] = {
    &font_basic8x8,
    &font_VeraBold14,
};
