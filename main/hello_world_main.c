#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "epd.h"

static const char *TAG = "greenlight";

static uint8_t fb[EPD_STRIDE * EPD_HEIGHT];

/* 5x7 font, column-major, bit 0 = top row. Only the glyphs we use. */
static const struct { char c; uint8_t col[5]; } font[] = {
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}}, {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

static const char *const words[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE"};
#define TEXT_SCALE 10  /* longest word: 5 chars * 60px - 10px gap = 290px wide */

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

static void draw_frame(const char *word)
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

    int w = (int)strlen(word) * 6 * TEXT_SCALE - TEXT_SCALE;
    draw_text((EPD_WIDTH - w) / 2, (EPD_HEIGHT - 7 * TEXT_SCALE) / 2, word, TEXT_SCALE);
}

void app_main(void)
{
    /* epd_init() sets up the SPI bus, so call it once; the panel stays awake between updates
     * (no epd_sleep(), which would need a hardware reset to wake). */
    if (epd_init() != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        return;
    }
    for (size_t i = 0;; i = (i + 1) % (sizeof(words) / sizeof(words[0]))) {
        draw_frame(words[i]);
        if (epd_display(fb) != ESP_OK) {
            ESP_LOGE(TAG, "e-paper update failed");
            return;
        }
        ESP_LOGI(TAG, "showing %s", words[i]);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
