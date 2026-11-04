#include <stdbool.h>
#include "util/utf8.h"

uint32_t utf8_next_code_point(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t       code_point = *p++;
    int            continuation_bytes = code_point >= 0xF0 ? 3 : code_point >= 0xE0 ? 2 : code_point >= 0xC0 ? 1 : 0;
    if (continuation_bytes) {
        code_point &= 0x3F >> continuation_bytes;
    }
    for (; continuation_bytes > 0 && (*p & 0xC0) == 0x80; continuation_bytes--) {
        code_point = code_point << 6 | (*p++ & 0x3F);
    }
    *text = (const char *)p;
    return continuation_bytes ? '?' : code_point;
}

void utf8_sanitize(char *text)
{
    uint8_t *p = (uint8_t *)text;
    while (*p) {
        int continuation_bytes = *p < 0x80                ? 0
                               : *p >= 0xC2 && *p < 0xE0  ? 1
                               : (*p & 0xF0) == 0xE0      ? 2
                               : *p >= 0xF0 && *p < 0xF5  ? 3
                                                          : -1;
        bool valid = continuation_bytes >= 0;
        for (int i = 1; valid && i <= continuation_bytes; i++) {
            valid = (p[i] & 0xC0) == 0x80;  /* also stops at the terminating 0 */
        }
        if (!valid || *p < 0x20 || *p == 0x7F) {
            *p++ = '?';
        } else {
            p += continuation_bytes + 1;
        }
    }
}

size_t utf8_prefix_length(const char *text, int characters)
{
    const char *p = text;
    for (; *p && characters > 0; characters--) {
        p++;
        while ((*p & 0xC0) == 0x80) {
            p++;
        }
    }
    return p - text;
}
