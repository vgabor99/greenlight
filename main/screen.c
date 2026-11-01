#include <stdbool.h>
#include <string.h>
#include "bitmaps.h"
#include "epd.h"
#include "screen.h"

/* Layout, measured from the accepted mockups ("Bezárva? e-paper kijelző – design specifikáció").
 * Text is placed by its baseline, icons by their centre. */
#define ROW_H       (EPD_HEIGHT / RENDER_ROWS)  /* RENDER_ROWS rows of 50 px fill the screen, no header */
#define ROW_ICON_X  27                          /* line icon centre */
#define ROW_ICON_DY 25                          /* line icon centre below the row top */
#define ROW_BASE_DY 33                          /* baseline below the row top */
#define ROW_NAME_X  56
#define ROW_RIGHT   387                         /* right end of the state */
#define ROW_GAP     12                          /* least room between the name and the state */
#define STATE_MAX_W 180                         /* a longer state is cut */
#define SEP_X0      56                          /* dotted separator above a row: 2 px on, 3 off */
#define SEP_X1      387

#define OK_ICON_Y     97   /* "Minden OK" screen */
#define OK_TITLE_BASE 214
#define OK_LINE1_BASE 248
#define OK_LINE2_BASE 274

#define BADGE_R    17  /* stale badge: black disc 34 px across, right edge at ROW_RIGHT, centred on the */
#define BADGE_RING 4   /* first row's icon, with a white ring around it, 4.5 px wide (DESIGN.md 5.3) */

#define DEV_ICON_Y    112  /* device screens: icon and one line */
#define DEV_TEXT_BASE 240

static uint8_t *fb;

static void set_pixel(int x, int y, bool black)
{
    if (x >= 0 && x < EPD_WIDTH && y >= 0 && y < EPD_HEIGHT) {
        uint8_t *b = &fb[y * EPD_STRIDE + x / 8];
        *b = black ? *b & ~(0x80 >> (x & 7)) : *b | 0x80 >> (x & 7);
    }
}

/* A 1-bit bitmap (rows of (w + 7) / 8 bytes, MSB first, 1 = ink) with its top left corner at x, y; the ink is
 * painted black, or white over a black background. */
static void blit(int x, int y, int w, int h, const uint8_t *bits, bool black)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (bits[j * stride + i / 8] & (0x80 >> (i & 7))) {
                set_pixel(x + i, y + j, black);
            }
        }
    }
}

static void draw_icon(const bm_icon_t *icon, int cx, int cy)
{
    blit(cx - icon->w / 2, cy - icon->h / 2, icon->w, icon->h, icon->bits, true);
}

/* The code point at *s, and *s moved past it; '?' for a broken sequence. */
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t       c = *p++;
    int            n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n) {
        c &= 0x3F >> n;
    }
    for (; n > 0 && (*p & 0xC0) == 0x80; n--) {
        c = c << 6 | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return n ? '?' : c;
}

/* The glyph of cp; '?' for a character the font does not have. */
static const bm_glyph_t *glyph(const bm_font_t *f, uint32_t cp)
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
static int text_width(const bm_font_t *f, const char *s, int len)
{
    int         w16 = 0;
    const char *end = s + len;
    while (s < end) {
        w16 += glyph(f, utf8_next(&s))->adv;
    }
    return (w16 + 8) / 16;
}

/* Draw the first len bytes of s with the pen starting at x on the baseline; returns the width. */
static int draw_text(const bm_font_t *f, int x, int base, const char *s, int len)
{
    int         pen16 = x * 16;
    const char *end = s + len;
    while (s < end) {
        const bm_glyph_t *g = glyph(f, utf8_next(&s));
        blit((pen16 + 8) / 16 + g->x, base + g->y, g->w, g->h, f->bits + g->off, true);
        pen16 += g->adv;
    }
    return (pen16 + 8) / 16 - x;
}

static void draw_centered(const bm_font_t *f, int base, const char *s)
{
    int len = strlen(s);
    draw_text(f, (EPD_WIDTH - text_width(f, s, len)) / 2, base, s, len);
}

/* How many bytes of s (len bytes) to draw in at most max_w pixels: all of them, or as many whole characters as
 * fit with "…" after them. *w is the width drawn, the ellipsis included. */
static int text_cut(const bm_font_t *f, const char *s, int len, int max_w, int *w)
{
    *w = text_width(f, s, len);
    if (*w <= max_w) {
        return len;
    }
    int         ell = text_width(f, "…", strlen("…"));
    const char *p = s;
    while (p < s + len) {
        const char *q = p;
        utf8_next(&q);
        if (text_width(f, s, q - s) + ell > max_w) {
            break;
        }
        p = q;
    }
    *w = text_width(f, s, p - s) + ell;
    return p - s;
}

/* Draw s cut to max_w pixels (text_cut) at x. */
static void draw_cut(const bm_font_t *f, int x, int base, const char *s, int len, int max_w)
{
    int w, n = text_cut(f, s, len, max_w, &w);
    x += draw_text(f, x, base, s, n);
    if (n < len) {
        draw_text(f, x, base, "…", strlen("…"));
    }
}

/* ---- House screens ---- */

/* A dataset line, "II|name|state" (DESIGN.md 5.1): icon id in hex, name, state (may be empty). */
typedef struct {
    const bm_icon_t *icon;
    const char      *name;
    int              name_len;
    const char      *state;
} line_t;

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Anything not in the line format is drawn as "??? ???" with the fallback icon; an unknown icon id only gets
 * the fallback icon. */
static line_t parse_line(const char *s)
{
    line_t      l = {&bm_line_icon[BM_ICON_ALERT_CIRCLE], "???", 3, "???"};
    int         hi = hex_digit(s[0]), lo = hi < 0 ? -1 : hex_digit(s[1]);
    const char *bar = lo < 0 || s[2] != '|' ? NULL : strchr(s + 3, '|');
    if (!bar || bar == s + 3) {
        return l;
    }
    if (hi * 16 + lo < BM_LINE_ICONS) {
        l.icon = &bm_line_icon[hi * 16 + lo];
    }
    l.name = s + 3;
    l.name_len = bar - l.name;
    l.state = bar + 1;
    return l;
}

/* The problem lines from the top: icon, bold name, state at the right, dotted separators between them. */
static void draw_rows(const render_shown_t *s)
{
    for (int i = 0; i < s->nrows; i++) {
        int top = i * ROW_H;
        if (i > 0) {
            for (int x = SEP_X0; x <= SEP_X1; x++) {
                if ((x - SEP_X0) % 5 < 2) {
                    set_pixel(x, top - 1, true);
                }
            }
        }
        line_t l = parse_line(s->row[i]);
        int    base = top + ROW_BASE_DY;
        draw_icon(l.icon, ROW_ICON_X, top + ROW_ICON_DY);
        int sw, slen = strlen(l.state);
        text_cut(&bm_font_state, l.state, slen, STATE_MAX_W, &sw);
        draw_cut(&bm_font_state, ROW_RIGHT - sw, base, l.state, slen, STATE_MAX_W);
        draw_cut(&bm_font_name, ROW_NAME_X, base, l.name, l.name_len, ROW_RIGHT - sw - ROW_GAP - ROW_NAME_X);
    }
}

static void draw_all_ok(void)
{
    draw_icon(&bm_icon_check_circle, EPD_WIDTH / 2, OK_ICON_Y);
    draw_centered(&bm_font_ok, OK_TITLE_BASE, "Minden OK");
    draw_centered(&bm_font_detail, OK_LINE1_BASE, "Ajtók, ablakok zárva");
    draw_centered(&bm_font_detail, OK_LINE2_BASE, "Készülékek kikapcsolva");
}

/* The stale flag (DESIGN.md 5.3), drawn over whatever is under it: white sync arrows on a black disc in a
 * white ring. Distances are in half pixels, from pixel centres. */
static void draw_badge(void)
{
    int x0 = ROW_RIGHT + 1 - 2 * BADGE_R, y0 = ROW_ICON_DY - BADGE_R;  /* the disc's bounding box */
    int cx2 = 2 * x0 + 2 * BADGE_R, cy2 = 2 * y0 + 2 * BADGE_R;
    int r2 = 2 * (BADGE_R + BADGE_RING) + 1;  /* the ring's outer radius, in half pixels */
    for (int y = y0 - BADGE_RING - 1; y <= y0 + 2 * BADGE_R + BADGE_RING; y++) {
        for (int x = x0 - BADGE_RING - 1; x <= x0 + 2 * BADGE_R + BADGE_RING; x++) {
            int dx = 2 * x + 1 - cx2, dy = 2 * y + 1 - cy2, d = dx * dx + dy * dy;
            if (d <= r2 * r2) {
                set_pixel(x, y, d <= 4 * BADGE_R * BADGE_R);
            }
        }
    }
    const bm_icon_t *icon = &bm_icon_sync_badge;
    blit(x0 + (2 * BADGE_R - icon->w + 1) / 2, y0 + (2 * BADGE_R - icon->h + 1) / 2, icon->w, icon->h, icon->bits,
         false);
}

/* ---- Device screens: a large icon and one line, no header ---- */

static void draw_device(const bm_icon_t *icon, const char *text)
{
    draw_icon(icon, EPD_WIDTH / 2, DEV_ICON_Y);
    draw_centered(&bm_font_big, DEV_TEXT_BASE, text);
}

void screen_draw(uint8_t *frame, const render_view_t *v)
{
    fb = frame;
    switch (v->screen) {
    case RENDER_BLANK:
        break;
    case RENDER_HOUSE:
        if (v->shown.nrows) {
            draw_rows(&v->shown);
        } else {
            draw_all_ok();
        }
        if (v->stale) {
            draw_badge();
        }
        break;
    case RENDER_SEARCHING:   draw_device(&bm_icon_access_point, "Hálózatkeresés"); break;
    case RENDER_JOINING:     draw_device(&bm_icon_access_point, "Csatlakozás"); break;
    case RENDER_SYNCING:     draw_device(&bm_icon_sync, "Szinkronizálás"); break;
    case RENDER_JOIN_FAILED: draw_device(&bm_icon_alert_outline, "Csatlakozási hiba"); break;
    case RENDER_BATTERY:     draw_device(&bm_icon_battery_outline, "Akkumulátor lemerült"); break;
    case RENDER_ERROR:       draw_device(&bm_icon_alert_outline, "Ismeretlen hiba"); break;
    }
}
