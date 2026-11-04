/* The display input (DESIGN.md 6.5): HA's writes to the custom cluster. A view comes as four view_* attributes in
 * one write; a dataset as ds_entry writes (one per line part) followed by a ds_commit. */

#include <string.h>
#include "esp_log.h"
#include "esp_zigbee.h"
#include "hex.h"
#include "utf8.h"
#include "zigbee.h"
#include "zigbee_internal.h"

static const char *TAG = "zb";

#define VIEW_ATTRIBUTES_ALL 0x0F  /* all four view_* attributes received */

static zigbee_view_t pending_view;       /* view_* values received since the last apply */
static uint8_t       pending_view_mask;  /* which of them (bit 0 = view_bits) */
static bool          apply_view_queued;

/* Dataset entry parts received since the last accepted commit, by line index and part */
typedef struct {
    int32_t dataset_hash;  /* -1 = none */
    bool    more;          /* another part of the line follows */
    char    text[ZIGBEE_PART_MAX_BYTES + 1];
} staged_part_t;

static staged_part_t staged_parts[ZIGBEE_DATASET_MAX_LINES][ZIGBEE_LINE_MAX_PARTS];

/* ---- Callbacks ---- */

static void notify_received_write(const zigbee_received_write_t *write)
{
    if (zigbee_context.callbacks.on_received_write) {
        zigbee_context.callbacks.on_received_write(write);
    }
}

static void notify_activity(void)
{
    if (zigbee_context.callbacks.on_activity) {
        zigbee_context.callbacks.on_activity();
    }
}

static void notify_view(const zigbee_view_t *view, zigbee_view_result_t result)
{
    if (zigbee_context.callbacks.on_view) {
        zigbee_context.callbacks.on_view(view, result);
    }
}

/* ---- View ---- */

/* Runs in the Zigbee task after the frame that carried the view_* writes has been processed, so all four
 * records of one write are in pending_view whatever their order. */
static void apply_view(void *unused)
{
    zigbee_received_write_t write = {.kind = ZIGBEE_WRITE_VIEW};
    write.view.view          = pending_view;
    write.view.received_mask = pending_view_mask;
    apply_view_queued = false;
    pending_view_mask = 0;
    const zigbee_view_t *view = &write.view.view;

    if (write.view.received_mask != VIEW_ATTRIBUTES_ALL || view->id == 0) {
        ESP_LOGW(TAG, "view %06lx incomplete: attributes 0x%x", (unsigned long)view->id, write.view.received_mask);
        write.view.result = ZIGBEE_VIEW_INCOMPLETE;
        notify_received_write(&write);
        return;
    }
    if (view->dataset_hash == 0 || view->dataset_hash != zigbee_context.dataset_hash) {
        ESP_LOGW(TAG, "view %06lx out of sync: for dataset %06lx, stored %06lx", (unsigned long)view->id,
                 (unsigned long)view->dataset_hash, (unsigned long)zigbee_context.dataset_hash);
        write.view.result = ZIGBEE_VIEW_OUT_OF_SYNC;
        notify_view(view, write.view.result);
        notify_received_write(&write);
        notify_activity();
        zigbee_report_dataset_hash_lock_held();  /* tells HA which dataset the device has */
        return;
    }
    write.view.result = view->id == zigbee_context.shown_view_id ? ZIGBEE_VIEW_TIME_ONLY : ZIGBEE_VIEW_NEW;
    ESP_LOGI(TAG, "view %06lx %s: bits %08lx time %lu", (unsigned long)view->id,
             write.view.result == ZIGBEE_VIEW_NEW ? "accepted" : "time only", (unsigned long)view->bits,
             (unsigned long)view->time);
    notify_view(view, write.view.result);
    zigbee_context.shown_view_id = view->id;
    notify_received_write(&write);
    zigbee_report_shown_view_id_lock_held();
}

static void handle_view_write(uint16_t attribute, uint32_t value)
{
    uint32_t *fields[] = {&pending_view.bits, &pending_view.dataset_hash, &pending_view.time, &pending_view.id};
    int       index = attribute - ATTR_VIEW_BITS;
    *fields[index] = value;
    pending_view_mask |= 1 << index;
    if (!apply_view_queued) {
        if (esp_zigbee_task_queue_post(apply_view, NULL) == ESP_OK) {
            apply_view_queued = true;
        } else {
            ESP_LOGE(TAG, "could not queue the view");
        }
    }
}

/* ---- Dataset ---- */

static void clear_staged_parts(void)
{
    for (int line = 0; line < ZIGBEE_DATASET_MAX_LINES; line++) {
        for (int part = 0; part < ZIGBEE_LINE_MAX_PARTS; part++) {
            staged_parts[line][part].dataset_hash = -1;
        }
    }
}

void zigbee_display_input_init(void)
{
    clear_staged_parts();
}

/* Line `line` of the dataset with this hash, its parts joined into out (ZIGBEE_LINE_MAX_BYTES + 1 bytes). False
 * while a part is missing or tagged with another hash. */
static bool join_staged_line(int line, int32_t dataset_hash, char *out)
{
    size_t length = 0;
    for (int part = 0; part < ZIGBEE_LINE_MAX_PARTS; part++) {
        const staged_part_t *staged = &staged_parts[line][part];
        if (staged->dataset_hash != dataset_hash) {
            return false;
        }
        size_t part_length = strlen(staged->text);
        if (length + part_length > ZIGBEE_LINE_MAX_BYTES) {
            ESP_LOGW(TAG, "ds_commit: line %02d longer than %d bytes, cut", line, ZIGBEE_LINE_MAX_BYTES);
            part_length = ZIGBEE_LINE_MAX_BYTES - length;
        }
        memcpy(&out[length], staged->text, part_length);
        length += part_length;
        out[length] = 0;
        if (!staged->more) {
            utf8_sanitize(out);  /* a cut can leave half a character */
            return true;
        }
    }
    return false;  /* not reached: the last part never has "more" (parse_entry_header) */
}

/* `digits` hex digits as a number, or -1 */
static int32_t parse_hex(const uint8_t *text, int digits)
{
    int32_t value = 0;
    for (int i = 0; i < digits; i++) {
        int digit = hex_digit_value((char)text[i]);
        if (digit < 0) {
            return -1;
        }
        value = (value << 4) | digit;
    }
    return value;
}

typedef struct {
    int32_t dataset_hash;
    int     line;
    int     part;
    bool    more;
} entry_header_t;

/* "HHHHHH|II|" at the start of the ZCL string s (length byte first): the dataset hash, and II: bits 0-4 the line
 * index, bits 5-6 the part, bit 7 "more" (another part of the line follows). False if it is not a valid entry. */
static bool parse_entry_header(const uint8_t *s, entry_header_t *header)
{
    size_t  length = s[0];
    int32_t dataset_hash = -1, line_and_part = -1;
    if (length >= 10 && length <= ENTRY_MAX_CHARS && s[7] == '|' && s[10] == '|') {
        dataset_hash  = parse_hex(&s[1], 6);
        line_and_part = parse_hex(&s[8], 2);
    }
    header->dataset_hash = dataset_hash;
    header->line         = line_and_part & 0x1F;
    header->part         = (line_and_part >> 5) & 0x03;
    header->more         = line_and_part & 0x80;
    return dataset_hash > 0 && line_and_part >= 0 && header->part < ZIGBEE_LINE_MAX_PARTS &&
           !(header->more && header->part == ZIGBEE_LINE_MAX_PARTS - 1);
}

/* "HHHHHH|II|text": stage one part of a line. */
static void handle_dataset_entry_write(const uint8_t *s)
{
    size_t                  length = s[0];
    zigbee_received_write_t write = {.kind = ZIGBEE_WRITE_DATASET_ENTRY, .entry = {.dataset_hash = -1}};
    entry_header_t          header;
    if (!parse_entry_header(s, &header)) {
        ESP_LOGW(TAG, "ds_entry: not a valid entry (length %u)", (unsigned)length);
        memcpy(write.entry.text, &s[1], length > ZIGBEE_PART_MAX_BYTES ? ZIGBEE_PART_MAX_BYTES : length);
        utf8_sanitize(write.entry.text);
    } else {
        size_t         text_length = length - 10;
        staged_part_t *staged = &staged_parts[header.line][header.part];
        staged->dataset_hash = header.dataset_hash;
        staged->more         = header.more;
        memset(staged->text, 0, sizeof(staged->text));
        memcpy(staged->text, &s[11], text_length);
        utf8_sanitize(staged->text);
        write.entry.dataset_hash = header.dataset_hash;
        write.entry.index        = header.line;
        write.entry.part         = header.part;
        write.entry.more         = header.more;
        memcpy(write.entry.text, staged->text, sizeof(write.entry.text));
        ESP_LOGI(TAG, "ds_entry %02d.%d%s for %06lx: \"%s\" (%u bytes)", header.line, header.part,
                 header.more ? "+" : "", (unsigned long)header.dataset_hash, staged->text, (unsigned)text_length);
    }
    notify_received_write(&write);
    notify_activity();
}

/* Join the staged lines of the dataset `hash` into dataset. -1 when every line is there, else the first line that
 * is missing a part or has one for another dataset. */
static int assemble_dataset(uint32_t hash, uint8_t count, zigbee_dataset_t *dataset)
{
    memset(dataset, 0, sizeof(*dataset));
    dataset->hash  = hash;
    dataset->count = count;
    for (int line = 0; line < count; line++) {
        if (!join_staged_line(line, (int32_t)hash, dataset->text[line])) {
            return line;
        }
    }
    return -1;
}

/* The commit value: bits 0-23 the dataset hash, bits 24-31 the line count. */
static void handle_dataset_commit_write(uint32_t value)
{
    zigbee_received_write_t write = {
        .kind   = ZIGBEE_WRITE_DATASET_COMMIT,
        .commit = {.hash = value & 0xFFFFFF, .count = value >> 24},
    };
    uint32_t hash  = write.commit.hash;
    uint8_t  count = write.commit.count;
    if (count > ZIGBEE_DATASET_MAX_LINES || hash == 0) {
        ESP_LOGW(TAG, "ds_commit %06lx: bad count %u", (unsigned long)hash, count);
        write.commit.result = ZIGBEE_COMMIT_BAD_COUNT;
    } else if (hash == zigbee_context.dataset_hash) {
        ESP_LOGI(TAG, "ds_commit %06lx: already stored", (unsigned long)hash);
        write.commit.result = ZIGBEE_COMMIT_ALREADY_STORED;
        zigbee_report_dataset_hash_lock_held();  /* re-acknowledge */
    } else {
        static zigbee_dataset_t dataset;
        int                     first_missing = assemble_dataset(hash, count, &dataset);
        if (first_missing >= 0) {
            ESP_LOGW(TAG, "ds_commit %06lx rejected: entry %02d missing a part or for another dataset",
                     (unsigned long)hash, first_missing);
            write.commit.result        = ZIGBEE_COMMIT_REJECTED;
            write.commit.first_missing = first_missing;
        } else {
            write.commit.result = ZIGBEE_COMMIT_STORED;
            clear_staged_parts();  /* used up */
            if (zigbee_context.callbacks.on_dataset) {
                zigbee_context.callbacks.on_dataset(&dataset);
            }
            zigbee_context.dataset_hash = hash;
            ESP_LOGI(TAG, "ds_commit %06lx accepted: %u entries", (unsigned long)hash, count);
            zigbee_report_dataset_hash_lock_held();
        }
    }
    notify_received_write(&write);
    notify_activity();
}

/* ---- Custom cluster handlers ---- */

static ezb_zcl_status_t check_attribute_value(uint16_t attribute, uint8_t endpoint, void *value)
{
    if (attribute == ATTR_DATASET_ENTRY && ((uint8_t *)value)[0] > ENTRY_MAX_CHARS) {
        ESP_LOGW(TAG, "ds_entry too long: %u characters", ((uint8_t *)value)[0]);
        return EZB_ZCL_STATUS_INVALID_VALUE;
    }
    return EZB_ZCL_STATUS_SUCCESS;
}

static void handle_attribute_write(uint8_t endpoint, uint16_t attribute, void *new_value, uint16_t manufacturer_code)
{
    uint32_t value;
    if (attribute >= ATTR_VIEW_BITS && attribute <= ATTR_VIEW_ID) {
        memcpy(&value, new_value, sizeof(value));
        handle_view_write(attribute, value);
    } else if (attribute == ATTR_DATASET_ENTRY) {
        handle_dataset_entry_write(new_value);
    } else if (attribute == ATTR_DATASET_COMMIT) {
        memcpy(&value, new_value, sizeof(value));
        handle_dataset_commit_write(value);
    } else if ((attribute >= ATTR_WAKE_COUNT && attribute <= ATTR_AWAKE) || attribute == ATTR_BATTERY_MV) {
        /* our own reports (the attributes are read-only for the network) */
    } else {
        ESP_LOGW(TAG, "write to unknown attribute 0x%04x", attribute);
    }
}

static ezb_zcl_status_t handle_command(const ezb_zcl_cmd_hdr_t *header, const uint8_t *payload,
                                       uint16_t payload_length)
{
    return EZB_ZCL_STATUS_UNSUP_CMD;  /* the cluster has no commands, only writable attributes */
}

static uint8_t list_commands(bool received, uint8_t **commands)
{
    *commands = NULL;
    return 0;
}

void zigbee_display_input_register_handlers(uint8_t endpoint)
{
    ezb_zcl_custom_cluster_handlers_t handlers = {
        .cluster_id     = CUSTOM_CLUSTER_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_SERVER,
        .process_cmd_cb = handle_command,
        .check_value_cb = check_attribute_value,
        .write_attr_cb  = handle_attribute_write,
        .cmd_disc_cb    = list_commands,
    };
    ezb_zcl_custom_cluster_handlers_register(&handlers);
}
