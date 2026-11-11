#pragma once

#include "app/screen_content.h"

/* The proper screen (docs/display.md): draw the content into the framebuffer, which the caller has cleared. */
void status_screen_draw(const screen_content_t *content);
