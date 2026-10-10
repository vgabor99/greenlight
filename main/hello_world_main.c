#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "epd.h"

static const char *TAG = "greenlight";

static uint8_t fb[EPD_STRIDE * EPD_HEIGHT];

/* 5x7 font, column-major, bit 0 = top row. Only the glyphs we use. */
static const struct { char c; uint8_t col[5]; } font[] = {
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
};

static const char *const words[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN"};
#define TEXT_SCALE 11  /* longest word: 5 chars * 66px - 11px gap = 319px wide */
#define TEXT_ROWS  3   /* rows of 77px, one in each 100px band of the 300px height */
#define CLEAR_EVERY 100  /* updates between full white resets */

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

    /* Double border (disabled)
    fill_rect(0, 0, EPD_WIDTH, 4, 1);
    fill_rect(0, EPD_HEIGHT - 4, EPD_WIDTH, 4, 1);
    fill_rect(0, 0, 4, EPD_HEIGHT, 1);
    fill_rect(EPD_WIDTH - 4, 0, 4, EPD_HEIGHT, 1);
    fill_rect(12, 12, EPD_WIDTH - 24, 2, 1);
    fill_rect(12, EPD_HEIGHT - 14, EPD_WIDTH - 24, 2, 1);
    fill_rect(12, 12, 2, EPD_HEIGHT - 24, 1);
    fill_rect(EPD_WIDTH - 14, 12, 2, EPD_HEIGHT - 24, 1);
    */

    /* The word, TEXT_ROWS times, one per horizontal band of the screen. Each copy sits at a random
     * position inside its band, so over many updates the whole screen area gets exercised. */
    int w = (int)strlen(word) * 6 * TEXT_SCALE - TEXT_SCALE;
    int row_h = 7 * TEXT_SCALE;
    int band_h = EPD_HEIGHT / TEXT_ROWS;
    for (int r = 0; r < TEXT_ROWS; r++) {
        int x = esp_random() % (EPD_WIDTH - w + 1);
        int y = r * band_h + esp_random() % (band_h - row_h + 1);
        draw_text(x, y, word, TEXT_SCALE);
    }
}

void app_main(void)
{
    /* epd_init() sets up the SPI bus, so call it once; the panel stays awake between updates
     * (no epd_sleep(), which would need a hardware reset to wake). */
    if (epd_init() != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        return;
    }
    /* Partial refreshes back to back, to see how the panel holds up (ghosting, contrast) over many
     * updates, with a software full refresh to white first and then every CLEAR_EVERY updates. */
    for (uint32_t n = 1;; n++) {
        if ((n - 1) % CLEAR_EVERY == 0) {
            if (epd_clear() != ESP_OK) {
                ESP_LOGE(TAG, "e-paper clear failed");
                return;
            }
            ESP_LOGI(TAG, "cleared");
        }
        const char *word = words[(n - 1) % (sizeof(words) / sizeof(words[0]))];
        draw_frame(word);
        TickType_t start = xTaskGetTickCount();
        if (epd_display_partial(fb) != ESP_OK) {
            ESP_LOGE(TAG, "e-paper update failed");
            return;
        }
        ESP_LOGI(TAG, "#%lu %s (%lu ms)", (unsigned long)n, word,
                 (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start));
    }
}
