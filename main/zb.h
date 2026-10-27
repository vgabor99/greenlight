#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Zigbee side of the device (DESIGN.md section 6): the data model, the reports and the display input.
 * The stack runs in its own task; every function here is safe to call from the application task. */

#define ZB_DS_MAX   32  /* dataset lines, one bit each in the view (DESIGN.md 5.1) */
#define ZB_DS_CHARS 26  /* longest dataset line text */

/* The dataset (DESIGN.md 5.1): every line the screen can show; the index is the line's bit in the view. */
typedef struct {
    uint32_t hash;   /* 24-bit hash chosen by HA, 0 = no dataset */
    uint8_t  count;  /* lines 0 .. count-1 */
    char     text[ZB_DS_MAX][ZB_DS_CHARS + 1];
} zb_dataset_t;

/* A view (DESIGN.md 5.2): the four view_* attributes of one write. */
typedef struct {
    uint32_t bits;  /* bit i = dataset line i active */
    uint32_t ds;    /* dataset hash the bits refer to */
    uint32_t time;  /* local wall-clock seconds since 1970, 0 = unknown */
    uint32_t id;    /* 24-bit id chosen by HA, not 0 */
} zb_view_t;

typedef enum {
    ZB_VIEW_OK,          /* new content: draw it */
    ZB_VIEW_TIME,        /* same id as shown: only the time is new */
    ZB_VIEW_OOS,         /* out of sync: ds is not the stored dataset, nothing changes */
    ZB_VIEW_INCOMPLETE,  /* not all four attributes arrived together (HA error, or Z2M split the write) */
} zb_view_result_t;

typedef enum {
    ZB_DS_OK,         /* stored */
    ZB_DS_SAME,       /* the stored dataset already has this hash */
    ZB_DS_BAD_COUNT,  /* count > ZB_DS_MAX or hash 0 */
    ZB_DS_REJECTED,   /* an entry is missing or tagged with another hash */
} zb_ds_result_t;

/* One received write to the display input (DESIGN.md 6.5), as it arrived; for the debug screen. */
typedef enum { ZB_RX_VIEW, ZB_RX_ENTRY, ZB_RX_DS_COMMIT } zb_rx_kind_t;

typedef struct {
    zb_rx_kind_t kind;
    union {
        struct {
            zb_view_t        view;
            zb_view_result_t result;
            uint8_t          mask;   /* INCOMPLETE: which attributes arrived (bit 0 = view_bits) */
        } view;
        struct {
            int32_t tag;             /* -1 = not a valid "HHHHHH|II|text" entry */
            uint8_t index;
            char    text[ZB_DS_CHARS + 1];
        } entry;
        struct {
            uint32_t       hash;
            uint8_t        count;
            zb_ds_result_t result;
            uint8_t        missing;  /* REJECTED: the first index missing or wrongly tagged */
        } commit;
    };
} zb_rx_t;

typedef struct {
    /* A view was received and checked (every result but INCOMPLETE). For OK and TIME, the device reports
     * shown_view_id after this returns, so the view must be stored by then. Called in the Zigbee task. */
    void (*view)(const zb_view_t *view, zb_view_result_t result);
    /* A valid dataset commit: store the dataset. dataset_hash is reported after this returns. Zigbee task. */
    void (*dataset)(const zb_dataset_t *ds);
    /* HA is working with the device (dataset write, out-of-sync view): stay awake. Zigbee task. */
    void (*activity)(void);
    /* Any write to the display input and its outcome, for the debug screen. Called in the Zigbee task. */
    void (*rx)(const zb_rx_t *ev);
} zb_callbacks_t;

/* Start the Zigbee stack task. ds_hash and shown_view_id are the stored dataset and view (0 = none);
 * wake_count is this wake's number (0 .. 2^24-1, different from the previous wake's). */
void zb_start(const zb_callbacks_t *cb, uint32_t ds_hash, uint32_t shown_view_id, uint32_t wake_count);

/* Wait for the stack to report whether a network was restored from storage. Returns false on timeout. */
bool zb_wait_started(uint32_t timeout_ms, bool *factory_new);

/* True while the device is on a network (restored, or joined by pairing). */
bool zb_joined(void);

/* Network steering (pairing). zb_pairing_start() keeps retrying until zb_pairing_stop(). */
void zb_pairing_start(void);
void zb_pairing_stop(void);

/* Leave the network, erase all Zigbee data and restart the chip. Does not return. */
void zb_erase_network(void);

/* Reports to the coordinator (DESIGN.md 6.4). */
void zb_report_battery(void);               /* the three Power Configuration attributes, fixed 100 % in the MVP */
void zb_report_occupancy(bool occupied);
void zb_report_shown_view_id(void);         /* endpoint 1 Analog Input */
void zb_report_dataset_hash(void);          /* endpoint 3 Analog Input */
void zb_report_awake(bool awake);
void zb_log_neighbors(void);                /* diagnostic: parent and neighbour table to the log */
void zb_report_wake(void);                  /* the wake counter (endpoint 2): HA's push trigger */
