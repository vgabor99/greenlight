#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "epd.h"

static const char *TAG = "greenlight";

static uint8_t fb[EPD_STRIDE * EPD_HEIGHT];

/* 5x7 font, column-major, bit 0 = top row. Only the glyphs we use. */
static const struct { char c; uint8_t col[5]; } font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'!', {0x00, 0x00, 0x5F, 0x00, 0x00}}, {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}}, {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

static void set_pixel(int x, int y, int black)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }
    uint8_t mask = 0x80 >> (x & 7);
    if (black) {
        fb[y * EPD_STRIDE + x / 8] &= ~mask;
    } else {
        fb[y * EPD_STRIDE + x / 8] |= mask;
    }
}

static void fill_rect(int x, int y, int w, int h, int black)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            set_pixel(x + i, y + j, black);
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        for (size_t g = 0; g < sizeof(font) / sizeof(font[0]); g++) {
            if (font[g].c != *s) {
                continue;
            }
            for (int cx = 0; cx < 5; cx++) {
                for (int cy = 0; cy < 7; cy++) {
                    if (font[g].col[cx] & (1 << cy)) {
                        fill_rect(x + cx * scale, y + cy * scale, scale, scale, 1);
                    }
                }
            }
            break;
        }
    }
}

void app_main(void)
{
    memset(fb, 0xFF, sizeof(fb));  /* all white */

    /* Double border */
    fill_rect(0, 0, EPD_WIDTH, 4, 1);
    fill_rect(0, EPD_HEIGHT - 4, EPD_WIDTH, 4, 1);
    fill_rect(0, 0, 4, EPD_HEIGHT, 1);
    fill_rect(EPD_WIDTH - 4, 0, 4, EPD_HEIGHT, 1);
    fill_rect(12, 12, EPD_WIDTH - 24, 2, 1);
    fill_rect(12, EPD_HEIGHT - 14, EPD_WIDTH - 24, 2, 1);
    fill_rect(12, 12, 2, EPD_HEIGHT - 24, 1);
    fill_rect(EPD_WIDTH - 14, 12, 2, EPD_HEIGHT - 24, 1);

    draw_text(40, 80, "HELLO WORLD!", 5);   /* 12 chars * 30px = 360px wide */
    draw_text(88, 170, "ESP32-C6", 6);      /*  8 chars * 36px = 288px wide */

    if (epd_init() != ESP_OK || epd_display(fb) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
        return;
    }
    epd_sleep();
    ESP_LOGI(TAG, "display updated");
}
