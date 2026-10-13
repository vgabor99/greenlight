#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "epd.h"
#include "zb.h"

static const char *TAG = "greenlight";

static uint8_t fb[EPD_STRIDE * EPD_HEIGHT];  /* 1 = white; app_main() whitens it, so a shutdown never pushes black */

/* 5x7 font, column-major, bit 0 = top row. Only the glyphs we use. */
static const struct { char c; uint8_t col[5]; } font[] = {
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}}, {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
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

/* Whole-screen word, centered; used as the placeholder screen of each state until the real renderer exists. */
static void draw_word(const char *word, int scale)
{
    memset(fb, 0xFF, sizeof(fb));
    int w = (int)strlen(word) * 6 * scale - scale;
    draw_text((EPD_WIDTH - w) / 2, (EPD_HEIGHT - 7 * scale) / 2, word, scale);
}

static void show_word(const char *word, int scale)
{
    draw_word(word, scale);
    if (epd_display_partial(fb) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

/* ---- Timing (DESIGN.md 3.2) ---- */
#define RUN_MS        30000   /* TEMPORARY for development (time to publish test messages); design value 5000 */
#define PAIR_MS       180000  /* pairing window */
#define INTERVIEW_MS  10000   /* TEMPORARY for development; design value 60000 */
#define RESET_HOLD_MS 3000    /* pair button hold that clears the network */
#define REJOIN_MS     15000   /* give up rejoining after this long */

#define STACK_START_MS 30000   /* BOOT waits this long for the Zigbee stack to restore the network */
#define DEV_SHOW_SLEEP 1       /* development: the final image before power-off is the word SLEEP */
#define HEARTBEAT_MS   30000   /* ACTIVE: repeat awake + shown_hash this often */

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

/* NVS (namespace "greenlight"): "pair_now" (start pairing right after the next boot, set by ERASE_NETWORK) and
 * "snap" (blob: the snapshot on the panel and its footer; its hash is the shown_hash). The Zigbee stack keeps
 * the network in the same partition. */
static zb_snapshot_t snap;

static void snap_load(void)
{
    size_t size = sizeof(snap);
    if (nvs_get_blob(nvs, "snap", &snap, &size) != ESP_OK || size != sizeof(snap)) {
        memset(&snap, 0, sizeof(snap));
    }
}

static void snap_save(void)
{
    nvs_set_blob(nvs, "snap", &snap, sizeof(snap));
    nvs_commit(nvs);
}

/* Called in the Zigbee task. "Rendering" is only logged until the renderer exists (DESIGN.md 12, step 5). */
static void on_snapshot(const zb_snapshot_t *s)
{
    snap = *s;
    snap_save();
    ESP_LOGI(TAG, "snapshot %06lx: %u lines, footer \"%s\"", (unsigned long)s->hash, s->count, s->footer);
    if (s->count == 0) {
        ESP_LOGI(TAG, "  (All OK)");
    }
    for (int i = 0; i < s->count; i++) {
        ESP_LOGI(TAG, "  %d: %s", i + 1, s->line[i]);
    }
}

static void on_footer(const char *footer)
{
    strncpy(snap.footer, footer, sizeof(snap.footer) - 1);
    snap_save();
    ESP_LOGI(TAG, "footer \"%s\"", snap.footer);
}

/* ---- State machine (DESIGN.md 3.3) ---- */
typedef enum {
    ST_BOOT, ST_UNPAIRED_WAIT, ST_PAIRING, ST_INTERVIEW, ST_REJOIN, ST_ACTIVE, ST_ERASE_NETWORK,
    ST_SHUTDOWN, ST_SLEEP,
} state_t;

static const char *const state_names[] = {
    "BOOT", "UNPAIRED_WAIT", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
};

static TickType_t boot_tick;

/* The reports of every wake (DESIGN.md 6.4), in this order; "awake" last. */
static void report_wake_state(void)
{
    zb_report_battery();
    zb_report_occupancy(pir_active);
    zb_report_shown_hash(snap.hash);
    zb_report_awake(true);
}

static state_t st_boot(void)
{
    bool pair_now = nv_get("pair_now");
    snap_load();
    ESP_LOGI(TAG, "boot: pair_now=%d shown_hash=%06lx", pair_now, (unsigned long)snap.hash);

    boot_tick = xTaskGetTickCount();
    zb_callbacks_t cbs = {.snapshot = on_snapshot, .footer = on_footer};
    zb_start(&cbs, snap.hash);

    /* MVP: full clear on every wake (DESIGN.md 3.4), in parallel with the stack start-up. epd_init() also
     * releases the pin hold and recreates the SPI bus left by epd_poweroff(). */
    if (epd_init() != ESP_OK || epd_clear() != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }

    bool factory_new = false;
    if (!zb_wait_started(STACK_START_MS, &factory_new)) {
        ESP_LOGE(TAG, "Zigbee stack did not start");
        return ST_SHUTDOWN;
    }
    ESP_LOGI(TAG, "stack up after %lu ms: %s", (unsigned long)ms_since(boot_tick),
             factory_new ? "no network" : "network restored");
    touch_activity();
    if (!factory_new) {
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
    zb_pairing_start();
    while (ms_since(start) < PAIR_MS) {
        if (zb_joined()) {
            zb_pairing_stop();
            ESP_LOGI(TAG, "joined after %lu ms", (unsigned long)ms_since(start));
            return ST_INTERVIEW;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    zb_pairing_stop();
    ESP_LOGW(TAG, "pairing window over");
    show_word("FAIL", 11);
    return ST_UNPAIRED_WAIT;
}

static state_t st_interview(void)
{
    show_word("INTERVIEW", 6);
    report_wake_state();  /* so HA already sees the device state and can push the first snapshot */
    ESP_LOGI(TAG, "staying awake for %d ms", INTERVIEW_MS);
    vTaskDelay(pdMS_TO_TICKS(INTERVIEW_MS));  /* the PIR is ignored here */
    return ST_ACTIVE;
}

static state_t st_rejoin(void)
{
    show_word("REJOIN", 6);
    TickType_t start = xTaskGetTickCount();
    touch_activity();
    while (ms_since(start) < REJOIN_MS) {
        if (zb_joined()) {
            ESP_LOGI(TAG, "rejoined, %lu ms after boot", (unsigned long)ms_since(boot_tick));
            report_wake_state();
            return ST_ACTIVE;
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {  /* network lost: allow re-pairing */
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "rejoin failed");
    return ST_SHUTDOWN;
}

static state_t st_active(void)
{
    show_word("ACTIVE", 7);  /* placeholder until the renderer shows the snapshot */
    touch_activity();
    bool      last_pir = pir_active;
    TickType_t last_beat = xTaskGetTickCount();
    for (;;) {
        if (pir_active != last_pir) {
            last_pir = pir_active;
            zb_report_occupancy(last_pir);
        }
        if (ms_since(last_beat) >= HEARTBEAT_MS) {  /* tell HA we are still awake */
            last_beat = xTaskGetTickCount();
            zb_report_shown_hash(snap.hash);
            zb_report_awake(true);
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        if (idle_ms() >= RUN_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", RUN_MS);
            return ST_SHUTDOWN;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static state_t st_erase_network(void)
{
    show_word("ERASE", 8);
    nv_set("pair_now", 1);
    nvs_erase_key(nvs, "snap");  /* nothing is known about the panel any more */
    nvs_commit(nvs);
    ESP_LOGI(TAG, "erasing the network, restarting");
    zb_erase_network();  /* does not return */
    return ST_BOOT;
}

static state_t st_shutdown(void)
{
    if (zb_joined()) {
        zb_report_awake(false);  /* best effort; the panel refresh below gives the radio time to send it */
    }
    /* Cleanest state for power-off: full white reset, then render the last image (still in fb). */
#if DEV_SHOW_SLEEP
    draw_word("SLEEP", 11);  /* development: the final image says the device is asleep */
#endif
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

/* XIAO ESP32-C6 antenna switch: GPIO3 low enables it, GPIO14 low selects the onboard ceramic antenna. */
static void rf_switch_init(void)
{
    gpio_config_t cfg = {.pin_bit_mask = (1ULL << 3) | (1ULL << 14), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(3, 0);
    gpio_set_level(14, 0);
}

void app_main(void)
{
    rf_switch_init();
    memset(fb, 0xFF, sizeof(fb));
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
