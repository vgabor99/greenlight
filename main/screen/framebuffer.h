#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "panel/panel.h"

/* The 1-bit frame both screens draw into, the size and format of the panel's frames (panel/panel.h): rows of
 * FRAMEBUFFER_ROW_BYTES, MSB first, 1 = white, 0 = black. Drawing outside it is clipped. */
#define FRAMEBUFFER_WIDTH     PANEL_WIDTH
#define FRAMEBUFFER_HEIGHT    PANEL_HEIGHT
#define FRAMEBUFFER_ROW_BYTES (FRAMEBUFFER_WIDTH / 8)
#define FRAMEBUFFER_SIZE      PANEL_FRAME_BYTES

void framebuffer_clear(void);  /* all white */
void framebuffer_set_pixel(int x, int y, bool black);
void framebuffer_fill_rect(int x, int y, int width, int height, bool black);

/* A 1-bit bitmap (rows of (width + 7) / 8 bytes, MSB first, 1 = ink) with its top left corner at x, y; the ink is
 * painted black, or white over a black background. */
void framebuffer_blit(int x, int y, int width, int height, const uint8_t *bits, bool black);

const uint8_t *framebuffer_data(void);
