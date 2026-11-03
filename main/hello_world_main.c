#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
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
#define HEARTBEAT_MS   30000   /* ACTIVE: repeat awake + shown_view_id this often */
#define REPORT_DELAY_MS 3000   /* after a rejoin: the reports that follow the wake counter (DESIGN.md 6.4) */
#define VIEW_WAIT_MS   45000   /* ACTIVE: stay awake this long for the wake's view, so HA's resend can land (3.2) */
#define OCC_HOLD_MS     5000   /* occupancy: the PIR was seen within this long (a PIR wake counts as seen at boot) */
#define GOODBYE_MS      5000   /* SHUTDOWN: wait this long for Z2M to confirm occupancy 0 and awake false */
#define SCREEN_VALID_MS 8000   /* a received view is fresh this long: HA pushes every 5 s while awake, + 3 s latency (5.3) */

/* ---- Battery (DESIGN.md 6.6) ---- */
#define BAT_ENTER_MV   3500  /* below this the wake ends in LOW_BATTERY */
#define BAT_EXIT_MV    3700  /* LOW_BATTERY is left only at or above this, or on USB power */
#define BAT_CHECK_MS   1000  /* mid-wake check period */
#define SIM_MV_DEFAULT 4100  /* simulated voltage until the console sets one */

/* ---- Inputs: PIR (D0) and pair button (D1), level based, polled and debounced by a task ---- */
#define INPUT_POLL_MS        10
#define INPUT_DEBOUNCE_POLLS 5  /* a level must be stable for this many polls (50 ms) */

static volatile bool pir_active;
static volatile bool pair_active;
static volatile TickType_t last_activity;  /* last time any input was active */
static volatile TickType_t pair_since;     /* when the pair button went down (valid while pair_active) */
static volatile TickType_t last_pir;       /* last time the PIR was active */

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

/* Reported occupancy (DESIGN.md 6.4): a short PIR pulse, the one that woke the device included, is long over by
 * the time the wake's reports go out, so it holds for OCC_HOLD_MS. */
static bool occupied(void)
{
    return ms_since(last_pir) < OCC_HOLD_MS;
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
        if (pir_active) {
            last_pir = now;
        }
    }
}

static void inputs_init(void)
{
    /* Deep sleep left both pins held on the LP IO mux (st_sleep); gpio_config() routes them back to digital. */
    rtc_gpio_hold_dis(PIR_PIN);
    rtc_gpio_hold_dis(PAIR_BUTTON_PIN);
    gpio_config_t pair_cfg = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* button between the pin and GND */
    };
    ESP_ERROR_CHECK(gpio_config(&pair_cfg));
    /* Active-high PIR: no internal pull, the external pull-down sets the idle level. Active-low button: pull-up. */
    gpio_config_t pir_cfg = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = PIR_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir_cfg));
    /* Start with the real levels: a button still held from the wake-up counts at once. */
    pir_active = gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
    pair_active = gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
    TickType_t now = xTaskGetTickCount();
    pair_since = last_activity = now;
    /* Ticks start at 0 on every wake: a PIR wake counts as seen at boot, without a PIR the hold starts out expired
     * (unsigned, so "-2 holds" is long ago). */
    bool pir_wake = esp_sleep_get_ext1_wakeup_status() & (1ULL << PIR_PIN);
    last_pir = pir_active ? now : pir_wake ? 0 : -2 * pdMS_TO_TICKS(OCC_HOLD_MS);
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
 * "ds" (blob: the dataset, DESIGN.md 5.1), "view" (blob: the view on the panel with its rows; its id is the
 * shown_view_id) and "wake_n" (the wake counter, DESIGN.md 6.4). The Zigbee stack keeps the network in the same
 * partition. */
static zb_dataset_t   ds;
static render_shown_t shown;
static bool           oos;  /* the last view received was for another dataset (not stored) */
static volatile bool  view_seen;  /* a view was applied in this wake (VIEW_WAIT_MS) */

/* Every boot is a wake (a deep-sleep wake is a reset), so count boots. 24 bits, like the ids and hashes
 * (DESIGN.md 6.4); one u32 write per wake is negligible flash wear for NVS. */
static uint32_t wake_count_next(void)
{
    uint32_t n = 0;
    nvs_get_u32(nvs, "wake_n", &n);
    n = (n + 1) & 0xFFFFFF;
    nvs_set_u32(nvs, "wake_n", n);
    nvs_commit(nvs);
    return n;
}

static void blob_load(const char *key, void *p, size_t size)
{
    size_t n = size;
    if (nvs_get_blob(nvs, key, p, &n) != ESP_OK || n != size) {
        memset(p, 0, size);
    }
}

static void blob_save(const char *key, const void *p, size_t size)
{
    nvs_set_blob(nvs, key, p, size);
    nvs_commit(nvs);
}

/* Staleness (DESIGN.md 5.3): every received view is valid for SCREEN_VALID_MS. The deadline is in RTC time
 * (esp_rtc_get_time_us(): monotonic, keeps counting through deep sleep and a software restart, 0 again after a
 * power-on) and lives in RTC memory, which survives the same, so a wake sees how old the stored view is. */
#define VALID_MAGIC 0x5CEE0A11u

static RTC_NOINIT_ATTR uint32_t valid_magic;
static RTC_NOINIT_ATTR uint64_t screen_valid_until;  /* RTC time in us; 0 = never received */

static void staleness_init(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (valid_magic != VALID_MAGIC || why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT || why == ESP_RST_UNKNOWN ||
        screen_valid_until > esp_rtc_get_time_us() + SCREEN_VALID_MS * 1000ULL) {  /* the RTC counter restarted */
        valid_magic = VALID_MAGIC;
        screen_valid_until = 0;
    }
    ESP_LOGI(TAG, "rtc time %llu ms, view valid until %llu ms", esp_rtc_get_time_us() / 1000,
             screen_valid_until / 1000);
}

static bool screen_stale(void)
{
    return esp_rtc_get_time_us() > screen_valid_until;
}

/* Age of the last received view in ms, -1 if none is known. */
static int64_t screen_age_ms(void)
{
    if (!screen_valid_until) {
        return -1;
    }
    return ((int64_t)esp_rtc_get_time_us() - (int64_t)screen_valid_until) / 1000 + SCREEN_VALID_MS;
}

static void state_load(void)
{
    blob_load("ds", &ds, sizeof(ds));
    blob_load("view", &shown, sizeof(shown));
    nvs_erase_key(nvs, "snap");  /* rev 5 snapshot, no longer used */
}

/* Battery: simulated until the hardware is there. The voltage and USB power are set over the console and kept
 * in NVS ("sim_mv", "sim_usb"), so they survive the sleep, a power cycle and a reflash like a real battery's
 * charge. "bat_low" latches LOW_BATTERY: while it is set, the exit threshold applies (hysteresis). USB power
 * overrides a low battery: the device then runs from USB. */
static volatile uint16_t sim_mv = SIM_MV_DEFAULT;
static volatile bool     sim_usb;
static volatile bool     bat_latched;
static volatile bool     bat_changed;  /* the console changed the simulation: report it */

static void battery_load(void)
{
    uint16_t mv;
    if (nvs_get_u16(nvs, "sim_mv", &mv) == ESP_OK) {
        sim_mv = mv;
    }
    sim_usb     = nv_get("sim_usb");
    bat_latched = nv_get("bat_low");
    ESP_LOGI(TAG, "battery: %u mV (simulated), usb %d, latched %d", sim_mv, sim_usb, bat_latched);
}

static uint16_t battery_mv(void)
{
    return sim_mv;  /* later the ADC, sampled at rest and averaged */
}

static bool usb_powered(void)
{
    return sim_usb;  /* later the VBUS sense pin */
}

static bool battery_low(void)
{
    return !usb_powered() && battery_mv() < (bat_latched ? BAT_EXIT_MV : BAT_ENTER_MV);
}

static void bat_latch(bool on)
{
    bat_latched = on;
    nv_set("bat_low", on);
}

/* ---- State machine (DESIGN.md 3.3) ---- */
typedef enum {
    ST_BOOT, ST_UNPAIRED_WAIT, ST_PAIRING, ST_INTERVIEW, ST_REJOIN, ST_ACTIVE, ST_ERASE_NETWORK,
    ST_SHUTDOWN, ST_SLEEP, ST_LOW_BATTERY,
} state_t;

static const char *const state_names[] = {
    "BOOT", "UNPAIRED_WAIT", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
    "LOW_BATTERY",
};

static TickType_t boot_tick;
static uint32_t   wake_n;  /* this wake's number */
static char       woke_by[24];  /* what ended the sleep; BOOT itself only looks at the input levels */

/* The start of the log is lost after a deep-sleep wake on USB (the host re-enumerates the port, and on Windows no
 * data comes through at all, DESIGN.md 3.6), so the wake cause is logged again with the stack start-up. */
static void wake_cause_init(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        snprintf(woke_by, sizeof woke_by, "%s%s", pins & (1ULL << PIR_PIN) ? "PIR " : "",
                 pins & (1ULL << PAIR_BUTTON_PIN) ? "pair button" : "");
    } else if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        snprintf(woke_by, sizeof woke_by, "timer");
    } else {
        snprintf(woke_by, sizeof woke_by, "reset %d", esp_reset_reason());
    }
    ESP_LOGI(TAG, "wake: %s", woke_by);
}

/* ---- The panel (render.c): the proper screen, or the debug screen with the state, the dataset, the shown view
 * and the last writes received ---- */
#define CLEAR_EVERY 50  /* partial refreshes between full clears, against ghosting */

static SemaphoreHandle_t view_lock;  /* guards ds, shown, oos, syncing and rx_log: the Zigbee task writes them */
static volatile bool     view_dirty;
static state_t           view_state;
static bool              syncing;  /* dataset entries arrived and their commit has not */
static render_screen_t   failed;   /* RENDER_JOIN_FAILED, RENDER_ERROR or RENDER_BATTERY once this wake failed,
                                      * else RENDER_BLANK */
static render_rx_t       rx_log[RENDER_RX_N];  /* ring; rx_head is the next slot */
static int               rx_head, rx_count;
static int               partials;   /* partial refreshes since the last full clear */

static void show_state(state_t st)
{
    view_state = st;
    view_dirty = true;
}

/* What the proper screen shows (DESIGN.md 8): the house status, unless the device is pairing, joining or
 * syncing, or this wake failed. */
static render_screen_t screen_for(state_t st)
{
    if (failed != RENDER_BLANK) {
        return failed;
    }
    switch (st) {
    case ST_UNPAIRED_WAIT:  return RENDER_JOIN_FAILED;
    case ST_PAIRING:        return RENDER_SEARCHING;
    case ST_ERASE_NETWORK:  return RENDER_SEARCHING;  /* pairing follows the restart */
    case ST_INTERVIEW:      return RENDER_JOINING;
    case ST_BOOT:           return shown.view.id ? RENDER_HOUSE : RENDER_BLANK;
    case ST_SLEEP:          return shown.view.id ? RENDER_HOUSE : RENDER_SYNCING;  /* a stale view beats none */
    default:                return shown.view.id && !syncing && !oos ? RENDER_HOUSE : RENDER_SYNCING;
    }
}

#ifdef SCREEN_DUMP
/* The frame on the console for tools/fb_dump.py, which makes PNGs of it: one line per pixel row that is not all
 * white, "FB <row> <byte>:<hex> ...", each item a run of bytes (1 = white) that are not 0xFF. Compact, because
 * the console is slow and the Zigbee task logs through it too: a full hex dump took ~3 s and stalled the sync. */
static void dump_frame(const uint8_t *fb, const char *label)
{
    printf("FB begin %s\n", label);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = fb + y * EPD_STRIDE;
        bool           any = false;
        for (int i = 0; i < EPD_STRIDE; i++) {
            if (row[i] == 0xFF) {
                continue;
            }
            if (!any) {
                printf("FB %d", y);
                any = true;
            }
            printf(" %d:", i);
            for (; i < EPD_STRIDE && row[i] != 0xFF; i++) {
                printf("%02X", row[i]);
            }
        }
        if (any) {
            printf("\n");
        }
    }
    printf("FB end\n");
}

/* Every screen once, with the content of the UI spec's mockups and a few edge cases, dumped but not drawn:
 * to compare with the mockups without having to provoke each state. */
static void dump_samples(void)
{
    static const char *const rows[][RENDER_ROWS] = {
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "27|Mosogató|nedves"},
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "01|további 4|"},
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva"},
        {"Teraszajtó nyitva", "7F|Ismeretlen ikon|nyitva", "02|Nagyon hosszú nevű ajtó a pincelépcső alatt|nyitva",
         "0D|Lámpa|nagyon hosszú állapot szöveg, ami nem fér el", "13|Árvíztűrő tükörfúrógép|"},
        {0},  /* Minden OK */
        {"02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "2E|Hűtő|túl meleg",
         "2F|Fagyasztó|túl meleg", "27|Mosogató|nedves"},
        {0},
    };
    static const char *const names[] = {"problems", "overflow", "two", "edge", "ok", "problems_stale", "ok_stale"};
    static render_view_t v;
    vTaskDelay(pdMS_TO_TICKS(2000));  /* the USB console drops output until the host has reopened the port */
    for (int s = 0; s < sizeof(rows) / sizeof(rows[0]); s++) {
        memset(&v, 0, sizeof(v));
        v.screen = RENDER_HOUSE;
        v.shown.view = (zb_view_t){.id = 1, .time = 1793472360};  /* Sat 2026-10-31 18:46 */
        for (int i = 0; i < RENDER_ROWS && rows[s][i]; i++) {
            strlcpy(v.shown.row[v.shown.nrows++], rows[s][i], sizeof(v.shown.row[0]));
        }
        v.shown.active = s == 1 ? 9 : v.shown.nrows;
        v.stale = s >= 5;  /* the sync badge */
        dump_frame(render_status(&v), names[s]);
    }
    static const struct { render_screen_t screen; const char *name; } dev[] = {
        {RENDER_SEARCHING, "searching"}, {RENDER_JOINING, "joining"}, {RENDER_SYNCING, "syncing"},
        {RENDER_JOIN_FAILED, "join_failed"}, {RENDER_BATTERY, "battery"}, {RENDER_ERROR, "error"},
    };
    for (int s = 0; s < sizeof(dev) / sizeof(dev[0]); s++) {
        memset(&v, 0, sizeof(v));
        v.screen = dev[s].screen;
        dump_frame(render_status(&v), dev[s].name);
    }
}
#endif

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

/* Partial refresh of fb (~0.5 s), with a full clear every CLEAR_EVERY partials. Nothing when fb is already on
 * the panel: most writes from HA do not change the proper screen. */
static esp_err_t panel_refresh(const uint8_t *fb, const char *label)
{
    if (memcmp(fb, prev_fb, sizeof(prev_fb)) == 0) {
        return ESP_OK;
    }
#ifdef SCREEN_DUMP
    dump_frame(fb, label);
#endif
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
    ESP_LOGI(TAG, "panel: refresh #%d (%s) %lu ms, busy before %d, black %d, changed %d%s", partials, label,
             (unsigned long)ms_since(t0), busy, black, changed, err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* What the last redraw showed of the staleness: the flag, and on the debug screen the age as drawn. */
static bool drawn_stale;
static char drawn_age[RENDER_AGE_LEN];

/* The staleness on the screen is out of date (checked under view_lock). */
static bool live_changed(void)
{
    if (screen_stale() != drawn_stale) {
        return true;
    }
    if (RENDER_DEBUG_SCREEN) {
        char age[RENDER_AGE_LEN];
        render_age(age, screen_age_ms());
        return strcmp(age, drawn_age) != 0;
    }
    return false;
}

/* Draw the screen now (~0.5 s). Blocks the state loop; input_task keeps polling the inputs meanwhile. */
static void redraw(void)
{
    static render_view_t v;
    xSemaphoreTake(view_lock, portMAX_DELAY);
    view_dirty  = false;
    v.stale     = screen_stale();
    v.age_ms    = screen_age_ms();
    v.valid_ms  = SCREEN_VALID_MS;
    if (v.stale != drawn_stale) {
        ESP_LOGI(TAG, "screen %s (age %lld ms)", v.stale ? "stale" : "fresh", (long long)v.age_ms);
    }
    drawn_stale = v.stale;
    render_age(drawn_age, v.age_ms);
    v.screen    = screen_for(view_state);
    v.state     = state_names[view_state];
    v.wake      = wake_n;
    v.uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    v.ds_hash   = ds.hash;
    v.ds_count  = ds.count;
    v.shown     = shown;
    v.oos       = oos;
    v.battery_mv = battery_mv();
    v.usb       = usb_powered();
    v.bat_low   = bat_latched || battery_low();
    v.rx_count  = rx_count;
    for (int i = 0; i < rx_count; i++) {
        v.rx[i] = rx_log[(rx_head - 1 - i + RENDER_RX_N) % RENDER_RX_N];
    }
    xSemaphoreGive(view_lock);
    if (panel_refresh(render_status(&v), v.state) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

/* The state loops wait with this, so whatever arrives is drawn while it comes in. Writes that arrive during a
 * refresh are drawn together by the next one. */
static void wait_ms(uint32_t ms)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    bool dirty = view_dirty || live_changed();
    xSemaphoreGive(view_lock);
    if (dirty) {
        redraw();
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Zigbee callbacks (called in the Zigbee task): update the view, the state loop redraws. */
static void on_view(const zb_view_t *v, zb_view_result_t result)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    oos = result == ZB_VIEW_OOS;
    if (result == ZB_VIEW_OK || result == ZB_VIEW_TIME) {
        view_seen = true;
        screen_valid_until = esp_rtc_get_time_us() + SCREEN_VALID_MS * 1000ULL;
    }
    if (result == ZB_VIEW_OK) {
        render_build_rows(&shown, &ds, v);
    } else if (result == ZB_VIEW_TIME) {
        shown.view.time = v->time;
    }
    view_dirty = true;
    xSemaphoreGive(view_lock);
    if (result == ZB_VIEW_OK || result == ZB_VIEW_TIME) {
        blob_save("view", &shown, sizeof(shown));
    }
    if (result == ZB_VIEW_OK) {
        ESP_LOGI(TAG, "view %06lx: %u active lines%s", (unsigned long)v->id, shown.active, shown.active ? "" : " (All OK)");
        for (int i = 0; i < shown.nrows; i++) {
            ESP_LOGI(TAG, "  %d: %s", i + 1, shown.row[i]);
        }
    }
}

static void on_dataset(const zb_dataset_t *d)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    ds = *d;
    view_dirty = true;
    xSemaphoreGive(view_lock);
    blob_save("ds", &ds, sizeof(ds));
    ESP_LOGI(TAG, "dataset %06lx stored: %u lines", (unsigned long)d->hash, d->count);
}

static void on_rx(const zb_rx_t *ev)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    if (ev->kind == ZB_RX_ENTRY && ev->entry.tag >= 0) {
        syncing = true;
    } else if (ev->kind == ZB_RX_DS_COMMIT) {
        syncing = false;
    }
    rx_log[rx_head] = (render_rx_t){.ms = pdTICKS_TO_MS(xTaskGetTickCount()), .ev = *ev};
    rx_head = (rx_head + 1) % RENDER_RX_N;
    if (rx_count < RENDER_RX_N) {
        rx_count++;
    }
    view_dirty = true;
    xSemaphoreGive(view_lock);
}

/* ---- Console (USB-Serial-JTAG): the battery simulation, see tools/console.py ----
 * "bat <mV>" sets the simulated battery voltage, "usb 0|1" the simulated USB power, "bat" or "usb" alone print
 * them. Applied at once; the state loops pick them up. */
static void console_task(void *arg)
{
    char line[32];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int v;
        if (line[0] == '\r' || line[0] == '\n') {
            continue;
        } else if (sscanf(line, "bat %d", &v) == 1) {
            if (v < 2500 || v > 4500) {
                ESP_LOGW(TAG, "console: bat %d out of range (2500-4500)", v);
                continue;
            }
            sim_mv = v;
            nvs_set_u16(nvs, "sim_mv", v);
            nvs_commit(nvs);
        } else if (sscanf(line, "usb %d", &v) == 1) {
            sim_usb = v != 0;
            nv_set("sim_usb", sim_usb);
        } else if (strncmp(line, "bat", 3) != 0 && strncmp(line, "usb", 3) != 0) {
            ESP_LOGW(TAG, "console: unknown command (bat <mV>, usb 0|1)");
            continue;
        }
        ESP_LOGI(TAG, "battery: %u mV (simulated), usb %d, latched %d", sim_mv, sim_usb, bat_latched);
        bat_changed = true;
        view_dirty  = true;
    }
}

static void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();  /* blocking reads; writes are dropped while no host listens */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 3072, NULL, 4, NULL);
}

/* Mid-wake battery check, at most every BAT_CHECK_MS: true when the wake must end in LOW_BATTERY (the caller
 * returns ST_SHUTDOWN, which draws the battery screen). */
static bool battery_check(void)
{
    static TickType_t last;
    if (ms_since(last) < BAT_CHECK_MS) {
        return false;
    }
    last = xTaskGetTickCount();
    if (!battery_low()) {
        return false;
    }
    ESP_LOGW(TAG, "battery low (%u mV): shutting down", battery_mv());
    failed = RENDER_BATTERY;
    return true;
}

/* The reports of a wake other than the wake counter (DESIGN.md 6.4). After a rejoin they follow the wake counter
 * by REPORT_DELAY_MS, so the push that the wake counter triggers is not held up by them. */
static void report_wake_rest(void)
{
    zb_report_dataset_hash();
    zb_report_shown_view_id();
    zb_report_awake(true);
    zb_report_battery(battery_mv(), battery_low());
    zb_report_occupancy(occupied());
}

static bool rest_pending;  /* ACTIVE: report_wake_rest() still to do */

static state_t st_boot(void)
{
    /* The battery first, before the radio, the panel and any NVS write (DESIGN.md 6.6). */
    if (bat_latched) {
        if (battery_low()) {
            ESP_LOGW(TAG, "boot: battery still low (%u mV)", battery_mv());
            return ST_LOW_BATTERY;  /* the panel still shows the battery screen */
        }
        ESP_LOGI(TAG, "boot: battery recovered (%u mV%s)", battery_mv(), usb_powered() ? ", USB" : "");
        bat_latch(false);
    } else if (battery_low()) {
        ESP_LOGW(TAG, "boot: battery low (%u mV)", battery_mv());
        if (epd_init() != ESP_OK) {
            ESP_LOGE(TAG, "e-paper init failed");
        }
        failed = RENDER_BATTERY;
        return ST_SHUTDOWN;  /* the stack is not started: no reports */
    }

    bool pair_now = nv_get("pair_now");
    state_load();
    wake_n = wake_count_next();
    ESP_LOGI(TAG, "boot: pair_now=%d dataset=%06lx view=%06lx wake=%lu", pair_now, (unsigned long)ds.hash,
             (unsigned long)shown.view.id, (unsigned long)wake_n);

#ifdef SCREEN_DUMP
    dump_samples();  /* before the stack starts, so its log does not cut into the dump */
#endif
    boot_tick = xTaskGetTickCount();
    zb_callbacks_t cbs = {.view = on_view, .dataset = on_dataset, .activity = touch_activity, .rx = on_rx};
    zb_start(&cbs, ds.hash, shown.view.id, wake_n, battery_mv());

    /* MVP: full clear on every wake (DESIGN.md 3.4), in parallel with the stack start-up. epd_init() also
     * releases the pin hold and recreates the SPI bus left by epd_poweroff(). */
    if (epd_init() != ESP_OK || panel_clear("boot") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper init failed");
        vTaskDelay(portMAX_DELAY);
    }
    partials = 0;
    show_state(ST_BOOT);
    redraw();  /* the stored view, while the stack starts */

    bool factory_new = false;
    if (!zb_wait_started(STACK_START_MS, &factory_new)) {
        ESP_LOGE(TAG, "Zigbee stack did not start");
        failed = RENDER_ERROR;
        return ST_SHUTDOWN;
    }
    ESP_LOGI(TAG, "stack up after %lu ms: %s (wake: %s)", (unsigned long)ms_since(boot_tick),
             factory_new ? "no network" : "network restored", woke_by);
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
            failed = RENDER_JOIN_FAILED;  /* still not paired */
            return ST_SHUTDOWN;
        }
        if (battery_check()) {
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
        if (battery_check()) {
            zb_pairing_stop();
            return ST_SHUTDOWN;
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
    report_wake_rest();  /* Z2M has no cached state yet: everything, the wake counter last */
    zb_report_wake();
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
            zb_report_wake();  /* the only report before the push (DESIGN.md 6.4) */
            rest_pending = true;
            return ST_ACTIVE;
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {  /* network lost: allow re-pairing */
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        wait_ms(20);
    }
    ESP_LOGW(TAG, "rejoin failed");
    failed = RENDER_JOIN_FAILED;
    return ST_SHUTDOWN;
}

static state_t st_active(void)
{
    show_state(ST_ACTIVE);
    touch_activity();
    bool       last_occ = occupied();
    TickType_t last_beat = xTaskGetTickCount();
    TickType_t start = last_beat;
    bool       waiting = false;
    for (;;) {
        if (rest_pending && ms_since(start) >= REPORT_DELAY_MS) {
            rest_pending = false;
            report_wake_rest();
            last_occ = occupied();
        }
        if (!rest_pending && occupied() != last_occ) {
            last_occ = !last_occ;
            zb_report_occupancy(last_occ);
        }
        if (battery_check()) {
            return ST_SHUTDOWN;
        }
        if (bat_changed && !rest_pending) {  /* the console changed the simulation */
            bat_changed = false;
            zb_report_battery(battery_mv(), battery_low());
        }
        if (ms_since(last_beat) >= HEARTBEAT_MS) {  /* tell HA we are still awake */
            last_beat = xTaskGetTickCount();
            zb_report_shown_view_id();
            zb_report_awake(true);
            zb_log_neighbors();
        }
        if (pair_held_ms() >= RESET_HOLD_MS) {
            ESP_LOGI(TAG, "pair button held %d ms", RESET_HOLD_MS);
            return ST_ERASE_NETWORK;
        }
        if (idle_ms() >= RUN_MS) {
            if (!view_seen && ms_since(start) < VIEW_WAIT_MS) {
                if (!waiting) {
                    waiting = true;
                    ESP_LOGI(TAG, "no view yet: staying awake up to %d ms", VIEW_WAIT_MS);
                }
            } else {
                ESP_LOGI(TAG, "no activity for %d ms%s", RUN_MS, view_seen ? "" : ", and no view");
                return ST_SHUTDOWN;
            }
        }
        wait_ms(20);
    }
}

static state_t st_erase_network(void)
{
    show_state(ST_ERASE_NETWORK);
    redraw();
    nv_set("pair_now", 1);
    nvs_erase_key(nvs, "ds");    /* nothing is known about the panel any more */
    nvs_erase_key(nvs, "view");
    nvs_commit(nvs);
    ESP_LOGI(TAG, "erasing the network, restarting");
    zb_erase_network();  /* does not return */
    return ST_BOOT;
}

static state_t st_shutdown(void)
{
    bool low_battery = failed == RENDER_BATTERY;
    if (low_battery) {
        bat_latch(true);
    }
    if (zb_joined()) {
        if (low_battery) {
            zb_report_battery(battery_mv(), true);  /* HA learns why the device went quiet */
        }
        zb_report_goodbye(GOODBYE_MS);  /* not joined (failed rejoin, unpaired, stack down): nothing to send */
    }
    /* Cleanest state for power-off: full white reset, then the screen to sleep with (SLEEP on the debug screen). */
    if (panel_clear("shutdown") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper final clear failed");
    }
    partials = 0;
    show_state(low_battery ? ST_LOW_BATTERY : ST_SLEEP);
    redraw();
    ESP_LOGI(TAG, "final refresh done");
    TickType_t t0 = xTaskGetTickCount();
    epd_poweroff();
    ESP_LOGI(TAG, "panel: poweroff %lu ms", (unsigned long)ms_since(t0));
    ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
    return low_battery ? ST_LOW_BATTERY : ST_SLEEP;
}

/* Deep sleep (DESIGN.md 3.6): the PIR (D0) and the pair button (D1) wake the chip, each on its own active level,
 * so an input that is still active wakes it at once. The wake is a reset: only NVS and RTC memory carry over.
 * The internal pulls are set on the LP IO side, which the sleep code holds. Built with -DSLEEP_TIMER_S=N, a timer
 * also wakes it after N s, for bench tests (the USB port, and so a reset over it, is gone while it sleeps).
 * Built with -DSIM_SLEEP=1, the sleep is simulated instead: wait for either input, then restart. USB stays up, so
 * every wake logs and the console and a reset over USB keep working (tools/console.py, paced_write_test.py). */
static state_t st_sleep(void)
{
#ifdef SIM_SLEEP
    ESP_LOGI(TAG, "simulated deep sleep");
    while (!active()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "wake: %s", pair_active ? "pair button" : "PIR");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
#else
    const uint64_t pir = 1ULL << PIR_PIN, pair = 1ULL << PAIR_BUTTON_PIN;
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pir, PIR_ACTIVE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH
                                                                          : ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pair, ESP_EXT1_WAKEUP_ANY_LOW));
    if (PIR_ACTIVE_LEVEL) {
        rtc_gpio_pullup_dis(PIR_PIN);  /* the external pull-down sets the idle level */
    } else {
        rtc_gpio_pullup_en(PIR_PIN);
    }
    rtc_gpio_pulldown_dis(PIR_PIN);
    rtc_gpio_pullup_en(PAIR_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PAIR_BUTTON_PIN);
#ifdef SLEEP_TIMER_S
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_TIMER_S * 1000000ULL));
    ESP_LOGI(TAG, "deep sleep (timer wake after %d s)", SLEEP_TIMER_S);
#else
    ESP_LOGI(TAG, "deep sleep");
#endif
    vTaskDelay(pdMS_TO_TICKS(100));  /* let the log out before USB goes away */
    esp_deep_sleep_start();
#endif
    return ST_BOOT;
}

/* Low battery (DESIGN.md 6.6): the battery screen is on the panel and the radio is off. The PIR and the pair
 * button are ignored: only USB power wakes the device, and the boot then finds the battery recovered. Simulated
 * like SLEEP; the final firmware replaces this with esp_deep_sleep_start() with VBUS as the only wake source. */
static state_t st_low_battery(void)
{
    ESP_LOGI(TAG, "low battery: waiting for USB power");
    while (!usb_powered()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "wake: USB power");
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
    battery_load();
    console_init();
    wake_cause_init();
    staleness_init();
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
        case ST_LOW_BATTERY:   next = st_low_battery(); break;
        default:               next = st_sleep(); break;
        }
        ESP_LOGI(TAG, "%s -> %s", state_names[st], state_names[next]);
        st = next;
    }
}
