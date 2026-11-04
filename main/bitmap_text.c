#include <string.h>
#include "bitmap_text.h"
#include "epd.h"
#include "framebuffer.h"
#include "utf8.h"

#define ELLIPSIS "…"

void bitmap_draw_icon(const bm_icon_t *icon, int center_x, int center_y)
{
    framebuffer_blit(center_x - icon->w / 2, center_y - icon->h / 2, icon->w, icon->h, icon->bits, true);
}

/* The glyph of code_point; '?' for a character the font does not have. Binary search: glyphs are sorted. */
static const bm_glyph_t *find_glyph(const bm_font_t *font, uint32_t code_point)
{
    int low = 0, high = font->count - 1;
    while (low <= high) {
        int middle = (low + high) / 2;
        if (font->glyph[middle].cp == code_point) {
            return &font->glyph[middle];
        }
        if (font->glyph[middle].cp < code_point) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return find_glyph(font, '?');
}

/* Advances are in 1/16 px; widths and positions round to whole pixels. */
int bitmap_text_width(const bm_font_t *font, const char *text, int length)
{
    int         width_16ths = 0;
    const char *end = text + length;
    while (text < end) {
        width_16ths += find_glyph(font, utf8_next_code_point(&text))->adv;
    }
    return (width_16ths + 8) / 16;
}

int bitmap_text_draw(const bm_font_t *font, int x, int baseline, const char *text, int length)
{
    int         pen_16ths = x * 16;
    const char *end = text + length;
    while (text < end) {
        const bm_glyph_t *glyph = find_glyph(font, utf8_next_code_point(&text));
        framebuffer_blit((pen_16ths + 8) / 16 + glyph->x, baseline + glyph->y, glyph->w, glyph->h,
                         font->bits + glyph->off, true);
        pen_16ths += glyph->adv;
    }
    return (pen_16ths + 8) / 16 - x;
}

void bitmap_text_draw_centered(const bm_font_t *font, int baseline, const char *text)
{
    int length = strlen(text);
    bitmap_text_draw(font, (EPD_WIDTH - bitmap_text_width(font, text, length)) / 2, baseline, text, length);
}

int bitmap_text_fit(const bm_font_t *font, const char *text, int length, int max_width, int *width)
{
    *width = bitmap_text_width(font, text, length);
    if (*width <= max_width) {
        return length;
    }
    int         ellipsis_width = bitmap_text_width(font, ELLIPSIS, strlen(ELLIPSIS));
    const char *fit_end = text;
    while (fit_end < text + length) {
        const char *next = fit_end;
        utf8_next_code_point(&next);
        if (bitmap_text_width(font, text, next - text) + ellipsis_width > max_width) {
            break;
        }
        fit_end = next;
    }
    *width = bitmap_text_width(font, text, fit_end - text) + ellipsis_width;
    return fit_end - text;
}

void bitmap_text_draw_fitted(const bm_font_t *font, int x, int baseline, const char *text, int length, int max_width)
{
    int width, fitting = bitmap_text_fit(font, text, length, max_width, &width);
    x += bitmap_text_draw(font, x, baseline, text, fitting);
    if (fitting < length) {
        bitmap_text_draw(font, x, baseline, ELLIPSIS, strlen(ELLIPSIS));
    }
}
