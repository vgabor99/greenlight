#include "app/device_state.h"

static const char *const state_names[] = {
    "BOOT", "UNPAIRED", "PAIRING", "INTERVIEW", "REJOIN", "ACTIVE", "ERASE_NETWORK", "SHUTDOWN", "SLEEP",
    "LOW_BATTERY",
};
_Static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_LOW_BATTERY + 1, "a name for every state");

const char *device_state_name(device_state_t state)
{
    return state_names[state];
}
