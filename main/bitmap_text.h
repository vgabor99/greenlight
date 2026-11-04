#pragma once

#include "bitmaps.h"

void              draw_icon(const bm_icon_t *icon, int cx, int cy);
const bm_glyph_t *glyph(const bm_font_t *f, uint32_t cp);
int               text_width(const bm_font_t *f, const char *s, int len);
int               draw_text(const bm_font_t *f, int x, int base, const char *s, int len);
void              draw_centered(const bm_font_t *f, int base, const char *s);
int               text_cut(const bm_font_t *f, const char *s, int len, int max_w, int *w);
void              draw_cut(const bm_font_t *f, int x, int base, const char *s, int len, int max_w);
