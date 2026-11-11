#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "screen/framebuffer.h"
#include "screen/debug_screen.h"
#include "u8g2.h"
#include "util/utf8.h"

/* Terminus Bold 16 with Latin Extended-A: every Hungarian letter (also o/u with double acute); its tallest
 * glyph is 15 px, so it fits the 16 px rows. Bold, because 1 px strokes are too faint on this panel. */
#define TEXT_FONT     u8g2_font_t0_16b_te
#define CHAR_ADVANCE  8   /* of TEXT_FONT (monospace) */
#define ROW_PITCH     16
#define LEFT_MARGIN   2
#define TEXT_COLUMNS  ((FRAMEBUFFER_WIDTH - 2 * LEFT_MARGIN) / CHAR_ADVANCE)  /* 49 */

/* Layout (y of each block) */
#define Y_HEADER          2
#define Y_RULE1           19
#define Y_SHOWN           22   /* dataset + view row, time row, SHOWN_ROWS line rows: 8 rows */
#define SHOWN_ROWS        6
#define Y_RULE2           151
#define Y_RECEIVED_WRITES 156  /* APP_STATE_RECEIVED_WRITES rows */

/* u8g2 is used only for its fonts and text drawing: it draws through draw_line() into the framebuffer and never
 * talks to the panel, so it has no buffer of its own (never call u8g2_ClearBuffer / u8g2_SendBuffer). */
static u8g2_t u8g2;

static const u8x8_display_info_t text_display = {
    .tile_width   = (FRAMEBUFFER_WIDTH + 7) / 8,
    .tile_height  = (FRAMEBUFFER_HEIGHT + 7) / 8,
    .pixel_width  = FRAMEBUFFER_WIDTH,
    .pixel_height = FRAMEBUFFER_HEIGHT,
};

static uint8_t handle_display_message(u8x8_t *u8x8, uint8_t message, uint8_t arg_int, void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &text_display);
    }
    return 1;
}

/* u8g2's only drawing primitive: a horizontal (direction 0) or vertical line. */
static void draw_line(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t length, uint8_t direction)
{
    if (direction == 0) {
        framebuffer_fill_rect(x, y, length, 1, u->draw_color);
    } else {
        framebuffer_fill_rect(x, y, 1, length, u->draw_color);
    }
}

static void init_text_drawing(void)
{
    static bool done;
    if (done) {
        return;
    }
    u8x8_Setup(u8g2_GetU8x8(&u8g2), handle_display_message, u8x8_cad_empty, u8x8_byte_empty, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u8g2, NULL, text_display.tile_height, draw_line, U8G2_R0);
    u8g2_SetFont(&u8g2, TEXT_FONT);
    u8g2_SetFontMode(&u8g2, 1);         /* transparent: draw only the glyph pixels */
    u8g2_SetFontRefHeightAll(&u8g2);    /* y = top of the tallest glyph, so accented capitals stay in the row */
    u8g2_SetFontPosTop(&u8g2);
    done = true;
}

/* UTF-8 text with its top at y; returns its width in pixels. */
static int draw_text(int x, int y, const char *text)
{
    return u8g2_DrawUTF8(&u8g2, x, y, text);
}

/* One text row at the left margin; anything past TEXT_COLUMNS characters is cut off. */
static void draw_row(int y, const char *format, ...)
{
    char    text[4 * TEXT_COLUMNS + 1];  /* TEXT_COLUMNS characters of up to 4 UTF-8 bytes */
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    text[utf8_prefix_length(text, TEXT_COLUMNS)] = 0;
    draw_text(LEFT_MARGIN, y, text);
}

static void draw_received_write(int y, const app_state_received_write_t *received)
{
    static const char *const view_results[]   = {"OK", "TIME", "OOS", "PART"};
    static const char *const commit_results[] = {"OK", "SAME", "BAD N", "REJ"};
    _Static_assert(sizeof(view_results) / sizeof(view_results[0]) == ZIGBEE_VIEW_INCOMPLETE + 1, "view results");
    _Static_assert(sizeof(commit_results) / sizeof(commit_results[0]) == ZIGBEE_COMMIT_REJECTED + 1, "commit results");
    char time[12];
    snprintf(time, sizeof(time), "%lu.%lu", (unsigned long)(received->uptime_ms / 1000),
             (unsigned long)(received->uptime_ms % 1000 / 100));
    const zigbee_received_write_t *write = &received->write;
    switch (write->kind) {
    case ZIGBEE_WRITE_VIEW: {
        const view_t *view = &write->view.view;
        if (write->view.result == ZIGBEE_VIEW_OUT_OF_SYNC) {
            draw_row(y, "%s V %06lX OOS ds=%06lX", time, (unsigned long)view->id, (unsigned long)view->dataset_hash);
        } else if (write->view.result == ZIGBEE_VIEW_INCOMPLETE) {
            draw_row(y, "%s V %06lX PART m=%X", time, (unsigned long)view->id, write->view.received_mask);
        } else {
            draw_row(y, "%s V %06lX b=%08lX %s", time, (unsigned long)view->id, (unsigned long)view->bits,
                     view_results[write->view.result]);
        }
    } break;
    case ZIGBEE_WRITE_DATASET_ENTRY:
        if (write->entry.dataset_hash < 0) {
            draw_row(y, "%s E bad %s", time, write->entry.text);
        } else {
            draw_row(y, "%s E %02u.%u%s %s", time, write->entry.index, write->entry.part, write->entry.more ? "+" : "",
                     write->entry.text);
        }
        break;
    case ZIGBEE_WRITE_DATASET_COMMIT:
        if (write->commit.result == ZIGBEE_COMMIT_REJECTED) {
            draw_row(y, "%s D %06lX n=%u REJ idx=%u", time, (unsigned long)write->commit.hash, write->commit.count,
                     write->commit.first_missing);
        } else {
            draw_row(y, "%s D %06lX n=%u %s", time, (unsigned long)write->commit.hash, write->commit.count,
                     commit_results[write->commit.result]);
        }
        break;
    }
}

/* The age as shown: "12s", "5m", "3h", "-" when unknown. */
#define AGE_LEN 12
static void format_age(char out[AGE_LEN], int64_t age_ms)
{
    int64_t seconds = age_ms < 0 ? 0 : age_ms / 1000;
    if (age_ms < 0) {
        strlcpy(out, "-", AGE_LEN);
    } else if (seconds < 60) {
        snprintf(out, AGE_LEN, "%ds", (int)seconds);
    } else if (seconds < 3600) {
        snprintf(out, AGE_LEN, "%dm", (int)(seconds / 60));
    } else {
        snprintf(out, AGE_LEN, "%dh", (int)(seconds / 3600));
    }
}

/* State left; battery, wake counter and uptime right */
static void draw_header(const app_state_t *state)
{
    char right[40];
    snprintf(right, sizeof(right), "%u.%02uV%s w%lu %lu.%lus", state->battery_mv / 1000,
             state->battery_mv % 1000 / 10, state->on_usb_power ? " USB" : state->battery_low ? " LOW" : "",
             (unsigned long)state->wake_number, (unsigned long)(state->uptime_ms / 1000),
             (unsigned long)(state->uptime_ms % 1000 / 100));
    draw_row(Y_HEADER, "%s", device_state_name(state->state));
    draw_text(FRAMEBUFFER_WIDTH - LEFT_MARGIN - u8g2_GetUTF8Width(&u8g2, right), Y_HEADER, right);
}

/* The dataset and the view on the panel */
static void draw_shown_view(const app_state_t *state)
{
    const shown_view_t *shown = &state->shown;
    draw_row(Y_SHOWN, "DS %06lX n=%u  VIEW %06lX", (unsigned long)state->dataset_hash, state->dataset_lines,
             (unsigned long)shown->view.id);
    char age[AGE_LEN];
    format_age(age, state->age_ms);
    draw_row(Y_SHOWN + ROW_PITCH, "age %s / %lus %s  %u lines%s", age, (unsigned long)(state->valid_ms / 1000),
             state->stale ? "STALE" : "FRESH", shown->line_count, state->out_of_sync ? "  OUT OF SYNC" : "");
    int lines_drawn = shown->line_count > SHOWN_ROWS ? SHOWN_ROWS - 1 : shown->line_count;
    for (int i = 0; i < SHOWN_ROWS; i++) {
        int y = Y_SHOWN + (i + 2) * ROW_PITCH;
        if (i < lines_drawn) {
            draw_row(y, "%d %s", i + 1, shown->lines[i]);
        } else if (i == lines_drawn && shown->line_count > lines_drawn) {
            draw_row(y, "  +%d more", shown->line_count - lines_drawn);
        } else if (i == 0 && shown->view.id != 0) {
            draw_row(y, "  ALL OK");
        } else {
            draw_row(y, "%d", i + 1);
        }
    }
}

void debug_screen_draw(const app_state_t *state)
{
    init_text_drawing();
    draw_header(state);
    framebuffer_fill_rect(0, Y_RULE1, FRAMEBUFFER_WIDTH, 2, true);
    draw_shown_view(state);
    framebuffer_fill_rect(0, Y_RULE2, FRAMEBUFFER_WIDTH, 2, true);
    for (int i = 0; i < state->received_write_count && i < APP_STATE_RECEIVED_WRITES; i++) {  /* newest first */
        draw_received_write(Y_RECEIVED_WRITES + i * ROW_PITCH, &state->received_writes[i]);
    }
}

bool debug_screen_is_outdated(const app_state_t *drawn, const app_state_t *now)
{
    char drawn_age[AGE_LEN], now_age[AGE_LEN];
    format_age(drawn_age, drawn->age_ms);
    format_age(now_age, now->age_ms);
    return drawn->changes != now->changes || drawn->stale != now->stale ||
           drawn->battery_simulation_changes != now->battery_simulation_changes || strcmp(drawn_age, now_age) != 0;
}
