#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "debug_screen.h"
#include "epaper.h"
#include "framebuffer.h"
#include "u8g2.h"
#include "utf8.h"

/* Terminus Bold 16 with Latin Extended-A: every Hungarian letter (also o/u with double acute); its tallest
 * glyph is 15 px, so it fits the 16 px rows. Bold, because 1 px strokes are too faint on this panel. */
#define TEXT_FONT     u8g2_font_t0_16b_te
#define CHAR_ADVANCE  8   /* of TEXT_FONT (monospace) */
#define ROW_PITCH     16
#define LEFT_MARGIN   2
#define TEXT_COLUMNS  ((EPAPER_WIDTH - 2 * LEFT_MARGIN) / CHAR_ADVANCE)  /* 49 */

/* Layout (y of each block) */
#define Y_HEADER          2
#define Y_RULE1           19
#define Y_SHOWN           22   /* dataset + view row, time row, 6 line rows: 8 rows */
#define Y_RULE2           151
#define Y_RECEIVED_WRITES 156  /* RENDER_RECEIVED_WRITES rows */

/* u8g2 is used only for its fonts and text drawing: it draws through draw_line() into the framebuffer and never
 * talks to the panel, so it has no buffer of its own (never call u8g2_ClearBuffer / u8g2_SendBuffer). */
static u8g2_t u8g2;

static const u8x8_display_info_t text_display = {
    .tile_width   = (EPAPER_WIDTH + 7) / 8,
    .tile_height  = (EPAPER_HEIGHT + 7) / 8,
    .pixel_width  = EPAPER_WIDTH,
    .pixel_height = EPAPER_HEIGHT,
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

static void draw_received_write(int y, const render_received_write_t *received)
{
    static const char *const view_results[]   = {"OK", "TIME", "OOS", "PART"};
    static const char *const commit_results[] = {"OK", "SAME", "BAD N", "REJ"};
    char time[12];
    snprintf(time, sizeof(time), "%lu.%lu", (unsigned long)(received->uptime_ms / 1000),
             (unsigned long)(received->uptime_ms % 1000 / 100));
    const zigbee_received_write_t *write = &received->write;
    switch (write->kind) {
    case ZIGBEE_WRITE_VIEW: {
        const zigbee_view_t *view = &write->view.view;
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

void debug_screen_format_age(char out[DEBUG_SCREEN_AGE_LEN], int64_t age_ms)
{
    int64_t seconds = age_ms < 0 ? 0 : age_ms / 1000;
    if (age_ms < 0) {
        strlcpy(out, "-", DEBUG_SCREEN_AGE_LEN);
    } else if (seconds < 60) {
        snprintf(out, DEBUG_SCREEN_AGE_LEN, "%ds", (int)seconds);
    } else if (seconds < 3600) {
        snprintf(out, DEBUG_SCREEN_AGE_LEN, "%dm", (int)(seconds / 60));
    } else {
        snprintf(out, DEBUG_SCREEN_AGE_LEN, "%dh", (int)(seconds / 3600));
    }
}

/* State left; battery, wake counter and uptime right */
static void draw_header(const render_content_t *content)
{
    char right[40];
    snprintf(right, sizeof(right), "%u.%02uV%s w%lu %lu.%lus", content->battery_mv / 1000,
             content->battery_mv % 1000 / 10, content->on_usb_power ? " USB" : content->battery_low ? " LOW" : "",
             (unsigned long)content->wake_number, (unsigned long)(content->uptime_ms / 1000),
             (unsigned long)(content->uptime_ms % 1000 / 100));
    draw_row(Y_HEADER, "%s", content->state_name);
    draw_text(EPAPER_WIDTH - LEFT_MARGIN - u8g2_GetUTF8Width(&u8g2, right), Y_HEADER, right);
}

/* The dataset and the view on the panel */
static void draw_shown_view(const render_content_t *content)
{
    const shown_view_t *shown = &content->shown;
    draw_row(Y_SHOWN, "DS %06lX n=%u  VIEW %06lX", (unsigned long)content->dataset_hash, content->dataset_lines,
             (unsigned long)shown->view.id);
    char age[DEBUG_SCREEN_AGE_LEN];
    debug_screen_format_age(age, content->age_ms);
    draw_row(Y_SHOWN + ROW_PITCH, "age %s / %lus %s  %u lines%s", age, (unsigned long)(content->valid_ms / 1000),
             content->stale ? "STALE" : "FRESH", shown->active_lines, content->out_of_sync ? "  OUT OF SYNC" : "");
    for (int i = 0; i < VIEW_LAYOUT_ROWS; i++) {
        int y = Y_SHOWN + (i + 2) * ROW_PITCH;
        if (i < shown->row_count) {
            draw_row(y, "%d %s", i + 1, shown->rows[i]);
        } else if (i == 0 && shown->view.id != 0) {
            draw_row(y, "  ALL OK");
        } else {
            draw_row(y, "%d", i + 1);
        }
    }
}

void debug_screen_draw(const render_content_t *content)
{
    init_text_drawing();
    draw_header(content);
    framebuffer_fill_rect(0, Y_RULE1, EPAPER_WIDTH, 2, true);
    draw_shown_view(content);
    framebuffer_fill_rect(0, Y_RULE2, EPAPER_WIDTH, 2, true);
    for (int i = 0; i < content->received_write_count && i < RENDER_RECEIVED_WRITES; i++) {  /* newest first */
        draw_received_write(Y_RECEIVED_WRITES + i * ROW_PITCH, &content->received_writes[i]);
    }
}
