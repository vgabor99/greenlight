#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "epd.h"
#include "render.h"
#include "zb.h"

static const char *TAG = "greenlight";

/* ---- Timing (DESIGN.md 3.2) ---- */
#define RUN_MS        20000   /* activity timeout */
#define PAIR_MS       180000  /* pairing window */
#define INTERVIEW_MS  10000   /* TEMPORARY for development; design value 60000 */
#define RESET_HOLD_MS 3000    /* pair button hold that clears the network */
#define REJOIN_MS     15000   /* give up rejoining after this long */

#define STACK_START_MS 30000   /* BOOT waits this long for the Zigbee stack to restore the network */
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

/* NVS (namespace "greenlight"): "pair_now" (start pairing right after the next boot, set by ERASE_NETWORK),
 * "snap" (blob: the snapshot on the panel and its footer; its hash is the shown_hash) and "wake_n" (the wake
 * counter, DESIGN.md 6.4). The Zigbee stack keeps the network in the same partition. */
static zb_snapshot_t snap;

/* Every boot is a wake (the sleep ends in a restart), so count boots. 24 bits, so the value is exact in the
 * Analog Input's float; one u32 write per wake is negligible flash wear for NVS. */
static uint32_t wake_count_next(void)
{
    uint32_t n = 0;
    nvs_get_u32(nvs, "wake_n", &n);
    n = (n + 1) & 0xFFFFFF;
    nvs_set_u32(nvs, "wake_n", n);
    nvs_commit(nvs);
    return n;
}

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

/* ---- State machine (DESIGN.md 3.3) ---- */
typedef enum {
    ST_BOOT, ST_UNPAIRED_WAIT, ST_PAIRING, ST_INTERVIEW, ST_REJOIN, ST_ACTIVE, ST_ERASE_NETWORK,
    ST_SHUTDOWN, ST_SLEEP,
} state_t;

static const char *const state_names[] = {
    "BOOT", "UNPAIRED_WAIT", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
};

static TickType_t boot_tick;
static uint32_t   wake_n;  /* this wake's number */

/* ---- Debug screen (render.c): the state, the shown snapshot and the last writes received ---- */
#define CLEAR_EVERY 50  /* partial refreshes between full clears, against ghosting */

static SemaphoreHandle_t view_lock;  /* guards snap and rx_log: the Zigbee task writes them */
static volatile bool     view_dirty;
static state_t           view_state;
static render_rx_t       rx_log[RENDER_RX_N];  /* ring; rx_head is the next slot */
static int               rx_head, rx_count;
static int               partials;   /* partial refreshes since the last full clear */

static void show_state(state_t st)
{
    view_state = st;
    view_dirty = true;
}

/* Panel diagnostics (BUSY hang hunt): every clear and refresh logs its duration, the BUSY level before it
 * starts and, for a refresh, how many pixels are black and how many change. */
static uint8_t prev_fb[EPD_STRIDE * EPD_HEIGHT];  /* last frame sent; all white after a clear */

static esp_err_t panel_clear(const char *why)
{
    int        busy = gpio_get_level(EPD_PIN_BUSY);
    TickType_t t0   = xTaskGetTickCount();
    esp_err_t  err  = epd_clear();
    memset(prev_fb, 0xFF, sizeof(prev_fb));
    ESP_LOGI(TAG, "panel: clear (%s) %lu ms, busy before %d%s", why, (unsigned long)ms_since(t0), busy,
             err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* Draw the screen now (~0.5 s). Blocks the state loop; input_task keeps polling the inputs meanwhile. */
static void redraw(void)
{
    static render_view_t v;
    xSemaphoreTake(view_lock, portMAX_DELAY);
    view_dirty  = false;
    v.state     = state_names[view_state];
    v.wake      = wake_n;
    v.uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    v.snap      = snap;
    v.rx_count  = rx_count;
    for (int i = 0; i < rx_count; i++) {
        v.rx[i] = rx_log[(rx_head - 1 - i + RENDER_RX_N) % RENDER_RX_N];
    }
    xSemaphoreGive(view_lock);
    const uint8_t *fb = render_status(&v);
    if (++partials >= CLEAR_EVERY) {
        partials = 0;
        if (panel_clear("periodic") != ESP_OK) {
            ESP_LOGE(TAG, "e-paper clear failed");
        }
    }
    int black = 0, changed = 0;
    for (size_t i = 0; i < sizeof(prev_fb); i++) {
        black += __builtin_popcount((uint8_t)~fb[i]);
        changed += __builtin_popcount(fb[i] ^ prev_fb[i]);
    }
    memcpy(prev_fb, fb, sizeof(prev_fb));
    int        busy = gpio_get_level(EPD_PIN_BUSY);
    TickType_t t0   = xTaskGetTickCount();
    esp_err_t  err  = epd_display_partial(fb);
    ESP_LOGI(TAG, "panel: refresh #%d (%s) %lu ms, busy before %d, black %d, changed %d%s", partials, v.state,
             (unsigned long)ms_since(t0), busy, black, changed, err == ESP_OK ? "" : ", FAILED");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

/* The state loops wait with this, so whatever arrives is drawn while it comes in. Writes that arrive during a
 * refresh are drawn together by the next one. */
static void wait_ms(uint32_t ms)
{
    if (view_dirty) {
        redraw();
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Zigbee callbacks (called in the Zigbee task): update the view, the state loop redraws. */
static void on_snapshot(const zb_snapshot_t *s)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    snap = *s;
    view_dirty = true;
    xSemaphoreGive(view_lock);
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
    xSemaphoreTake(view_lock, portMAX_DELAY);
    strncpy(snap.footer, footer, sizeof(snap.footer) - 1);
    view_dirty = true;
    xSemaphoreGive(view_lock);
    snap_save();
    ESP_LOGI(TAG, "footer \"%s\"", snap.footer);
}

static void on_rx(const zb_rx_t *ev)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    rx_log[rx_head] = (render_rx_t){.ms = pdTICKS_TO_MS(xTaskGetTickCount()), .ev = *ev};
    rx_head = (rx_head + 1) % RENDER_RX_N;
    if (rx_count < RENDER_RX_N) {
        rx_count++;
    }
    view_dirty = true;
    xSemaphoreGive(view_lock);
}

/* The reports of every wake (DESIGN.md 6.4), in this order; the wake counter last: it is HA's push trigger. */
static void report_wake_state(void)
{
    zb_report_battery();
    zb_report_occupancy(pir_active);
    zb_report_shown_hash(snap.hash);
    zb_report_awake(true);
    zb_report_wake();
}

static state_t st_boot(void)
{
    bool pair_now = nv_get("pair_now");
    snap_load();
    wake_n = wake_count_next();
    ESP_LOGI(TAG, "boot: pair_now=%d shown_hash=%06lx wake=%lu", pair_now, (unsigned long)snap.hash, (unsigned long)wake_n);

    boot_tick = xTaskGetTickCount();
    zb_callbacks_t cbs = {.snapshot = on_snapshot, .footer = on_footer, .rx = on_rx};
    zb_start(&cbs, snap.hash, wake_n);

    /* MVP: full clear on every wake (DESIGN.md 3.4), in parallel with the stack start-up. epd_init() also
     * releases the pin hold and recreates the SPI bus left by epd_poweroff(). */
    if (epd_init() != ESP_OK || panel_clear("boot") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }
    partials = 0;
    show_state(ST_BOOT);
    redraw();  /* the stored snapshot, while the stack starts */

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
    show_state(ST_UNPAIRED_WAIT);
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
        wait_ms(20);
    }
}

static state_t st_pairing(void)
{
    show_state(ST_PAIRING);
    TickType_t start = xTaskGetTickCount();
    zb_pairing_start();
    while (ms_since(start) < PAIR_MS) {
        if (zb_joined()) {
            zb_pairing_stop();
            ESP_LOGI(TAG, "joined after %lu ms", (unsigned long)ms_since(start));
            return ST_INTERVIEW;
        }
        wait_ms(100);
    }
    zb_pairing_stop();
    ESP_LOGW(TAG, "pairing window over");
    return ST_UNPAIRED_WAIT;
}

static state_t st_interview(void)
{
    show_state(ST_INTERVIEW);
    report_wake_state();  /* so HA already sees the device state and can push the first snapshot */
    ESP_LOGI(TAG, "staying awake for %d ms", INTERVIEW_MS);
    TickType_t start = xTaskGetTickCount();
    while (ms_since(start) < INTERVIEW_MS) {  /* the PIR is ignored here */
        wait_ms(20);
    }
    return ST_ACTIVE;
}

static state_t st_rejoin(void)
{
    show_state(ST_REJOIN);
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
        wait_ms(20);
    }
    ESP_LOGW(TAG, "rejoin failed");
    return ST_SHUTDOWN;
}

static state_t st_active(void)
{
    show_state(ST_ACTIVE);
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
            zb_log_neighbors();
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        if (idle_ms() >= RUN_MS) {
            ESP_LOGI(TAG, "no activity for %d ms", RUN_MS);
            return ST_SHUTDOWN;
        }
        wait_ms(20);
    }
}

static state_t st_erase_network(void)
{
    show_state(ST_ERASE_NETWORK);
    redraw();
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
    /* Cleanest state for power-off: full white reset, then the debug screen saying SLEEP. */
    if (panel_clear("shutdown") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper final clear failed");
    }
    partials = 0;
    show_state(ST_SLEEP);
    redraw();
    ESP_LOGI(TAG, "final refresh done");
    TickType_t t0 = xTaskGetTickCount();
    epd_poweroff();
    ESP_LOGI(TAG, "panel: poweroff %lu ms", (unsigned long)ms_since(t0));
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
    view_lock = xSemaphoreCreateMutex();
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
