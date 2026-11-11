#pragma once

/* The device state machine (docs/behaviour.md#state-machine); the states are in app/device_state.h. Runs the states
 * from BOOT on. Does not return: every wake ends in a sleep, which restarts the chip. */
void state_machine_run(void);
