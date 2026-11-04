#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "epd.h"
#include "framebuffer.h"
#include "render.h"
#include "screen.h"
#include "u8g2.h"
#include "utf8.h"

#define ADV   8   /* character advance of FONT (monospace) */
#define PITCH 16  /* row pitch */
#define COLS  49  /* (EPD_WIDTH - 2 * X0) / ADV */
#define X0    2

/* Terminus Bold 16 with Latin Extended-A: every Hungarian letter (also o/u with double acute); its tallest
 * glyph is 15 px, so it fits the 16 px rows. Bold, because 1 px strokes are too faint on this panel. */
#define FONT u8g2_font_t0_16b_te

/* Layout (y of each block) */
#define Y_HEADER 2
#define Y_RULE1  19
#define Y_SHOWN  22  /* dataset + view row, time row, 6 line rows: 8 rows */
#define Y_RULE2  151
#define Y_RX     156 /* RENDER_RX_N rows */

/* u8g2 is used only for its fonts and text drawing: it draws through text_hvline() into the framebuffer and never talks
 * to the panel, so it has no buffer of its own (never call u8g2_ClearBuffer / u8g2_SendBuffer). */
static u8g2_t u8g2;

static const u8x8_display_info_t text_display = {
    .tile_width   = (EPD_WIDTH + 7) / 8,
    .tile_height  = (EPD_HEIGHT + 7) / 8,
    .pixel_width  = EPD_WIDTH,
    .pixel_height = EPD_HEIGHT,
};

static uint8_t text_display_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &text_display);
    }
    return 1;
}

static void text_hvline(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t len, uint8_t dir)
{
    if (dir == 0) {
        framebuffer_fill_rect(x, y, len, 1, u->draw_color);
    } else {
        framebuffer_fill_rect(x, y, 1, len, u->draw_color);
    }
}

static void text_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    u8x8_Setup(u8g2_GetU8x8(&u8g2), text_display_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u8g2, NULL, text_display.tile_height, text_hvline, U8G2_R0);
    u8g2_SetFont(&u8g2, FONT);
    u8g2_SetFontMode(&u8g2, 1);         /* transparent: draw only the glyph pixels */
    u8g2_SetFontRefHeightAll(&u8g2);    /* y = top of the tallest glyph, so accented capitals stay in the row */
    u8g2_SetFontPosTop(&u8g2);
    done = true;
}

/* UTF-8 text with its top at y; returns its width in pixels. */
static int draw_text(int x, int y, const char *s)
{
    return u8g2_DrawUTF8(&u8g2, x, y, s);
}

/* One text row at the left margin; anything past COLS characters is cut off. */
static void row(int y, const char *fmt, ...)
{
    char    buf[4 * COLS + 1];  /* COLS characters of up to 4 UTF-8 bytes */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[utf8_prefix_length(buf, COLS)] = 0;
    draw_text(X0, y, buf);
}

static void rx_row(int y, const render_rx_t *r)
{
    static const char *const view_res[] = {"OK", "TIME", "OOS", "PART"};
    static const char *const ds_res[]   = {"OK", "SAME", "BAD N", "REJ"};
    char t[12];
    snprintf(t, sizeof(t), "%lu.%lu", (unsigned long)(r->ms / 1000), (unsigned long)(r->ms % 1000 / 100));
    const zb_rx_t *e = &r->ev;
    switch (e->kind) {
    case ZB_RX_VIEW: {
        const zb_view_t *v = &e->view.view;
        if (e->view.result == ZB_VIEW_OOS) {
            row(y, "%s V %06lX OOS ds=%06lX", t, (unsigned long)v->id, (unsigned long)v->ds);
        } else if (e->view.result == ZB_VIEW_INCOMPLETE) {
            row(y, "%s V %06lX PART m=%X", t, (unsigned long)v->id, e->view.mask);
        } else {
            row(y, "%s V %06lX b=%08lX %s", t, (unsigned long)v->id, (unsigned long)v->bits, view_res[e->view.result]);
        }
    } break;
    case ZB_RX_ENTRY:
        if (e->entry.tag < 0) {
            row(y, "%s E bad %s", t, e->entry.text);
        } else {
            row(y, "%s E %02u.%u%s %s", t, e->entry.index, e->entry.part, e->entry.more ? "+" : "", e->entry.text);
        }
        break;
    case ZB_RX_DS_COMMIT:
        if (e->commit.result == ZB_DS_REJECTED) {
            row(y, "%s D %06lX n=%u REJ idx=%u", t, (unsigned long)e->commit.hash, e->commit.count, e->commit.missing);
        } else {
            row(y, "%s D %06lX n=%u %s", t, (unsigned long)e->commit.hash, e->commit.count, ds_res[e->commit.result]);
        }
        break;
    }
}

void render_build_rows(render_shown_t *out, const zb_dataset_t *ds, const zb_view_t *view)
{
    memset(out, 0, sizeof(*out));
    out->view = *view;
    for (int i = 0; i < ds->count; i++) {
        if (view->bits & (1UL << i)) {
            out->active++;
        }
    }
    int shown = out->active > RENDER_ROWS ? RENDER_ROWS - 1 : out->active;
    for (int i = 0; i < ds->count && out->nrows < shown; i++) {
        if (view->bits & (1UL << i)) {
            memcpy(out->row[out->nrows++], ds->text[i], sizeof(out->row[0]));
        }
    }
    if (out->active > shown) {
        snprintf(out->row[out->nrows++], sizeof(out->row[0]), "01|további %u|", out->active - shown);
    }
}

void render_age(char out[RENDER_AGE_LEN], int64_t age_ms)
{
    int64_t s = age_ms < 0 ? 0 : age_ms / 1000;
    if (age_ms < 0) {
        strlcpy(out, "-", RENDER_AGE_LEN);
    } else if (s < 60) {
        snprintf(out, RENDER_AGE_LEN, "%ds", (int)s);
    } else if (s < 3600) {
        snprintf(out, RENDER_AGE_LEN, "%dm", (int)(s / 60));
    } else {
        snprintf(out, RENDER_AGE_LEN, "%dh", (int)(s / 3600));
    }
}

static void render_debug(const render_view_t *v)
{
    text_init();

    /* Header: state left; battery, wake counter and uptime right */
    char right[40];
    snprintf(right, sizeof(right), "%u.%02uV%s w%lu %lu.%lus", v->battery_mv / 1000, v->battery_mv % 1000 / 10,
             v->usb ? " USB" : v->bat_low ? " LOW" : "", (unsigned long)v->wake,
             (unsigned long)(v->uptime_ms / 1000), (unsigned long)(v->uptime_ms % 1000 / 100));
    row(Y_HEADER, "%s", v->state);
    draw_text(EPD_WIDTH - X0 - u8g2_GetUTF8Width(&u8g2, right), Y_HEADER, right);
    framebuffer_fill_rect(0, Y_RULE1, EPD_WIDTH, 2, true);

    /* Dataset and the view on the panel */
    const render_shown_t *s = &v->shown;
    row(Y_SHOWN, "DS %06lX n=%u  VIEW %06lX", (unsigned long)v->ds_hash, v->ds_count, (unsigned long)s->view.id);
    char age[RENDER_AGE_LEN];
    render_age(age, v->age_ms);
    row(Y_SHOWN + PITCH, "age %s / %lus %s  %u lines%s", age, (unsigned long)(v->valid_ms / 1000), v->stale ? "STALE" : "FRESH", s->active,
        v->oos ? "  OUT OF SYNC" : "");
    for (int i = 0; i < RENDER_ROWS; i++) {
        int y = Y_SHOWN + (i + 2) * PITCH;
        if (i < s->nrows) {
            row(y, "%d %s", i + 1, s->row[i]);
        } else if (i == 0 && s->view.id != 0) {
            row(y, "  ALL OK");
        } else {
            row(y, "%d", i + 1);
        }
    }
    framebuffer_fill_rect(0, Y_RULE2, EPD_WIDTH, 2, true);

    /* Received writes, newest first */
    for (int i = 0; i < v->rx_count && i < RENDER_RX_N; i++) {
        rx_row(Y_RX + i * PITCH, &v->rx[i]);
    }
}

const uint8_t *render_status(const render_view_t *v)
{
    framebuffer_clear();
    if (RENDER_DEBUG_SCREEN) {
        render_debug(v);
    } else {
        screen_draw(v);
    }
    return framebuffer_data();
}
