#pragma once

#include <stdint.h>
#include "screen/render.h"

/* The RnD debug screen (DESIGN.md 8.1): draw the content into the framebuffer, which the caller has cleared. */
void debug_screen_draw(const render_content_t *content);

/* The age as the debug screen shows it: "12s", "5m", "3h", "-" when unknown. */
#define DEBUG_SCREEN_AGE_LEN 12
void debug_screen_format_age(char out[DEBUG_SCREEN_AGE_LEN], int64_t age_ms);
