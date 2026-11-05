#pragma once

#include "screen/render.h"

/* The proper screen (docs/display.md): draw content->screen into the framebuffer, which the caller has cleared. */
void status_screen_draw(const render_content_t *content);
