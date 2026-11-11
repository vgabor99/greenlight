#pragma once

/* The device states (docs/behaviour.md#state-machine) */
typedef enum {
    STATE_BOOT, STATE_UNPAIRED, STATE_PAIRING, STATE_INTERVIEW, STATE_REJOIN, STATE_ACTIVE, STATE_ERASE_NETWORK,
    STATE_SHUTDOWN, STATE_SLEEP, STATE_LOW_BATTERY,
} device_state_t;

/* "BOOT", "UNPAIRED", ... as in the log and on the debug screen */
const char *device_state_name(device_state_t state);

/* Why this wake failed. From then on the screen shows the failure whatever the state
 * (docs/display.md#device-screens). */
typedef enum {
    WAKE_FAILURE_NONE,
    WAKE_FAILURE_UNPAIRED,     /* still not paired when the wake ends: sleep with how to pair */
    WAKE_FAILURE_JOIN_FAILED,  /* the rejoin failed (retried next wake) */
    WAKE_FAILURE_ERROR,        /* the Zigbee stack did not start */
    WAKE_FAILURE_LOW_BATTERY,  /* the wake ends in LOW_BATTERY, or tries to report it */
} wake_failure_t;
