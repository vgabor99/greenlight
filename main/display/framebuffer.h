#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "display/epaper.h"

/* The 1-bit frame both screens draw into: EPAPER_ROW_BYTES * EPAPER_HEIGHT bytes, rows MSB first, 1 = white, 0 = black.
 * Drawing outside the panel is clipped. */
#define FRAMEBUFFER_SIZE (EPAPER_ROW_BYTES * EPAPER_HEIGHT)

void framebuffer_clear(void);  /* all white */
void framebuffer_set_pixel(int x, int y, bool black);
void framebuffer_fill_rect(int x, int y, int width, int height, bool black);

/* A 1-bit bitmap (rows of (width + 7) / 8 bytes, MSB first, 1 = ink) with its top left corner at x, y; the ink is
 * painted black, or white over a black background. */
void framebuffer_blit(int x, int y, int width, int height, const uint8_t *bits, bool black);

const uint8_t *framebuffer_data(void);
