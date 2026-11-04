#pragma once

#include "render.h"

/* The proper screen (DESIGN.md 8): draw content->screen into the framebuffer, which the caller has cleared. */
void status_screen_draw(const render_content_t *content);
