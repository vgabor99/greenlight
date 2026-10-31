#pragma once

#include <stdint.h>
#include "render.h"

/* The proper screen (DESIGN.md 8): draw v->screen into fb (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white), which the
 * caller has cleared to white. */
void screen_draw(uint8_t *fb, const render_view_t *v);
