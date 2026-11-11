#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app/app_state.h"

/* The UI: the application state drawn into the framebuffer. Two renderers, chosen at build time:
 * - the proper screen (status_screen.c, docs/display.md): what app/screen_content.h says should be on the screen,
 *   in Inter with Material Design Icons;
 * - the debug screen for RnD (debug_screen.c, docs/display.md#debug-screen): the device state, the dataset, the view
 *   on the panel and the last writes received from HA, as UTF-8 text in u8g2's Terminus Bold 16 (49 columns,
 *   Hungarian accents included).
 * RENDER_DEBUG_SCREEN 1 selects the debug screen: `idf.py -DDEBUG_SCREEN=1 build` (`-DDEBUG_SCREEN=0` switches
 * back; CMake remembers the last value). Both are always compiled. */
#ifndef RENDER_DEBUG_SCREEN
#define RENDER_DEBUG_SCREEN 0
#endif

/* Draw the state into the framebuffer and return it (FRAMEBUFFER_SIZE bytes, 1 = white). */
const uint8_t *render_frame(const app_state_t *state);

/* A frame drawn from `drawn` would look different drawn from `now`: only what the selected screen shows counts. */
bool render_is_outdated(const app_state_t *drawn, const app_state_t *now);
