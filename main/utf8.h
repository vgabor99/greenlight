#pragma once

#include <stddef.h>
#include <stdint.h>

/* UTF-8 helpers for the dataset text (DESIGN.md 5.1). */

/* The code point at *text, and *text moved past it; '?' for a broken sequence. */
uint32_t utf8_next_code_point(const char **text);

/* Replace each byte that is not part of a valid sequence (a stray continuation byte, a truncated or overlong
 * sequence) and each control character with '?', in place. */
void utf8_sanitize(char *text);

/* Byte length of the first `characters` characters (code points) of text. */
size_t utf8_prefix_length(const char *text, int characters);
