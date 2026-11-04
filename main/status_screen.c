#include <stdbool.h>
#include <string.h>
#include "bitmap_text.h"
#include "bitmaps.h"
#include "epaper.h"
#include "framebuffer.h"
#include "hex.h"
#include "status_screen.h"

/* Layout, measured from the accepted mockups ("Bezárva? e-paper kijelző – design specifikáció").
 * Text is placed by its baseline, icons by their centre. */
#define ROW_HEIGHT         (EPAPER_HEIGHT / VIEW_LAYOUT_ROWS)  /* VIEW_LAYOUT_ROWS rows of 50 px fill the screen, no header */
#define ROW_ICON_X         27                               /* line icon centre */
#define ROW_ICON_CENTER_DY 25                               /* line icon centre below the row top */
#define ROW_BASELINE_DY    33                               /* baseline below the row top */
#define ROW_NAME_X         56
#define ROW_RIGHT_X        387                              /* right end of the state */
#define ROW_GAP            12                               /* least room between the name and the state */
#define STATE_MAX_WIDTH    180                              /* a longer state is cut */
#define SEPARATOR_X0       56                               /* dotted separator above a row: 2 px on, 3 off */
#define SEPARATOR_X1       387

#define OK_ICON_Y          97   /* "Minden OK" screen */
#define OK_TITLE_BASELINE  214
#define OK_LINE1_BASELINE  248
#define OK_LINE2_BASELINE  274

#define BADGE_RADIUS 17  /* stale badge: black disc 34 px across, right edge at ROW_RIGHT_X, centred on the */
#define BADGE_RING   4   /* first row's icon, with a white ring around it, 4.5 px wide (DESIGN.md 5.3) */

#define DEVICE_ICON_Y        112  /* device screens: icon and one line */
#define DEVICE_TEXT_BASELINE 240

#define PAIR_ICON_Y          94   /* "Párosítás": a smaller icon, the title and two instruction lines */
#define PAIR_TITLE_BASELINE  180
#define PAIR_LINE1_BASELINE  217
#define PAIR_LINE2_BASELINE  249

/* ---- House screens ---- */

/* A dataset line, "II|name|state" (DESIGN.md 5.1): icon id in hex, name, state (may be empty). */
typedef struct {
    const bm_icon_t *icon;
    const char      *name;
    int              name_length;
    const char      *state;
} dataset_line_t;

/* Anything not in the line format is drawn as "??? ???" with the fallback icon; an unknown icon id only gets
 * the fallback icon. */
static dataset_line_t parse_dataset_line(const char *text)
{
    dataset_line_t line = {&bm_line_icon[BM_ICON_ALERT_CIRCLE], "???", 3, "???"};
    int            high = hex_digit_value(text[0]), low = high < 0 ? -1 : hex_digit_value(text[1]);
    const char    *name_end = low < 0 || text[2] != '|' ? NULL : strchr(text + 3, '|');
    if (!name_end || name_end == text + 3) {
        return line;
    }
    int icon_id = high * 16 + low;
    if (icon_id < BM_LINE_ICONS) {
        line.icon = &bm_line_icon[icon_id];
    }
    line.name        = text + 3;
    line.name_length = name_end - line.name;
    line.state       = name_end + 1;
    return line;
}

static void draw_separator(int y)
{
    for (int x = SEPARATOR_X0; x <= SEPARATOR_X1; x++) {
        if ((x - SEPARATOR_X0) % 5 < 2) {
            framebuffer_set_pixel(x, y, true);
        }
    }
}

/* Icon, bold name, state at the right; the name is cut to leave room for the state. */
static void draw_problem_row(int top, const char *text)
{
    dataset_line_t line = parse_dataset_line(text);
    int            baseline = top + ROW_BASELINE_DY;
    bitmap_draw_icon(line.icon, ROW_ICON_X, top + ROW_ICON_CENTER_DY);
    int state_width, state_length = strlen(line.state);
    bitmap_text_fit(&bm_font_state, line.state, state_length, STATE_MAX_WIDTH, &state_width);
    bitmap_text_draw_fitted(&bm_font_state, ROW_RIGHT_X - state_width, baseline, line.state, state_length,
                            STATE_MAX_WIDTH);
    bitmap_text_draw_fitted(&bm_font_name, ROW_NAME_X, baseline, line.name, line.name_length,
                            ROW_RIGHT_X - state_width - ROW_GAP - ROW_NAME_X);
}

/* The problem lines from the top, dotted separators between them. */
static void draw_problem_rows(const shown_view_t *shown)
{
    for (int i = 0; i < shown->row_count; i++) {
        int top = i * ROW_HEIGHT;
        if (i > 0) {
            draw_separator(top - 1);
        }
        draw_problem_row(top, shown->rows[i]);
    }
}

static void draw_all_ok(void)
{
    bitmap_draw_icon(&bm_icon_check_circle, EPAPER_WIDTH / 2, OK_ICON_Y);
    bitmap_text_draw_centered(&bm_font_ok, OK_TITLE_BASELINE, "Minden OK");
    bitmap_text_draw_centered(&bm_font_detail, OK_LINE1_BASELINE, "Ajtók, ablakok zárva");
    bitmap_text_draw_centered(&bm_font_detail, OK_LINE2_BASELINE, "Készülékek kikapcsolva");
}

/* The stale flag (DESIGN.md 5.3), drawn over whatever is under it: white sync arrows on a black disc in a
 * white ring. Distances are in half pixels, from pixel centres. */
static void draw_stale_badge(void)
{
    int left = ROW_RIGHT_X + 1 - 2 * BADGE_RADIUS, top = ROW_ICON_CENTER_DY - BADGE_RADIUS;  /* the disc's box */
    int center_x2 = 2 * left + 2 * BADGE_RADIUS, center_y2 = 2 * top + 2 * BADGE_RADIUS;
    int ring_radius2 = 2 * (BADGE_RADIUS + BADGE_RING) + 1;  /* the ring's outer radius, in half pixels */
    for (int y = top - BADGE_RING - 1; y <= top + 2 * BADGE_RADIUS + BADGE_RING; y++) {
        for (int x = left - BADGE_RING - 1; x <= left + 2 * BADGE_RADIUS + BADGE_RING; x++) {
            int dx = 2 * x + 1 - center_x2, dy = 2 * y + 1 - center_y2, distance_squared = dx * dx + dy * dy;
            if (distance_squared <= ring_radius2 * ring_radius2) {
                framebuffer_set_pixel(x, y, distance_squared <= 4 * BADGE_RADIUS * BADGE_RADIUS);
            }
        }
    }
    const bm_icon_t *icon = &bm_icon_sync_badge;
    framebuffer_blit(left + (2 * BADGE_RADIUS - icon->w + 1) / 2, top + (2 * BADGE_RADIUS - icon->h + 1) / 2, icon->w,
                     icon->h, icon->bits, false);
}

static void draw_house(const render_content_t *content)
{
    if (content->shown.row_count) {
        draw_problem_rows(&content->shown);
    } else {
        draw_all_ok();
    }
    if (content->stale) {
        draw_stale_badge();
    }
}

/* ---- Device screens: a large icon and one line, no header ("Párosítás" adds two instruction lines) ---- */

static void draw_device_screen(const bm_icon_t *icon, const char *text)
{
    bitmap_draw_icon(icon, EPAPER_WIDTH / 2, DEVICE_ICON_Y);
    bitmap_text_draw_centered(&bm_font_big, DEVICE_TEXT_BASELINE, text);
}

static void draw_unpaired(void)
{
    bitmap_draw_icon(&bm_icon_gesture_tap_hold, EPAPER_WIDTH / 2, PAIR_ICON_Y);
    bitmap_text_draw_centered(&bm_font_big, PAIR_TITLE_BASELINE, "Párosítás");
    bitmap_text_draw_centered(&bm_font_hint, PAIR_LINE1_BASELINE, "Engedélyezd a csatlakozást");
    bitmap_text_draw_centered(&bm_font_hint, PAIR_LINE2_BASELINE, "Nyomd a gombot 3 mp-ig");
}

void status_screen_draw(const render_content_t *content)
{
    switch (content->screen) {
    case RENDER_BLANK:       break;
    case RENDER_HOUSE:       draw_house(content); break;
    case RENDER_UNPAIRED:    draw_unpaired(); break;
    case RENDER_SEARCHING:   draw_device_screen(&bm_icon_access_point, "Hálózatkeresés"); break;
    case RENDER_JOINING:     draw_device_screen(&bm_icon_access_point, "Csatlakozás"); break;
    case RENDER_SYNCING:     draw_device_screen(&bm_icon_sync, "Szinkronizálás"); break;
    case RENDER_JOIN_FAILED: draw_device_screen(&bm_icon_alert_outline, "Csatlakozási hiba"); break;
    case RENDER_BATTERY:     draw_device_screen(&bm_icon_battery_outline, "Akkumulátor lemerült"); break;
    case RENDER_ERROR:       draw_device_screen(&bm_icon_alert_outline, "Ismeretlen hiba"); break;
    }
}
