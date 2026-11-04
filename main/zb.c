/* Zigbee side of the device. See DESIGN.md section 6 for the data model this implements (rev 7): a single
 * endpoint with Basic, Power Configuration, Occupancy Sensing, the OTA Upgrade client (declared only) and the
 * custom cluster 0xFC8A. Its writable attributes carry the view and the dataset; its reported attributes are
 * the device state (wake_count, the HA push trigger; shown_view_id; dataset_hash; awake). Z2M needs the
 * external converter in z2m/greenlight.mjs to publish them. */

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "hex.h"
#include "utf8.h"
#include "zb.h"

static const char *TAG = "zb";

#define ZB_EP        1
#define COORD_EP     1
#define CUSTOM_CLUSTER_ID 0xFC8A

/* Custom cluster attribute ids (DESIGN.md 6.4, 6.5); 0x0000-0x0007 are retired, 0x0010-0x0014 reserved */
#define ATTR_VIEW_BITS     0x0020  /* .. ATTR_VIEW_ID: the four view_* attributes, in zb_view_t order */
#define ATTR_VIEW_ID       0x0023
#define ATTR_DS_ENTRY      0x0030
#define ATTR_DS_COMMIT     0x0031
#define ATTR_BATTERY_MV    0x0013  /* exact battery voltage in mV (DESIGN.md 6.6), reported, read-only */
#define ATTR_WAKE_COUNT    0x0040  /* reported state, read-only */
#define ATTR_SHOWN_VIEW_ID 0x0041
#define ATTR_DATASET_HASH  0x0042
#define ATTR_AWAKE         0x0043

#define VIEW_ALL  0x0F                         /* all four view_* attributes received */
#define ENTRY_MAX (6 + 1 + 2 + 1 + ZB_PART_CHARS)  /* "HHHHHH|II|text": 36 characters */

/* Battery (DESIGN.md 6.6): batteryAlarmState bit 0 (BatteryMinThreshold) = low, Z2M's battery_low */
#define BATTERY_ALARM_LOW 0x00000001u

/* End device timeout requested from the parent. The longest value (16384 min) made the join fail with a TCLK
 * exchange error on Z2M, so use the value of the Espressif examples; revisit when the device sleeps for hours. */
#define ZB_ALL_CHANNELS_MASK 0x07FFF800U  /* channels 11-26 */
#define ED_TIMEOUT EZB_NWK_ED_TIMEOUT_64MIN
#define POLL_MS    50    /* how often the awake device polls its parent for queued messages (ZIGBEE_PAYLOAD.md 6) */
#define GOODBYE_RETRY_MS 1000  /* zb_report_goodbye(): resend an unconfirmed report this often */

#define BIT_STARTED     BIT0
#define BIT_FACTORY_NEW BIT1
#define BIT_JOINED      BIT2
#define BIT_LEFT        BIT3
#define BIT_ACK_OCC     BIT4  /* Z2M confirmed the goodbye's occupancy report */
#define BIT_ACK_AWAKE   BIT5  /* ... and its awake report */

static EventGroupHandle_t s_events;
static zb_callbacks_t     s_cb;
static uint32_t           s_shown_view_id;
static uint32_t           s_ds_hash;
static uint32_t           s_wake_count;
static uint16_t           s_battery_mv;
static esp_timer_handle_t s_retry_timer;
static volatile bool      s_pairing;
static uint8_t            s_retry_mode;

/* Attribute storage of the custom cluster (the stack keeps the written values here). */
static uint32_t s_view_attr[4];
static uint32_t s_commit_attr;
static uint8_t  s_entry_attr[ENTRY_MAX + 1];
static uint32_t s_state_attr[3];  /* wake_count, shown_view_id, dataset_hash */
static bool     s_awake_attr;
static char     s_build_id[20];
static char    s_date_code[20];

/* ---- helpers ---- */

/* Add an attribute to a cluster descriptor and say so if the stack refuses it. */
#define ADD_ATTR(fn, desc, id, value)                                                       \
    do {                                                                                    \
        ezb_err_t e_ = fn(desc, id, value);                                                 \
        if (e_ != EZB_ERR_NONE) {                                                           \
            ESP_LOGW(TAG, "adding attribute 0x%04x to " #desc " failed: 0x%04x", (id), e_); \
        }                                                                                   \
    } while (0)

/* Build a ZCL character string (length byte + text) in buf. */
static void zcl_string(char *buf, size_t size, const char *text)
{
    size_t n = strlen(text);
    if (n > size - 1) {
        n = size - 1;
    }
    buf[0] = (char)n;
    memcpy(buf + 1, text, n);
}

/* want_rsp asks the coordinator for a default response: only the goodbye reports do, so nothing else adds a downlink. */
static void report_attr(uint16_t cluster, uint16_t attr, bool want_rsp)
{
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .dst_ep     = COORD_EP,
            .src_ep     = ZB_EP,
            .cluster_id = cluster,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {.manuf_specific = 0, .direction = EZB_ZCL_CMD_DIRECTION_TO_CLI, .dis_default_rsp = !want_rsp},
        },
        .payload = {.attr_id = attr},
    };
    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report cluster 0x%04x attr 0x%04x failed: 0x%04x", cluster, attr, err);
    }
}

static void set_value(uint16_t cluster, uint16_t attr, void *value)
{
    ezb_zcl_status_t st = ezb_zcl_set_attr_value(ZB_EP, cluster, EZB_ZCL_CLUSTER_SERVER, attr, EZB_ZCL_STD_MANUF_CODE,
                                                 value, false);
    if (st != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set cluster 0x%04x attr 0x%04x failed: 0x%02x", cluster, attr, st);
    }
}

static void set_and_report(uint16_t cluster, uint16_t attr, void *value)
{
    set_value(cluster, attr, value);
    report_attr(cluster, attr, false);
}

/* LiPo charge in half percents (ZCL batteryPercentageRemaining, 200 = 100 %), interpolated in a rough open-circuit
 * voltage curve. */
static uint8_t battery_half_percent(uint16_t mv)
{
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 12}, {3700, 30}, {3800, 50}, {4000, 80}, {4200, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (mv <= curve[0].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv < curve[i].mv) {
            uint32_t pct = curve[i - 1].pct * 2 + (uint32_t)(curve[i].pct - curve[i - 1].pct) * 2 *
                                                      (mv - curve[i - 1].mv) / (curve[i].mv - curve[i - 1].mv);
            return (uint8_t)pct;
        }
    }
    return 200;
}

static void report_shown_view_id_nolock(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_SHOWN_VIEW_ID, &s_shown_view_id);
}

static void report_dataset_hash_nolock(void)
{
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_DATASET_HASH, &s_ds_hash);
}

/* ---- display input (custom cluster writes, DESIGN.md 6.5) ---- */

static zb_view_t s_view_in;       /* view_* values received since the last apply */
static uint8_t   s_view_mask;     /* which of them (bit 0 = view_bits) */
static bool      s_view_posted;   /* apply_view() is queued */

/* Dataset entry parts received since the last accepted commit, by line index and part; tag -1 = none. */
typedef struct {
    int32_t tag;
    bool    more;  /* another part of the line follows */
    char    text[ZB_PART_CHARS + 1];
} stage_part_t;

static stage_part_t s_stage[ZB_DS_MAX][ZB_PARTS];

static void emit(const zb_rx_t *ev)
{
    if (s_cb.rx) {
        s_cb.rx(ev);
    }
}

static void activity(void)
{
    if (s_cb.activity) {
        s_cb.activity();
    }
}

/* n hex digits as a number, or -1 */
static int32_t hex_value(const uint8_t *p, int n)
{
    int32_t v = 0;
    for (int i = 0; i < n; i++) {
        int d = hex_digit_value((char)p[i]);
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* Runs in the Zigbee task after the frame that carried the view_* writes has been processed, so all four
 * records of one write are in s_view_in whatever their order. */
static void apply_view(void *ctx)
{
    zb_rx_t ev = {.kind = ZB_RX_VIEW};
    ev.view.view = s_view_in;
    ev.view.mask = s_view_mask;
    s_view_posted = false;
    s_view_mask   = 0;
    const zb_view_t *v = &ev.view.view;

    if (ev.view.mask != VIEW_ALL || v->id == 0) {
        ESP_LOGW(TAG, "view %06lx incomplete: attributes 0x%x", (unsigned long)v->id, ev.view.mask);
        ev.view.result = ZB_VIEW_INCOMPLETE;
        emit(&ev);
        return;
    }
    if (v->ds == 0 || v->ds != s_ds_hash) {
        ESP_LOGW(TAG, "view %06lx out of sync: for dataset %06lx, stored %06lx", (unsigned long)v->id,
                 (unsigned long)v->ds, (unsigned long)s_ds_hash);
        ev.view.result = ZB_VIEW_OOS;
        if (s_cb.view) {
            s_cb.view(v, ev.view.result);
        }
        emit(&ev);
        activity();
        report_dataset_hash_nolock();
        return;
    }
    ev.view.result = v->id == s_shown_view_id ? ZB_VIEW_TIME : ZB_VIEW_OK;
    ESP_LOGI(TAG, "view %06lx %s: bits %08lx time %lu", (unsigned long)v->id,
             ev.view.result == ZB_VIEW_OK ? "accepted" : "time only", (unsigned long)v->bits, (unsigned long)v->time);
    if (s_cb.view) {
        s_cb.view(v, ev.view.result);
    }
    s_shown_view_id = v->id;
    emit(&ev);
    report_shown_view_id_nolock();
}

static void view_write(uint16_t attr_id, uint32_t value)
{
    uint32_t *field[] = {&s_view_in.bits, &s_view_in.ds, &s_view_in.time, &s_view_in.id};
    int       i       = attr_id - ATTR_VIEW_BITS;
    *field[i] = value;
    s_view_mask |= 1 << i;
    if (!s_view_posted) {
        if (esp_zigbee_task_queue_post(apply_view, NULL) == ESP_OK) {
            s_view_posted = true;
        } else {
            ESP_LOGE(TAG, "could not queue the view");
        }
    }
}

static void stage_clear(void)
{
    for (int i = 0; i < ZB_DS_MAX; i++) {
        for (int p = 0; p < ZB_PARTS; p++) {
            s_stage[i][p].tag = -1;
        }
    }
}

/* Line i of the dataset with this hash, its parts joined into out (ZB_DS_CHARS + 1 bytes). False while a part
 * is missing or tagged with another hash. */
static bool stage_join(int i, int32_t hash, char *out)
{
    size_t n = 0;
    for (int p = 0; p < ZB_PARTS; p++) {
        if (s_stage[i][p].tag != hash) {
            return false;
        }
        size_t m = strlen(s_stage[i][p].text);
        if (n + m > ZB_DS_CHARS) {
            ESP_LOGW(TAG, "ds_commit: line %02d longer than %d bytes, cut", i, ZB_DS_CHARS);
            m = ZB_DS_CHARS - n;
        }
        memcpy(&out[n], s_stage[i][p].text, m);
        n += m;
        out[n] = 0;
        if (!s_stage[i][p].more) {
            utf8_sanitize(out);  /* a cut can leave half a character */
            return true;
        }
    }
    return false;  /* not reached: the last part never has "more" (ds_entry_write) */
}

/* "HHHHHH|II|text": stage one part of a line. II: bits 0-4 the line index, bits 5-6 the part, bit 7 "more"
 * (another part of the line follows). */
static void ds_entry_write(const uint8_t *s)
{
    size_t  len = s[0];
    zb_rx_t ev  = {.kind = ZB_RX_ENTRY, .entry = {.tag = -1}};
    int32_t tag = -1, ii = -1;
    if (len >= 10 && len <= ENTRY_MAX && s[7] == '|' && s[10] == '|') {
        tag = hex_value(&s[1], 6);
        ii  = hex_value(&s[8], 2);
    }
    int  idx  = ii & 0x1F;
    int  part = (ii >> 5) & 0x03;
    bool more = ii & 0x80;
    if (tag <= 0 || ii < 0 || part >= ZB_PARTS || (more && part == ZB_PARTS - 1)) {
        ESP_LOGW(TAG, "ds_entry: not a valid entry (length %u)", (unsigned)len);
        memcpy(ev.entry.text, &s[1], len > ZB_PART_CHARS ? ZB_PART_CHARS : len);
        utf8_sanitize(ev.entry.text);
    } else {
        size_t        n  = len - 10;
        stage_part_t *st = &s_stage[idx][part];
        st->tag  = tag;
        st->more = more;
        memset(st->text, 0, sizeof(st->text));
        memcpy(st->text, &s[11], n);
        utf8_sanitize(st->text);
        ev.entry.tag   = tag;
        ev.entry.index = idx;
        ev.entry.part  = part;
        ev.entry.more  = more;
        memcpy(ev.entry.text, st->text, sizeof(ev.entry.text));
        ESP_LOGI(TAG, "ds_entry %02d.%d%s for %06lx: \"%s\" (%u bytes)", idx, part, more ? "+" : "",
                 (unsigned long)tag, st->text, (unsigned)n);
    }
    emit(&ev);
    activity();
}

static void ds_commit_write(uint32_t v)
{
    zb_rx_t ev = {.kind = ZB_RX_DS_COMMIT, .commit = {.hash = v & 0xFFFFFF, .count = v >> 24}};
    uint32_t hash  = ev.commit.hash;
    uint8_t  count = ev.commit.count;
    if (count > ZB_DS_MAX || hash == 0) {
        ESP_LOGW(TAG, "ds_commit %06lx: bad count %u", (unsigned long)hash, count);
        ev.commit.result = ZB_DS_BAD_COUNT;
    } else if (hash == s_ds_hash) {
        ESP_LOGI(TAG, "ds_commit %06lx: already stored", (unsigned long)hash);
        ev.commit.result = ZB_DS_SAME;
        report_dataset_hash_nolock();  /* re-acknowledge */
    } else {
        ev.commit.result = ZB_DS_OK;
        static zb_dataset_t ds;
        memset(&ds, 0, sizeof(ds));
        ds.hash  = hash;
        ds.count = count;
        for (int i = 0; i < count; i++) {
            if (!stage_join(i, (int32_t)hash, ds.text[i])) {
                ESP_LOGW(TAG, "ds_commit %06lx rejected: entry %02d missing a part or for another dataset",
                         (unsigned long)hash, i);
                ev.commit.result  = ZB_DS_REJECTED;
                ev.commit.missing = i;
                break;
            }
        }
        if (ev.commit.result == ZB_DS_OK) {
            stage_clear();
            if (s_cb.dataset) {
                s_cb.dataset(&ds);
            }
            s_ds_hash = hash;
            ESP_LOGI(TAG, "ds_commit %06lx accepted: %u entries", (unsigned long)hash, count);
            report_dataset_hash_nolock();
        }
    }
    emit(&ev);
    activity();
}

static ezb_zcl_status_t custom_check_value(uint16_t attr_id, uint8_t ep_id, void *value)
{
    if (attr_id == ATTR_DS_ENTRY && ((uint8_t *)value)[0] > ENTRY_MAX) {
        ESP_LOGW(TAG, "ds_entry too long: %u characters", ((uint8_t *)value)[0]);
        return EZB_ZCL_STATUS_INVALID_VALUE;
    }
    return EZB_ZCL_STATUS_SUCCESS;
}

static void custom_write_attr(uint8_t ep_id, uint16_t attr_id, void *new_value, uint16_t manuf_code)
{
    uint32_t v;
    if (attr_id >= ATTR_VIEW_BITS && attr_id <= ATTR_VIEW_ID) {
        memcpy(&v, new_value, sizeof(v));
        view_write(attr_id, v);
    } else if (attr_id == ATTR_DS_ENTRY) {
        ds_entry_write(new_value);
    } else if (attr_id == ATTR_DS_COMMIT) {
        memcpy(&v, new_value, sizeof(v));
        ds_commit_write(v);
    } else if ((attr_id >= ATTR_WAKE_COUNT && attr_id <= ATTR_AWAKE) || attr_id == ATTR_BATTERY_MV) {
        /* our own set_and_report() (the attributes are read-only for the network) */
    } else {
        ESP_LOGW(TAG, "write to unknown attribute 0x%04x", attr_id);
    }
}

static ezb_zcl_status_t custom_process_cmd(const ezb_zcl_cmd_hdr_t *header, const uint8_t *payload, uint16_t payload_length)
{
    return EZB_ZCL_STATUS_UNSUP_CMD;  /* the cluster has no commands, only writable attributes */
}

static uint8_t custom_disc_cmd(bool is_recv, uint8_t **list)
{
    *list = NULL;
    return 0;
}

static void custom_cluster_init(uint8_t ep_id)
{
    ezb_zcl_custom_cluster_handlers_t handlers = {
        .cluster_id     = CUSTOM_CLUSTER_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .process_cmd_cb = custom_process_cmd,
        .check_value_cb = custom_check_value,
        .write_attr_cb  = custom_write_attr,
        .cmd_disc_cb    = custom_disc_cmd,
    };
    ezb_zcl_custom_cluster_handlers_register(&handlers);
}

static void custom_cluster_deinit(uint8_t ep_id)
{
}

/* ---- commissioning ---- */

static void retry_timer_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

/* Diagnostic: log the parent and the other neighbours, to see whether a rejoin after a restart picks a new parent. */
static void log_neighbors_nolock(const char *when)
{
    static const char *rel[] = {"PARENT", "child", "sibling", "other"};
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t n;
    ESP_LOGI(TAG, "neighbours (%s): own 0x%04hx", when, ezb_nwk_get_short_address());
    while (ezb_nwk_get_next_neighbor(&it, &n) == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "  %-7s 0x%04hx %02x%02x%02x%02x%02x%02x%02x%02x type %u lqi %u rssi %d", rel[n.relationship & 3],
                 n.short_addr, n.ieee_addr.u8[7], n.ieee_addr.u8[6], n.ieee_addr.u8[5], n.ieee_addr.u8[4], n.ieee_addr.u8[3],
                 n.ieee_addr.u8[2], n.ieee_addr.u8[1], n.ieee_addr.u8[0], n.device_type, n.lqi, n.rssi);
    }
}

static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    s_retry_mode = mode;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack started, initializing");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            bool factory_new = ezb_bdb_is_factory_new();
            ESP_LOGI(TAG, "%s", factory_new ? "factory new: no network" : "network restored");
            if (!factory_new) {
                log_neighbors_nolock("restored");
            }
            xEventGroupSetBits(s_events, BIT_STARTED | (factory_new ? BIT_FACTORY_NEW : BIT_JOINED));
        } else {
            ESP_LOGW(TAG, "%s failed with status 0x%02x, retrying", ezb_app_signal_to_string(type), status);
            if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
                /* The stack is up but cannot restore the stored network (yet); let the app go on and show REJOIN. */
                xEventGroupSetBits(s_events, BIT_STARTED);
            }
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 250);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext_pan;
            ezb_nwk_get_extended_panid(&ext_pan);
            ESP_LOGI(TAG, "joined: PAN 0x%04hx, channel %d, short address 0x%04hx", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_pairing = false;
            log_neighbors_nolock("joined");
            xEventGroupSetBits(s_events, BIT_JOINED);
            xEventGroupClearBits(s_events, BIT_FACTORY_NEW);
        } else if (s_pairing) {
            ESP_LOGW(TAG, "steering failed with status 0x%02x, retrying", status);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGI(TAG, "left the network");
        xEventGroupSetBits(s_events, BIT_LEFT);
        break;
    default:
        ESP_LOGI(TAG, "signal %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

static void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        /* Tell the stack the write is fine; custom_write_attr() checks the content. */
        ezb_zcl_set_attr_value_message_t *m = message;
        ESP_LOGI(TAG, "set attr: cluster 0x%04x attr 0x%04x type 0x%02x size %u (stack status 0x%02x)", m->info.cluster_id,
                 m->in.attribute.id, m->in.attribute.data.type, m->in.attribute.data.size, m->info.status);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = message;
        ESP_LOGI(TAG, "default response: cluster 0x%04x cmd 0x%02x status 0x%02x", rsp->info.cluster_id,
                 rsp->in.rsp_to_cmd, rsp->in.status_code);
        if (rsp->in.rsp_to_cmd == EZB_ZCL_CMD_REPORT_ATTRIBUTES) {  /* only the goodbye reports ask for one */
            if (rsp->info.cluster_id == EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING) {
                xEventGroupSetBits(s_events, BIT_ACK_OCC);
            } else if (rsp->info.cluster_id == CUSTOM_CLUSTER_ID) {
                xEventGroupSetBits(s_events, BIT_ACK_AWAKE);
            }
        }
    } break;
    default:
        ESP_LOGI(TAG, "ZCL core action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

/* ---- data model ---- */

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();

    /* Basic */
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    static char manufacturer[16], model[24];
    zcl_string(manufacturer, sizeof(manufacturer), "Greenlight");
    zcl_string(model, sizeof(model), "greenlight-eink42");
    zcl_string(s_build_id, sizeof(s_build_id), esp_app_get_description()->version);
    zcl_string(s_date_code, sizeof(s_date_code), esp_app_get_description()->date);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, s_build_id);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_DATE_CODE_ID, s_date_code);

    /* Power Configuration: the battery as measured at boot (simulated for now, DESIGN.md 6.6) */
    ezb_zcl_power_config_cluster_server_config_t power_cfg = {
        .mains_voltage               = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_DEFAULT_VALUE,
        .mains_voltage_min_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD_DEFAULT_VALUE,
        .mains_voltage_max_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MAX_THRESHOLD_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(&power_cfg, EZB_ZCL_CLUSTER_SERVER);
    uint8_t  voltage = s_battery_mv / 100, percent = battery_half_percent(s_battery_mv);
    uint8_t  size = EZB_ZCL_POWER_CONFIG_BATTERY_SIZE_OTHER, quantity = 1, rated = 37;
    uint16_t ahr = 100;  /* 1000 mAh in units of 10 mAh */
    uint32_t alarm = 0;
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &voltage);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &percent);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID, &size);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID, &quantity);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID, &rated);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_A_HR_RATING_ID, &ahr);
    ADD_ATTR(ezb_zcl_power_config_cluster_desc_add_attr, power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm);

    /* Occupancy Sensing: the PIR */
    ezb_zcl_occupancy_sensing_cluster_server_config_t occ_cfg = {
        .occupancy                    = 0,
        .occupancy_sensor_type        = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_PIR,
        .occupancy_sensor_type_bitmap = EZB_ZCL_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_PIR,
    };
    ezb_zcl_cluster_desc_t occupancy = ezb_zcl_occupancy_sensing_create_cluster_desc(&occ_cfg, EZB_ZCL_CLUSTER_SERVER);

    /* OTA Upgrade client: declared so the cluster list stays final (DESIGN.md 6.3), not implemented yet */
    ezb_zcl_ota_upgrade_cluster_client_config_t ota_cfg = {
        .upgrade_server_id    = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset          = EZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEFAULT_VALUE,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t ota = ezb_zcl_ota_upgrade_create_cluster_desc(&ota_cfg, EZB_ZCL_CLUSTER_CLIENT);

    /* Custom cluster: the view and the dataset input, and the reported state. The stack takes the storage size of
     * a string attribute from the initial value, so register a full-length placeholder (not a valid entry). */
    s_entry_attr[0] = ENTRY_MAX;
    memset(&s_entry_attr[1], ' ', ENTRY_MAX);
    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = CUSTOM_CLUSTER_ID,
        .init_func   = custom_cluster_init,
        .deinit_func = custom_cluster_deinit,
    };
    ezb_zcl_cluster_desc_t custom = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    for (int i = 0; i < 4; i++) {  /* view_bits, view_ds, view_time, view_id */
        ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_VIEW_BITS + i, EZB_ZCL_ATTR_TYPE_UINT32,
                                             EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, &s_view_attr[i]);
    }
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_DS_ENTRY, EZB_ZCL_ATTR_TYPE_STRING,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, s_entry_attr);
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_DS_COMMIT, EZB_ZCL_ATTR_TYPE_UINT32,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, &s_commit_attr);
    s_state_attr[0] = s_wake_count;
    s_state_attr[1] = s_shown_view_id;
    s_state_attr[2] = s_ds_hash;
    for (int i = 0; i < 3; i++) {  /* wake_count, shown_view_id, dataset_hash */
        ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_WAKE_COUNT + i, EZB_ZCL_ATTR_TYPE_UINT32,
                                             EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_state_attr[i]);
    }
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_AWAKE, EZB_ZCL_ATTR_TYPE_BOOL,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_awake_attr);
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_BATTERY_MV, EZB_ZCL_ATTR_TYPE_UINT16,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, &s_battery_mv);

    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = ZB_EP,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, basic));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, power));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, occupancy));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, custom));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, ota));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));

    ezb_zcl_core_action_handler_register(core_action_handler);
    return ESP_OK;
}

static void stack_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config          = {.ed_timeout = ED_TIMEOUT, .keep_alive = POLL_MS},
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config           = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZB_ALL_CHANNELS_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    /* Sleepy end device: the parent queues messages for us and we poll it every POLL_MS (the first attempt with
     * rx-on-when-idle lost about half of Z2M's writes). Must be set before joining; changing it needs a re-pair. */
    ezb_nwk_set_rx_on_when_idle(false);

    ESP_ERROR_CHECK(create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ---- public API ---- */

void zb_start(const zb_callbacks_t *cb, uint32_t ds_hash, uint32_t shown_view_id, uint32_t wake_count,
              uint16_t battery_mv)
{
    s_cb            = *cb;
    s_battery_mv    = battery_mv;
    s_ds_hash       = ds_hash;
    s_shown_view_id = shown_view_id;
    s_wake_count    = wake_count;
    stage_clear();
    s_events     = xEventGroupCreate();
    const esp_timer_create_args_t targs = {.callback = retry_timer_cb, .name = "zb_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    xTaskCreate(stack_task, "zigbee", 6144, NULL, 5, NULL);
}

bool zb_wait_started(uint32_t timeout_ms, bool *factory_new)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_STARTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_STARTED)) {
        return false;
    }
    *factory_new = (xEventGroupGetBits(s_events) & BIT_FACTORY_NEW) != 0;
    return true;
}

bool zb_joined(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_JOINED) != 0;  /* false before zb_start() */
}

void zb_pairing_start(void)
{
    s_pairing = true;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "network steering started (0x%04x)", err);
}

void zb_pairing_stop(void)
{
    s_pairing = false;
    esp_timer_stop(s_retry_timer);
}

void zb_erase_network(void)
{
    if (zb_joined()) {
        xEventGroupClearBits(s_events, BIT_LEFT);
        esp_zigbee_lock_acquire(portMAX_DELAY);
        ezb_bdb_reset_via_local_action();  /* leave the network, clear the Zigbee data */
        esp_zigbee_lock_release();
        xEventGroupWaitBits(s_events, BIT_LEFT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    esp_zigbee_factory_reset();  /* clears all datasets and restarts the chip */
}

void zb_report_battery(uint16_t mv, bool low)
{
    uint8_t  voltage = mv / 100, percent = battery_half_percent(mv);
    uint32_t alarm   = low ? BATTERY_ALARM_LOW : 0;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* BatteryVoltage (0x0020) is set but not reported: the stack answers "not found" for it. A read gets it. */
    ezb_zcl_set_attr_value(ZB_EP, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, EZB_ZCL_STD_MANUF_CODE, &voltage, false);
    set_and_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &percent);
    set_and_report(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID, &alarm);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_BATTERY_MV, &mv);  /* the exact voltage: 0x0020 has 100 mV steps */
    esp_zigbee_lock_release();
}

void zb_report_occupancy(bool occupied)
{
    uint8_t v = occupied ? 1 : 0;
    ESP_LOGI(TAG, "report occupancy %u", v);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, &v);
    esp_zigbee_lock_release();
}

void zb_report_shown_view_id(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    report_shown_view_id_nolock();
    esp_zigbee_lock_release();
}

void zb_report_dataset_hash(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    report_dataset_hash_nolock();
    esp_zigbee_lock_release();
}

void zb_report_awake(bool awake)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
}

bool zb_report_goodbye(uint32_t timeout_ms)
{
    uint8_t    occ = 0;
    bool       awake = false;
    TickType_t start = xTaskGetTickCount();
    xEventGroupClearBits(s_events, BIT_ACK_OCC | BIT_ACK_AWAKE);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_value(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, &occ);
    set_value(CUSTOM_CLUSTER_ID, ATTR_AWAKE, &awake);
    esp_zigbee_lock_release();
    const EventBits_t both = BIT_ACK_OCC | BIT_ACK_AWAKE;
    EventBits_t       got = 0;
    int               sends = 0;
    while ((got & both) != both && pdTICKS_TO_MS(xTaskGetTickCount() - start) < timeout_ms) {
        esp_zigbee_lock_acquire(portMAX_DELAY);  /* resend what is not confirmed yet */
        if (!(got & BIT_ACK_OCC)) {
            report_attr(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, true);
        }
        if (!(got & BIT_ACK_AWAKE)) {
            report_attr(CUSTOM_CLUSTER_ID, ATTR_AWAKE, true);
        }
        esp_zigbee_lock_release();
        sends++;
        got = xEventGroupWaitBits(s_events, both, pdFALSE, pdTRUE, pdMS_TO_TICKS(GOODBYE_RETRY_MS));
    }
    bool ok = (got & both) == both;
    ESP_LOGI(TAG, "goodbye %s: %d send(s), %lu ms (occupancy %s, awake %s)", ok ? "confirmed" : "timed out", sends,
             (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start), got & BIT_ACK_OCC ? "ok" : "lost",
             got & BIT_ACK_AWAKE ? "ok" : "lost");
    return ok;
}

void zb_report_wake(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(CUSTOM_CLUSTER_ID, ATTR_WAKE_COUNT, &s_wake_count);
    esp_zigbee_lock_release();
}

void zb_log_neighbors(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    log_neighbors_nolock("heartbeat");
    esp_zigbee_lock_release();
}
