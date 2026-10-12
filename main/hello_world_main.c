#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "epd.h"

static const char *TAG = "greenlight";

static uint8_t fb[EPD_STRIDE * EPD_HEIGHT];

/* 5x7 font, column-major, bit 0 = top row. Only the glyphs we use. */
static const struct { char c; uint8_t col[5]; } font[] = {
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
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


/* Whole-screen word, centered; used as the placeholder screen of each state until the real renderer exists. */
static void show_word(const char *word, int scale)
{
    memset(fb, 0xFF, sizeof(fb));
    int w = (int)strlen(word) * 6 * scale - scale;
    draw_text((EPD_WIDTH - w) / 2, (EPD_HEIGHT - 7 * scale) / 2, word, scale);
    if (epd_display_partial(fb) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

/* ---- Timing (DESIGN.md 3.2) ---- */
#define RUN_MS        5000    /* activity timeout */
#define PAIR_MS       180000  /* pairing window */
#define INTERVIEW_MS  10000   /* TEMPORARY for development; design value 60000 */
#define RESET_HOLD_MS 3000    /* pair button hold that clears the network */
#define REJOIN_MS     15000   /* give up rejoining after this long */

/* Zigbee is stubbed in this phase: "joining" and "rejoining" just succeed after these delays. */
#define STUB_JOIN_MS   3000
#define STUB_REJOIN_MS 1000

/* ---- Inputs: PIR (D0) and pair button (D1), level based, polled and debounced by a task ---- */
#define INPUT_POLL_MS        10
#define INPUT_DEBOUNCE_POLLS 5  /* a level must be stable for this many polls (50 ms) */

static volatile bool pir_active;
static volatile bool pair_active;
static volatile TickType_t last_activity;  /* last time any input was active */
static volatile TickType_t pair_since;     /* when the pair button went down (valid while pair_active) */

static bool active(void)
{
    return pir_active || pair_active;
}

static uint32_t ms_since(TickType_t t)
{
    return pdTICKS_TO_MS(xTaskGetTickCount() - t);
}

/* 0 while an input is active, else the time since the last activity: the RUN_MS countdown. */
static uint32_t idle_ms(void)
{
    return active() ? 0 : ms_since(last_activity);
}

static uint32_t pair_held_ms(void)
{
    return pair_active ? ms_since(pair_since) : 0;
}

static void touch_activity(void)
{
    last_activity = xTaskGetTickCount();
}

static void input_task(void *arg)
{
    int pir_n = 0, pair_n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        TickType_t now = xTaskGetTickCount();
        bool pir = gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
        bool pair = gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
        if (pir != pir_active) {
            if (++pir_n >= INPUT_DEBOUNCE_POLLS) {
                pir_active = pir;
                pir_n = 0;
            }
        } else {
            pir_n = 0;
        }
        if (pair != pair_active) {
            if (++pair_n >= INPUT_DEBOUNCE_POLLS) {
                pair_active = pair;
                pair_n = 0;
                if (pair) {
                    pair_since = now;
                }
            }
        } else {
            pair_n = 0;
        }
        if (pir_active || pair_active) {
            last_activity = now;
        }
    }
}

static void inputs_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIR_PIN) | (1ULL << PAIR_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* buttons between the pin and GND */
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    /* Start with the real levels: a button still held from the wake-up counts at once. */
    pir_active = gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
    pair_active = gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
    pair_since = last_activity = xTaskGetTickCount();
    ESP_LOGI(TAG, "inputs at boot: PIR(D0)=%d pair(D1)=%d", gpio_get_level(PIR_PIN), gpio_get_level(PAIR_BUTTON_PIN));
    xTaskCreate(input_task, "inputs", 2048, NULL, 5, NULL);
}

/* ---- State that survives the (simulated) deep sleep: NVS ---- */
static nvs_handle_t nvs;

static uint8_t nv_get(const char *key)
{
    uint8_t v = 0;
    nvs_get_u8(nvs, key, &v);
    return v;
}

static void nv_set(const char *key, uint8_t v)
{
    ESP_ERROR_CHECK(nvs_set_u8(nvs, key, v));
    ESP_ERROR_CHECK(nvs_commit(nvs));
}

static void nvs_open_state(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("greenlight", NVS_READWRITE, &nvs));
}

/* NVS keys: "paired" (stands in for "the Zigbee stack has a stored network") and "pair_now" (start pairing
 * right after the next boot, set by ERASE_NETWORK). */

/* ---- State machine (DESIGN.md 3.3) ---- */
typedef enum {
    ST_BOOT, ST_UNPAIRED_WAIT, ST_PAIRING, ST_INTERVIEW, ST_REJOIN, ST_ACTIVE, ST_ERASE_NETWORK,
    ST_SHUTDOWN, ST_SLEEP,
} state_t;

static const char *const state_names[] = {
    "BOOT", "UNPAIRED_WAIT", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
};

static state_t st_boot(void)
{
    bool paired = nv_get("paired");
    bool pair_now = nv_get("pair_now");
    ESP_LOGI(TAG, "boot: paired=%d pair_now=%d", paired, pair_now);
    /* MVP: full clear on every wake (DESIGN.md 3.4). epd_init() also releases the pin hold and recreates
     * the SPI bus left by epd_poweroff(). */
    if (epd_init() != ESP_OK || epd_clear() != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }
    touch_activity();
    if (paired) {
        return ST_REJOIN;
    }
    if (pair_now) {
        nv_set("pair_now", 0);
        return ST_PAIRING;
    }
    return ST_UNPAIRED_WAIT;
}

static state_t st_unpaired_wait(void)
{
    show_word("WAIT", 11);
    touch_activity();
    for (;;) {
        /* Long press, like the network reset in ACTIVE. A button still held from the wake-up counts from boot. */
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_PAIRING;
        }
        if (idle_ms() >= RUN_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", RUN_MS);
            return ST_SHUTDOWN;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static state_t st_pairing(void)
{
    show_word("PAIR", 11);
    TickType_t start = xTaskGetTickCount();
    while (ms_since(start) < PAIR_MS) {
        if (ms_since(start) >= STUB_JOIN_MS) {  /* stub for network steering */
            nv_set("paired", 1);
            ESP_LOGI(TAG, "joined (stub)");
            return ST_INTERVIEW;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "pairing window over");
    show_word("FAIL", 11);
    return ST_UNPAIRED_WAIT;
}

static state_t st_interview(void)
{
    show_word("INTERVIEW", 6);
    ESP_LOGI(TAG, "staying awake for %d ms", INTERVIEW_MS);
    vTaskDelay(pdMS_TO_TICKS(INTERVIEW_MS));  /* the PIR is ignored here */
    return ST_ACTIVE;
}

static state_t st_rejoin(void)
{
    show_word("REJOIN", 6);
    TickType_t start = xTaskGetTickCount();
    while (ms_since(start) < REJOIN_MS) {
        if (ms_since(start) >= STUB_REJOIN_MS) {  /* stub for the Zigbee rejoin */
            ESP_LOGI(TAG, "rejoined (stub, %lu ms)", (unsigned long)ms_since(start));
            return ST_ACTIVE;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "rejoin failed");
    return ST_SHUTDOWN;
}

static state_t st_active(void)
{
    static uint32_t n;  /* placeholder content: the ONE..TEN refresh test, until the real renderer exists */
    touch_activity();
    for (;; n++) {
        const char *word = words[n % (sizeof(words) / sizeof(words[0]))];
        draw_frame(word);
        TickType_t start = xTaskGetTickCount();
        if (epd_display_partial(fb) != ESP_OK) {
            ESP_LOGE(TAG, "e-paper update failed");
            return ST_SHUTDOWN;
        }
        ESP_LOGI(TAG, "#%lu %s (%lu ms)", (unsigned long)(n + 1), word, (unsigned long)ms_since(start));
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        if (idle_ms() >= RUN_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", RUN_MS);
            return ST_SHUTDOWN;
        }
    }
}

static state_t st_erase_network(void)
{
    show_word("ERASE", 8);
    nv_set("paired", 0);  /* stub for: leave, erase the Zigbee storage */
    nv_set("pair_now", 1);
    ESP_LOGI(TAG, "network erased (stub), restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ST_BOOT;
}

static state_t st_shutdown(void)
{
    /* Cleanest state for power-off: full white reset, then render the last image (still in fb). */
    if (epd_clear() != ESP_OK || epd_display_partial(fb) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper final refresh failed");
    }
    ESP_LOGI(TAG, "final refresh done");
    epd_poweroff();
    ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
    return ST_SLEEP;
}

/* Simulated deep sleep: wait for either wake input, then restart the chip, which runs the same boot path
 * as a real wake (only NVS carries over). The final firmware replaces this with esp_deep_sleep_start()
 * and both pins as wake sources. */
static state_t st_sleep(void)
{
    while (!active()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "wake: %s", pair_active ? "pair button" : "PIR");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ST_BOOT;
}

void app_main(void)
{
    nvs_open_state();
    inputs_init();
    state_t st = ST_BOOT;
    for (;;) {
        state_t next;
        switch (st) {
        case ST_BOOT:          next = st_boot(); break;
        case ST_UNPAIRED_WAIT: next = st_unpaired_wait(); break;
        case ST_PAIRING:       next = st_pairing(); break;
        case ST_INTERVIEW:     next = st_interview(); break;
        case ST_REJOIN:        next = st_rejoin(); break;
        case ST_ACTIVE:        next = st_active(); break;
        case ST_ERASE_NETWORK: next = st_erase_network(); break;
        case ST_SHUTDOWN:      next = st_shutdown(); break;
        default:               next = st_sleep(); break;
        }
        ESP_LOGI(TAG, "%s -> %s", state_names[st], state_names[next]);
        st = next;
    }
}
