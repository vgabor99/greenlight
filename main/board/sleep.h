#pragma once

/* Sleep until the PIR (D0) or the pair button (D1) wakes the device (docs/behaviour.md#deep-sleep). The wake is a reset, so this
 * does not return: only NVS and RTC memory carry over. */
void sleep_until_input(void);

/* Low battery (docs/behaviour.md#low-battery): sleep until USB power arrives (the USB sense pin); the PIR and the
 * pair button are ignored. Does not return. */
void sleep_until_usb_power(void);
