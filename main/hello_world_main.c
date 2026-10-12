#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
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
#define BUTTON_DEBOUNCE_MS 50
#define RUN_MS 10000 /* how long the screen updates run before the final clean refresh and power-off */

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

static TaskHandle_t main_task;
static volatile TickType_t last_activity;  /* start of the RUN_MS window; moved forward by every button press */

static volatile uint32_t presses;  /* debounced presses so far, counted in the ISR (no logging there) */

static void log_new_presses(void)
{
    static uint32_t logged;
    uint32_t now = presses;
    if (now != logged) {
        ESP_LOGI(TAG, "button pressed (press #%lu)", (unsigned long)now);
        logged = now;
    }
}

/* Falling edge on the button: restart the run window and wake the main task if it is waiting. */
static void IRAM_ATTR button_isr(void *arg)
{
    static TickType_t last_edge;
    TickType_t now = xTaskGetTickCountFromISR();
    if (pdTICKS_TO_MS(now - last_edge) < BUTTON_DEBOUNCE_MS) {
        return;
    }
    last_edge = now;
    last_activity = now;
    presses++;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(main_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_init(void)
{
    main_task = xTaskGetCurrentTaskHandle();
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* button between the pin and GND */
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));
}

void app_main(void)
{
    button_init();
    /* A button press keeps the lifecycle going: during the run it restarts the RUN_MS window, once the
     * panel is asleep it restarts the whole cycle. */
    for (;;) {
        /* epd_init() also releases the pin hold and recreates the SPI bus left by epd_poweroff(). */
        if (epd_init() != ESP_OK) {
            ESP_LOGE(TAG, "e-paper init failed");
            return;
        }
        /* 1) Initial full white screen, so the controller has a valid previous frame. 2) Partial refreshes
         * back to back until RUN_MS after the start or the last button press. */
        if (epd_clear() != ESP_OK) {
            ESP_LOGE(TAG, "e-paper clear failed");
            return;
        }
        ulTaskNotifyTake(pdTRUE, 0);  /* forget the press that started this cycle */
        last_activity = xTaskGetTickCount();
        for (uint32_t n = 1; pdTICKS_TO_MS(xTaskGetTickCount() - last_activity) < RUN_MS; n++) {
            const char *word = words[(n - 1) % (sizeof(words) / sizeof(words[0]))];
            draw_frame(word);
            TickType_t start = xTaskGetTickCount();
            if (epd_display_partial(fb) != ESP_OK) {
                ESP_LOGE(TAG, "e-paper update failed");
                return;
            }
            ESP_LOGI(TAG, "#%lu %s (%lu ms)", (unsigned long)n, word,
                     (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start));
            log_new_presses();
        }

        /* 3) Cleanest state for power-off: full white reset, then render the last image (still in fb)
         * on the clean panel. A press from here on is kept and restarts the cycle after power-off. */
        ulTaskNotifyTake(pdTRUE, 0);
        if (epd_clear() != ESP_OK || epd_display_partial(fb) != ESP_OK) {
            ESP_LOGE(TAG, "e-paper final refresh failed");
            return;
        }
        ESP_LOGI(TAG, "final refresh done");

        /* 4) Sleep the panel, hold the pins low and wait for a button press (or power-off). */
        epd_poweroff();
        ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        log_new_presses();
        ESP_LOGI(TAG, "restarting");
    }
}
