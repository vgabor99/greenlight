#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bitmaps.h"
#include "epd.h"
#include "screen.h"

/* Layout, measured from the accepted mockups ("Bezárva? e-paper kijelző – design specifikáció").
 * Text is placed by its baseline, icons by their centre. */
#define HDR_ICON_X    24   /* house icon */
#define HDR_ICON_Y    20   /* house and update icons */
#define HDR_TEXT_X    45
#define HDR_BASE      29   /* "Bezárva?" */
#define HDR_TIME_R    386  /* right end of the time */
#define HDR_TIME_BASE 27
#define HDR_GAP       7    /* from the update icon's right end to the time */
#define HDR_RULE_Y    41   /* 2 px rule under the header */

#define ROWS_TOP    43                       /* RENDER_ROWS rows of about 42.8 px down to the bottom */
#define ROWS_H      (EPD_HEIGHT - ROWS_TOP)
#define ROW_ICON_X  26                       /* line icon centre */
#define ROW_ICON_DY 22                       /* line icon centre below the row top */
#define ROW_BASE_DY 29                       /* baseline below the row top */
#define ROW_NAME_X  53
#define ROW_RIGHT   385                      /* right end of the state */
#define ROW_GAP     12                       /* least room between the name and the state */
#define STATE_MAX_W 150                      /* a longer state is cut */
#define SEP_X0      52                       /* dotted separator between rows: 2 px on, 3 off */
#define SEP_X1      383

#define OK_ICON_Y     118  /* "Minden OK" screen */
#define OK_TITLE_BASE 220
#define OK_LINE1_BASE 248
#define OK_LINE2_BASE 272

#define DEV_ICON_Y    112  /* device screens: icon and one line */
#define DEV_TEXT_BASE 240

static uint8_t *fb;

static void set_black(int x, int y)
{
    if (x >= 0 && x < EPD_WIDTH && y >= 0 && y < EPD_HEIGHT) {
        fb[y * EPD_STRIDE + x / 8] &= ~(0x80 >> (x & 7));
    }
}

static void fill_black(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            set_black(x + i, y + j);
        }
    }
}

/* A 1-bit bitmap (rows of (w + 7) / 8 bytes, MSB first, 1 = black) with its top left corner at x, y. */
static void blit(int x, int y, int w, int h, const uint8_t *bits)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (bits[j * stride + i / 8] & (0x80 >> (i & 7))) {
                set_black(x + i, y + j);
            }
        }
    }
}

static void draw_icon(const bm_icon_t *icon, int cx, int cy)
{
    blit(cx - icon->w / 2, cy - icon->h / 2, icon->w, icon->h, icon->bits);
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
        blit((pen16 + 8) / 16 + g->x, base + g->y, g->w, g->h, f->bits + g->off);
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

static const char *const weekdays[] = {"vasárnap", "hétfő", "kedd", "szerda", "csütörtök", "péntek", "szombat"};

/* House icon, "Bezárva?", and the time of the view (DESIGN.md 5.3) at the right: "szombat, 18:46". */
static void draw_header(const render_shown_t *s)
{
    draw_icon(s->active ? &bm_icon_home_alert_outline : &bm_icon_home_outline, HDR_ICON_X, HDR_ICON_Y);
    draw_text(&bm_font_title, HDR_TEXT_X, HDR_BASE, "Bezárva?", strlen("Bezárva?"));
    if (s->view.time) {
        time_t    t = s->view.time;  /* local wall-clock time, so format it as UTC */
        struct tm tm;
        char      when[32];
        gmtime_r(&t, &tm);
        snprintf(when, sizeof(when), "%s, %d:%02d", weekdays[tm.tm_wday], tm.tm_hour, tm.tm_min);
        int len = strlen(when);
        int x = HDR_TIME_R - text_width(&bm_font_time, when, len);
        draw_text(&bm_font_time, x, HDR_TIME_BASE, when, len);
        const bm_icon_t *icon = &bm_icon_update;
        draw_icon(icon, x - HDR_GAP - icon->w + icon->w / 2, HDR_ICON_Y);
    }
    fill_black(0, HDR_RULE_Y, EPD_WIDTH, 2);
}

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
        int top = (ROWS_TOP * RENDER_ROWS + i * ROWS_H + RENDER_ROWS / 2) / RENDER_ROWS;
        if (i > 0) {
            for (int x = SEP_X0; x <= SEP_X1; x++) {
                if ((x - SEP_X0) % 5 < 2) {
                    set_black(x, top);
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
    draw_centered(&bm_font_big, OK_TITLE_BASE, "Minden OK");
    draw_centered(&bm_font_detail, OK_LINE1_BASE, "Ajtók, ablakok zárva");
    draw_centered(&bm_font_detail, OK_LINE2_BASE, "Készülékek kikapcsolva");
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
        draw_header(&v->shown);
        if (v->shown.nrows) {
            draw_rows(&v->shown);
        } else {
            draw_all_ok();
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
