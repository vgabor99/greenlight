#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The data HA and the device share (docs/data-model.md), whatever carries it and whatever shows it. */

#define DATASET_MAX_LINES 32  /* dataset lines, one bit each in the view (docs/data-model.md#dataset) */
#define LINE_MAX_BYTES    64  /* longest dataset line text, UTF-8 bytes (32 accented letters) */

/* The dataset (docs/data-model.md#dataset): every line the screen can show; the index is the line's bit in the view.
 * Stored in NVS as is: keep the layout. */
typedef struct {
    uint32_t hash;   /* 24-bit hash chosen by HA, 0 = no dataset */
    uint8_t  count;  /* lines 0 .. count-1 */
    char     text[DATASET_MAX_LINES][LINE_MAX_BYTES + 1];
} dataset_t;

/* A view (docs/data-model.md#view): which dataset lines are active. Stored in NVS as is: keep the layout. */
typedef struct {
    uint32_t bits;          /* bit i = dataset line i active */
    uint32_t dataset_hash;  /* the dataset the bits refer to */
    uint32_t time;          /* local wall-clock seconds since 1970, 0 = unknown */
    uint32_t id;            /* 24-bit id chosen by HA, not 0 */
} view_t;

/* What a view means for the device (docs/data-model.md#view). */
typedef enum {
    VIEW_NEW,          /* new content: show it */
    VIEW_TIME_ONLY,    /* the id of the shown view: only the time is new */
    VIEW_OUT_OF_SYNC,  /* for another dataset than the stored one: nothing changes */
    VIEW_INCOMPLETE,   /* not a whole view (a field missing, or id 0): nothing changes */
} view_result_t;

/* The rule for a whole view, given the stored dataset's hash and the shown view's id (0 = none). */
view_result_t data_model_check_view(const view_t *view, uint32_t dataset_hash, uint32_t shown_view_id);

/* Line i of the dataset is active in the view. */
bool data_model_line_active(const view_t *view, int line);

/* A dataset line, "II|name|state" (docs/data-model.md#line-format), split up. The parts point into the text. */
typedef struct {
    uint8_t     icon_id;      /* docs/data-model.md#line-icons; may be one the device does not know */
    const char *name;
    int         name_length;  /* bytes; the name is not 0-terminated */
    const char *state;        /* 0-terminated, may be empty */
} line_t;

/* Split a line; false if it is not in the line format (then *line is not filled). */
bool data_model_parse_line(const char *text, line_t *line);
