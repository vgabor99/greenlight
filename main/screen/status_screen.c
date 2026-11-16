#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "data_model.h"
#include "screen/framebuffer.h"
#include "screen/bitmap_text.h"
#include "screen/bitmaps.h"
#include "screen/status_screen.h"

/* Layout, measured from the accepted mockups (docs/uispec/ui-spec.md).
 * Text is placed by its baseline, icons by their centre. */
#define ROWS               6                                /* line rows (docs/uispec/ui-spec.md#problems) */
#define ROW_HEIGHT         (FRAMEBUFFER_HEIGHT / ROWS)      /* ROWS rows of 50 px fill the screen, no header */
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
#define BADGE_RING   4   /* first row's icon, with a white ring around it, 4.5 px wide (docs/data-model.md#staleness-flag) */

#define DEVICE_ICON_Y        112  /* device screens: icon, one line and an optional hint line */
#define DEVICE_TEXT_BASELINE 240
#define DEVICE_HINT_BASELINE 250  /* with a hint line, the icon and text move up by DEVICE_HINT_LIFT */
#define DEVICE_HINT_LIFT     24

#define PAIR_ICON_Y          94   /* "Párosítás": a smaller icon, the title and two instruction lines */
#define PAIR_TITLE_BASELINE  180
#define PAIR_LINE1_BASELINE  217
#define PAIR_LINE2_BASELINE  249

/* ---- House screens ---- */

/* Anything not in the line format is drawn as "??? ???" with the fallback icon; an unknown icon id only gets
 * the fallback icon. */
static line_t parse_line(const char *text)
{
    line_t line = {BITMAP_LINE_ICON_ALERT_CIRCLE, "???", 3, "???"};
    data_model_parse_line(text, &line);
    return line;
}

static const bitmap_icon_t *line_icon(uint8_t icon_id)
{
    return &bitmap_line_icons[icon_id < BITMAP_LINE_ICON_COUNT ? icon_id : BITMAP_LINE_ICON_ALERT_CIRCLE];
}

static void draw_separator(int y)
{
    for (int x = SEPARATOR_X0; x <= SEPARATOR_X1; x++) {
        if ((x - SEPARATOR_X0) % 5 < 2) {
            framebuffer_set_pixel(x, y, true);
        }
    }
}

/* Icon, bold name, state at the right; the name is cut to leave room for the state. A dotted separator above
 * every row but the first. */
static void draw_problem_row(int row, line_t line)
{
    int top = row * ROW_HEIGHT, baseline = top + ROW_BASELINE_DY;
    if (row > 0) {
        draw_separator(top - 1);
    }
    bitmap_draw_icon(line_icon(line.icon_id), ROW_ICON_X, top + ROW_ICON_CENTER_DY);
    int state_width, state_length = strlen(line.state);
    bitmap_text_fit(&bitmap_font_state, line.state, state_length, STATE_MAX_WIDTH, &state_width);
    bitmap_text_draw_fitted(&bitmap_font_state, ROW_RIGHT_X - state_width, baseline, line.state, state_length,
                            STATE_MAX_WIDTH);
    bitmap_text_draw_fitted(&bitmap_font_name, ROW_NAME_X, baseline, line.name, line.name_length,
                            ROW_RIGHT_X - state_width - ROW_GAP - ROW_NAME_X);
}

/* The layout rule (docs/uispec/ui-spec.md#problems): the lines from the top in dataset order; with more than ROWS of
 * them, the first ROWS - 1 and "további N" with the dots icon. */
static void draw_problem_rows(const shown_view_t *shown)
{
    int rows = shown->line_count > ROWS ? ROWS - 1 : shown->line_count;
    for (int i = 0; i < rows; i++) {
        draw_problem_row(i, parse_line(shown->lines[i]));
    }
    if (shown->line_count > rows) {
        char more[24];
        snprintf(more, sizeof(more), "további %d", shown->line_count - rows);
        draw_problem_row(rows, (line_t){BITMAP_LINE_ICON_DOTS_HORIZONTAL, more, strlen(more), ""});
    }
}

static void draw_all_ok(void)
{
    bitmap_draw_icon(&bitmap_icon_check_circle, FRAMEBUFFER_WIDTH / 2, OK_ICON_Y);
    bitmap_text_draw_centered(&bitmap_font_ok, OK_TITLE_BASELINE, "Minden OK");
    bitmap_text_draw_centered(&bitmap_font_detail, OK_LINE1_BASELINE, "Ajtók, ablakok zárva");
    bitmap_text_draw_centered(&bitmap_font_detail, OK_LINE2_BASELINE, "Készülékek kikapcsolva");
}

/* The stale flag (docs/data-model.md#staleness-flag), drawn over whatever is under it: white sync arrows on a black disc in a
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
    const bitmap_icon_t *icon = &bitmap_icon_sync_badge;
    framebuffer_blit(left + (2 * BADGE_RADIUS - icon->width + 1) / 2, top + (2 * BADGE_RADIUS - icon->height + 1) / 2, icon->width,
                     icon->height, icon->bits, false);
}

static void draw_house(const screen_content_t *content)
{
    if (content->shown->line_count) {
        draw_problem_rows(content->shown);
    } else {
        draw_all_ok();
    }
    if (content->stale) {
        draw_stale_badge();
    }
}

/* ---- Device screens: a large icon and one line, no header; some add a small hint line ("Párosítás" has its own
 * layout with two instruction lines) ---- */

static void draw_device_screen(const bitmap_icon_t *icon, const char *text, const char *hint)
{
    int lift = hint ? DEVICE_HINT_LIFT : 0;
    bitmap_draw_icon(icon, FRAMEBUFFER_WIDTH / 2, DEVICE_ICON_Y - lift);
    bitmap_text_draw_centered(&bitmap_font_big, DEVICE_TEXT_BASELINE - lift, text);
    if (hint) {
        bitmap_text_draw_centered(&bitmap_font_hint, DEVICE_HINT_BASELINE, hint);
    }
}

static void draw_unpaired(void)
{
    bitmap_draw_icon(&bitmap_icon_gesture_tap_hold, FRAMEBUFFER_WIDTH / 2, PAIR_ICON_Y);
    bitmap_text_draw_centered(&bitmap_font_big, PAIR_TITLE_BASELINE, "Párosítás");
    bitmap_text_draw_centered(&bitmap_font_hint, PAIR_LINE1_BASELINE, "Engedélyezd a csatlakozást");
    bitmap_text_draw_centered(&bitmap_font_hint, PAIR_LINE2_BASELINE, "Nyomd a gombot 3 mp-ig");
}

void status_screen_draw(const screen_content_t *content)
{
    switch (content->kind) {
    case SCREEN_BLANK:       break;
    case SCREEN_HOUSE:       draw_house(content); break;
    case SCREEN_UNPAIRED:    draw_unpaired(); break;
    case SCREEN_SEARCHING:
        draw_device_screen(&bitmap_icon_access_point, "Hálózatkeresés", "Engedélyezd a csatlakozást");
        break;
    case SCREEN_JOINING:     draw_device_screen(&bitmap_icon_access_point, "Csatlakozva", "Mindjárt kész"); break;
    case SCREEN_SYNCING:     draw_device_screen(&bitmap_icon_sync, "Szinkronizálás", NULL); break;
    case SCREEN_JOIN_FAILED: draw_device_screen(&bitmap_icon_alert_outline, "Csatlakozási hiba", NULL); break;
    case SCREEN_BATTERY_DEAD: draw_device_screen(&bitmap_icon_battery_outline, "Akkumulátor lemerült", NULL); break;
    case SCREEN_ERROR:       draw_device_screen(&bitmap_icon_alert_outline, "Ismeretlen hiba", NULL); break;
    }
}
