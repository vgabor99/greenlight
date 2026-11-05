#pragma once

/* The device state machine (docs/behaviour.md#state-machine) */
typedef enum {
    STATE_BOOT, STATE_UNPAIRED, STATE_PAIRING, STATE_INTERVIEW, STATE_REJOIN, STATE_ACTIVE, STATE_ERASE_NETWORK,
    STATE_SHUTDOWN, STATE_SLEEP, STATE_LOW_BATTERY,
} device_state_t;

/* "BOOT", "UNPAIRED", ... as in the log and on the debug screen */
const char *device_state_name(device_state_t state);

/* Run the states from BOOT on. Does not return: every wake ends in a sleep, which restarts the chip. */
void state_machine_run(void);
