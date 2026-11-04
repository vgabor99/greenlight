#pragma once

#include "bitmaps.h"

/* Drawing with the generated fonts and icons (bitmaps.c). Text is UTF-8, placed by its baseline; a character the
 * font does not have is drawn as '?'. Lengths are in bytes. */

/* The icon with its centre at center_x, center_y. */
void bitmap_draw_icon(const bitmap_icon_t *icon, int center_x, int center_y);

/* Width in pixels of the first length bytes of text. */
int bitmap_text_width(const bitmap_font_t *font, const char *text, int length);

/* Draw the first length bytes of text with the pen starting at x on the baseline; returns the width. */
int bitmap_text_draw(const bitmap_font_t *font, int x, int baseline, const char *text, int length);

/* The whole text, centred on the panel. */
void bitmap_text_draw_centered(const bitmap_font_t *font, int baseline, const char *text);

/* How many bytes of text (length bytes) to draw in at most max_width pixels: all of them, or as many whole
 * characters as fit with "…" after them. *width is the width drawn, the ellipsis included. */
int bitmap_text_fit(const bitmap_font_t *font, const char *text, int length, int max_width, int *width);

/* Draw text cut to max_width pixels (bitmap_text_fit) at x. */
void bitmap_text_draw_fitted(const bitmap_font_t *font, int x, int baseline, const char *text, int length, int max_width);
