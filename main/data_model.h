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
