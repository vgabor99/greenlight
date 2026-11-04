#include <string.h>
#include "bitmap_text.h"
#include "epd.h"
#include "framebuffer.h"
#include "utf8.h"

void draw_icon(const bm_icon_t *icon, int cx, int cy)
{
    framebuffer_blit(cx - icon->w / 2, cy - icon->h / 2, icon->w, icon->h, icon->bits, true);
}

/* The glyph of cp; '?' for a character the font does not have. */
const bm_glyph_t *glyph(const bm_font_t *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyph[mid].cp == cp) {
            return &f->glyph[mid];
        }
        if (f->glyph[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return glyph(f, '?');
}

/* Width in pixels of the first len bytes of the UTF-8 text s. */
int text_width(const bm_font_t *f, const char *s, int len)
{
    int         w16 = 0;
    const char *end = s + len;
    while (s < end) {
        w16 += glyph(f, utf8_next_code_point(&s))->adv;
    }
    return (w16 + 8) / 16;
}

/* Draw the first len bytes of s with the pen starting at x on the baseline; returns the width. */
int draw_text(const bm_font_t *f, int x, int base, const char *s, int len)
{
    int         pen16 = x * 16;
    const char *end = s + len;
    while (s < end) {
        const bm_glyph_t *g = glyph(f, utf8_next_code_point(&s));
        framebuffer_blit((pen16 + 8) / 16 + g->x, base + g->y, g->w, g->h, f->bits + g->off, true);
        pen16 += g->adv;
    }
    return (pen16 + 8) / 16 - x;
}

void draw_centered(const bm_font_t *f, int base, const char *s)
{
    int len = strlen(s);
    draw_text(f, (EPD_WIDTH - text_width(f, s, len)) / 2, base, s, len);
}

/* How many bytes of s (len bytes) to draw in at most max_w pixels: all of them, or as many whole characters as
 * fit with "…" after them. *w is the width drawn, the ellipsis included. */
int text_cut(const bm_font_t *f, const char *s, int len, int max_w, int *w)
{
    *w = text_width(f, s, len);
    if (*w <= max_w) {
        return len;
    }
    int         ell = text_width(f, "…", strlen("…"));
    const char *p = s;
    while (p < s + len) {
        const char *q = p;
        utf8_next_code_point(&q);
        if (text_width(f, s, q - s) + ell > max_w) {
            break;
        }
        p = q;
    }
    *w = text_width(f, s, p - s) + ell;
    return p - s;
}

/* Draw s cut to max_w pixels (text_cut) at x. */
void draw_cut(const bm_font_t *f, int x, int base, const char *s, int len, int max_w)
{
    int w, n = text_cut(f, s, len, max_w, &w);
    x += draw_text(f, x, base, s, n);
    if (n < len) {
        draw_text(f, x, base, "…", strlen("…"));
    }
}
