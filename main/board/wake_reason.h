#pragma once

/* Find out and log what ended the sleep: "PIR", "pair button", "timer" or "reset <reason>". */
void wake_reason_init(void);

/* What ended the sleep; BOOT itself only looks at the input levels. */
const char *wake_reason_text(void);
