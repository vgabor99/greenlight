#pragma once

#include <stdbool.h>
#include "app/app_state.h"

/* The RnD debug screen (docs/display.md#debug-screen): draw the state into the framebuffer, which the caller has
 * cleared. */
void debug_screen_draw(const app_state_t *state);

/* The debug screen drawn from `drawn` would differ from one drawn from `now`: the model changed, or the age as
 * printed, the staleness or the battery simulation did. */
bool debug_screen_is_outdated(const app_state_t *drawn, const app_state_t *now);
