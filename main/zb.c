/* Zigbee side of the device. See DESIGN.md section 6 for the data model this implements:
 *   endpoint 1: Basic, Power Configuration, Occupancy Sensing, Binary Input (awake), Analog Input
 *   (shown_hash) and the custom cluster 0xFC8A whose writable attributes carry the display input;
 *   endpoint 2: Analog Input (wake counter, the HA push trigger). */

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
#include "zb.h"

static const char *TAG = "zb";

#define ZB_EP        1
#define ZB_EP_WAKE   2  /* second endpoint, only for the wake counter's Analog Input */
#define COORD_EP     1
#define CUSTOM_CLUSTER_ID 0xFC8A

/* Custom cluster attribute ids (DESIGN.md 6.5) */
#define ATTR_LINE1   0x0000  /* .. ATTR_LINE1 + 5 */
#define ATTR_FOOTER  0x0006
#define ATTR_COMMIT  0x0007

#define SLOT_BUF 64         /* storage of one string slot: length byte + up to 63 characters */
#define SLOT_MAX ZB_SLOT_CHARS  /* longest string we accept */

/* MVP battery: fixed 100 % (DESIGN.md 6.6) */
#define BATTERY_VOLTAGE_100MV 42
#define BATTERY_PERCENT_HALF  200

/* End device timeout requested from the parent. The longest value (16384 min) made the join fail with a TCLK
 * exchange error on Z2M, so use the value of the Espressif examples; revisit when the device sleeps for hours. */
#define ZB_ALL_CHANNELS_MASK 0x07FFF800U  /* channels 11-26 */
#define ED_TIMEOUT EZB_NWK_ED_TIMEOUT_64MIN
#define POLL_MS    50    /* how often the awake device polls its parent for queued messages (ZIGBEE_PAYLOAD.md 6) */

#define BIT_STARTED     BIT0
#define BIT_FACTORY_NEW BIT1
#define BIT_JOINED      BIT2
#define BIT_LEFT        BIT3

static EventGroupHandle_t s_events;
static zb_callbacks_t     s_cb;
static uint32_t           s_shown_hash;
static uint32_t           s_wake_count;
static esp_timer_handle_t s_retry_timer;
static volatile bool      s_pairing;
static uint8_t            s_retry_mode;

/* Raw string slots as the coordinator wrote them (length byte first). */
static uint8_t s_slot[ZB_MAX_LINES + 1][SLOT_BUF];
static char    s_build_id[20];
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

static void report_attr_ep(uint8_t ep, uint16_t cluster, uint16_t attr)
{
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .dst_ep     = COORD_EP,
            .src_ep     = ep,
            .cluster_id = cluster,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {.manuf_specific = 0, .direction = EZB_ZCL_CMD_DIRECTION_TO_CLI, .dis_default_rsp = 1},
        },
        .payload = {.attr_id = attr},
    };
    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "report cluster 0x%04x attr 0x%04x failed: 0x%04x", cluster, attr, err);
    }
}

static void report_attr(uint16_t cluster, uint16_t attr)
{
    report_attr_ep(ZB_EP, cluster, attr);
}

static void set_and_report_ep(uint8_t ep, uint16_t cluster, uint16_t attr, void *value)
{
    ezb_zcl_status_t st = ezb_zcl_set_attr_value(ep, cluster, EZB_ZCL_CLUSTER_SERVER, attr, EZB_ZCL_STD_MANUF_CODE,
                                                 value, false);
    if (st != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set ep %u cluster 0x%04x attr 0x%04x failed: 0x%02x", ep, cluster, attr, st);
    }
    report_attr_ep(ep, cluster, attr);
}

static void set_and_report(uint16_t cluster, uint16_t attr, void *value)
{
    set_and_report_ep(ZB_EP, cluster, attr, value);
}

static void report_shown_hash_nolock(uint32_t hash)
{
    float f = (float)hash;  /* 24-bit hashes are exact in a single-precision float */
    set_and_report(EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, 0x0055, &f);
}

/* ---- display input (custom cluster writes) ---- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "HHHHHH|text" from a slot; returns the hash, or -1 if the slot has no valid tag. */
static int32_t slot_tag(const uint8_t *slot, const char **text, size_t *text_len)
{
    size_t len = slot[0];
    if (len < 7 || slot[7] != '|') {
        return -1;
    }
    int32_t h = 0;
    for (int i = 0; i < 6; i++) {
        int d = hex_digit((char)slot[1 + i]);
        if (d < 0) {
            return -1;
        }
        h = (h << 4) | d;
    }
    *text = (const char *)&slot[8];
    *text_len = len - 7;
    return h;
}

static void emit_commit(uint32_t hash, uint8_t count, zb_commit_result_t result, uint8_t line, int32_t tag)
{
    if (s_cb.rx) {
        zb_rx_t ev = {.kind = ZB_RX_COMMIT, .line = line, .hash = hash, .count = count, .result = result, .tag = tag};
        s_cb.rx(&ev);
    }
}

static void handle_commit(uint32_t v)
{
    uint8_t  count = v >> 24;
    uint32_t hash  = v & 0xFFFFFF;
    if (count > ZB_MAX_LINES) {
        ESP_LOGW(TAG, "commit: bad line count %u", count);
        emit_commit(hash, count, ZB_COMMIT_BAD_COUNT, 0, -1);
        return;
    }
    if (hash == s_shown_hash) {
        ESP_LOGI(TAG, "commit: hash %06lx already shown", (unsigned long)hash);
        emit_commit(hash, count, ZB_COMMIT_SAME, 0, -1);
        report_shown_hash_nolock(s_shown_hash);  /* re-acknowledge */
        return;
    }
    static zb_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.hash  = hash;
    snap.count = count;
    for (int i = 0; i < count; i++) {
        const char *text;
        size_t      n;
        int32_t     tag = slot_tag(s_slot[i], &text, &n);
        if (tag < 0 || (uint32_t)tag != hash) {
            ESP_LOGW(TAG, "commit %06lx rejected: line %d missing or tagged %ld", (unsigned long)hash, i + 1, (long)tag);
            emit_commit(hash, count, ZB_COMMIT_REJECTED, i + 1, tag);
            return;
        }
        if (n > ZB_LINE_CHARS) {
            n = ZB_LINE_CHARS;
        }
        memcpy(snap.line[i], text, n);
    }
    /* The footer is not tagged: take whatever was written last. */
    size_t fl = s_slot[ZB_MAX_LINES][0];
    if (fl > ZB_LINE_CHARS) {
        fl = ZB_LINE_CHARS;
    }
    memcpy(snap.footer, &s_slot[ZB_MAX_LINES][1], fl);

    s_shown_hash = hash;
    ESP_LOGI(TAG, "commit %06lx accepted: %u lines", (unsigned long)hash, count);
    emit_commit(hash, count, ZB_COMMIT_OK, 0, -1);
    if (s_cb.snapshot) {
        s_cb.snapshot(&snap);
    }
    report_shown_hash_nolock(hash);
}

static ezb_zcl_status_t custom_check_value(uint16_t attr_id, uint8_t ep_id, void *value)
{
    ESP_LOGI(TAG, "check attr 0x%04x (first byte %u)", attr_id, ((uint8_t *)value)[0]);
    if (attr_id <= ATTR_FOOTER) {
        if (((uint8_t *)value)[0] > SLOT_MAX) {
            return EZB_ZCL_STATUS_INVALID_VALUE;
        }
    }
    return EZB_ZCL_STATUS_SUCCESS;
}

static void custom_write_attr(uint8_t ep_id, uint16_t attr_id, void *new_value, uint16_t manuf_code)
{
    ESP_LOGI(TAG, "write attr 0x%04x (first byte %u)", attr_id, ((uint8_t *)new_value)[0]);
    if (attr_id <= ATTR_FOOTER) {
        const uint8_t *s = new_value;
        uint8_t        len = s[0] > SLOT_MAX ? SLOT_MAX : s[0];
        int            idx = attr_id == ATTR_FOOTER ? ZB_MAX_LINES : attr_id - ATTR_LINE1;
        memset(s_slot[idx], 0, SLOT_BUF);
        s_slot[idx][0] = len;
        memcpy(&s_slot[idx][1], &s[1], len);
        if (s_cb.rx) {
            zb_rx_t ev = {.kind = attr_id == ATTR_FOOTER ? ZB_RX_FOOTER : ZB_RX_LINE, .line = idx + 1};
            memcpy(ev.text, &s[1], len);
            s_cb.rx(&ev);
        }
        if (attr_id == ATTR_FOOTER && s_cb.footer) {
            char text[ZB_LINE_CHARS + 1] = {0};
            memcpy(text, &s[1], len > ZB_LINE_CHARS ? ZB_LINE_CHARS : len);
            s_cb.footer(text);
        }
    } else if (attr_id == ATTR_COMMIT) {
        uint32_t v;
        memcpy(&v, new_value, sizeof(v));
        handle_commit(v);
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
        /* Tell the stack the write is fine. HA sends one attribute per write (DESIGN.md 6.5): batched writes of long
         * strings were refused by the coordinator or stopped after the first attribute. */
        ezb_zcl_set_attr_value_message_t *m = message;
        ESP_LOGI(TAG, "set attr: cluster 0x%04x attr 0x%04x type 0x%02x size %u (stack status 0x%02x)", m->info.cluster_id,
                 m->in.attribute.id, m->in.attribute.data.type, m->in.attribute.data.size, m->info.status);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = message;
        ESP_LOGI(TAG, "default response, status 0x%02x", rsp->in.status_code);
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

    /* Power Configuration: fixed 100 % for the MVP */
    ezb_zcl_power_config_cluster_server_config_t power_cfg = {
        .mains_voltage               = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_DEFAULT_VALUE,
        .mains_voltage_min_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD_DEFAULT_VALUE,
        .mains_voltage_max_threshold = EZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MAX_THRESHOLD_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(&power_cfg, EZB_ZCL_CLUSTER_SERVER);
    uint8_t  voltage = BATTERY_VOLTAGE_100MV, percent = BATTERY_PERCENT_HALF;
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

    /* Binary Input: awake. Analog Input: shown_hash */
    ezb_zcl_binary_input_cluster_server_config_t bin_cfg = {.out_of_service = false, .present_value = false, .status_flags = 0};
    ezb_zcl_cluster_desc_t binary = ezb_zcl_binary_input_create_cluster_desc(&bin_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_analog_input_cluster_server_config_t ana_cfg = {.out_of_service = false, .present_value = (float)s_shown_hash, .status_flags = 0};
    ezb_zcl_cluster_desc_t analog = ezb_zcl_analog_input_create_cluster_desc(&ana_cfg, EZB_ZCL_CLUSTER_SERVER);

    /* Custom cluster: the display input slots. The stack takes the storage size of a string attribute from the
     * initial value, so register full-length placeholders (they are not valid tags, so a commit rejects them). */
    for (int i = 0; i <= ZB_MAX_LINES; i++) {
        s_slot[i][0] = SLOT_MAX;
        memset(&s_slot[i][1], ' ', SLOT_MAX);
    }
    ezb_zcl_custom_cluster_config_t custom_cfg = {
        .cluster_id  = CUSTOM_CLUSTER_ID,
        .init_func   = custom_cluster_init,
        .deinit_func = custom_cluster_deinit,
    };
    ezb_zcl_cluster_desc_t custom = ezb_zcl_custom_create_cluster_desc(&custom_cfg, EZB_ZCL_CLUSTER_SERVER);
    for (int i = 0; i <= ZB_MAX_LINES; i++) {  /* lines 1..6 and the footer */
        ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_LINE1 + i, EZB_ZCL_ATTR_TYPE_STRING,
                                             EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, s_slot[i]);
    }
    uint32_t commit = 0;
    ezb_zcl_custom_cluster_desc_add_attr(custom, ATTR_COMMIT, EZB_ZCL_ATTR_TYPE_UINT32,
                                         EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE, &commit);

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
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, binary));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, analog));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, custom));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));

    /* Endpoint 2: Analog Input = wake counter. A cluster can appear only once per endpoint, and Z2M's generic
     * mapping exposes it as a second analog input (DESIGN.md 6.4). */
    ezb_zcl_analog_input_cluster_server_config_t wake_cfg = {.out_of_service = false, .present_value = (float)s_wake_count, .status_flags = 0};
    ezb_zcl_cluster_desc_t wake = ezb_zcl_analog_input_create_cluster_desc(&wake_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_af_ep_config_t ep2_cfg = {
        .ep_id              = ZB_EP_WAKE,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = EZB_ZHA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep2 = ezb_af_create_endpoint_desc(&ep2_cfg);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep2, wake));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep2));
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

void zb_start(const zb_callbacks_t *cb, uint32_t shown_hash, uint32_t wake_count)
{
    s_cb         = *cb;
    s_shown_hash = shown_hash;
    s_wake_count = wake_count;
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
    return (xEventGroupGetBits(s_events) & BIT_JOINED) != 0;
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

void zb_report_battery(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* BatteryVoltage (0x0020) is declared but not reported: the stack answers "not found" for it. */
    report_attr(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
    report_attr(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID);
    esp_zigbee_lock_release();
}

void zb_report_occupancy(bool occupied)
{
    uint8_t v = occupied ? 1 : 0;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, &v);
    esp_zigbee_lock_release();
}

void zb_report_shown_hash(uint32_t hash)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    s_shown_hash = hash;
    report_shown_hash_nolock(hash);
    esp_zigbee_lock_release();
}

void zb_report_awake(bool awake)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report(EZB_ZCL_CLUSTER_ID_BINARY_INPUT, 0x0055, &awake);
    esp_zigbee_lock_release();
}

void zb_report_wake(void)
{
    float f = (float)s_wake_count;  /* kept below 2^24 by the caller, so exact in a single-precision float */
    esp_zigbee_lock_acquire(portMAX_DELAY);
    set_and_report_ep(ZB_EP_WAKE, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, 0x0055, &f);
    esp_zigbee_lock_release();
}

void zb_log_neighbors(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    log_neighbors_nolock("heartbeat");
    esp_zigbee_lock_release();
}
