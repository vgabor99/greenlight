#pragma once

/* Sleep until the PIR (D0) or the pair button (D1) wakes the device (DESIGN.md 3.6). The wake is a reset, so this
 * does not return: only NVS and RTC memory carry over. */
void sleep_until_input(void);

/* Low battery (DESIGN.md 6.6): sleep until USB power arrives; the PIR and the pair button are ignored. Does not
 * return. */
void sleep_until_usb_power(void);
