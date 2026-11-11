#include <string.h>
#include "panel/epaper.h"
#include "display/framebuffer.h"
#include "screen/bitmap_text.h"
#include "util/utf8.h"

#define ELLIPSIS "…"

void bitmap_draw_icon(const bitmap_icon_t *icon, int center_x, int center_y)
{
    framebuffer_blit(center_x - icon->width / 2, center_y - icon->height / 2, icon->width, icon->height, icon->bits, true);
}

/* The glyph of code_point; '?' for a character the font does not have. Binary search: glyphs are sorted. */
static const bitmap_glyph_t *find_glyph(const bitmap_font_t *font, uint32_t code_point)
{
    int low = 0, high = font->glyph_count - 1;
    while (low <= high) {
        int middle = (low + high) / 2;
        if (font->glyphs[middle].code_point == code_point) {
            return &font->glyphs[middle];
        }
        if (font->glyphs[middle].code_point < code_point) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return find_glyph(font, '?');
}

/* Advances are in 1/16 px; widths and positions round to whole pixels. */
int bitmap_text_width(const bitmap_font_t *font, const char *text, int length)
{
    int         width_16ths = 0;
    const char *end = text + length;
    while (text < end) {
        width_16ths += find_glyph(font, utf8_next_code_point(&text))->advance_16ths;
    }
    return (width_16ths + 8) / 16;
}

int bitmap_text_draw(const bitmap_font_t *font, int x, int baseline, const char *text, int length)
{
    int         pen_16ths = x * 16;
    const char *end = text + length;
    while (text < end) {
        const bitmap_glyph_t *glyph = find_glyph(font, utf8_next_code_point(&text));
        framebuffer_blit((pen_16ths + 8) / 16 + glyph->x_offset, baseline + glyph->y_offset, glyph->width, glyph->height,
                         font->bits + glyph->bits_offset, true);
        pen_16ths += glyph->advance_16ths;
    }
    return (pen_16ths + 8) / 16 - x;
}

void bitmap_text_draw_centered(const bitmap_font_t *font, int baseline, const char *text)
{
    int length = strlen(text);
    bitmap_text_draw(font, (EPAPER_WIDTH - bitmap_text_width(font, text, length)) / 2, baseline, text, length);
}

int bitmap_text_fit(const bitmap_font_t *font, const char *text, int length, int max_width, int *width)
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

void bitmap_text_draw_fitted(const bitmap_font_t *font, int x, int baseline, const char *text, int length, int max_width)
{
    int width, fitting = bitmap_text_fit(font, text, length, max_width, &width);
    x += bitmap_text_draw(font, x, baseline, text, fitting);
    if (fitting < length) {
        bitmap_text_draw(font, x, baseline, ELLIPSIS, strlen(ELLIPSIS));
    }
}
