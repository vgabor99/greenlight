#pragma once

#include <stdint.h>
#include "render.h"

/* The proper screen (DESIGN.md 8): draw v->screen into the framebuffer, which the caller has cleared to white. */
void screen_draw(const render_view_t *v);
