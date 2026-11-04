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
#include "zigbee.h"
#include "zigbee_internal.h"

static const char *TAG = "zb";

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

void stage_clear(void)
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

void custom_cluster_init(uint8_t ep_id)
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

void custom_cluster_deinit(uint8_t ep_id)
{
}
