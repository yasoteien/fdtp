// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_JIS_H
#define FDTP_JIS_H
#include <stddef.h>
#include <stdint.h>

/* The first three values are fixed (saved state and tests use them). */
typedef enum {
    JIS_UTF8, JIS_SHIFT_JIS, JIS_EUC_JP,
    JIS_ISO2022JP,           /* "JIS": 7-bit, escape sequences switch sets */
    JIS_UTF16LE, JIS_UTF16BE
} JisEncoding;
#define JIS_ENCODING_COUNT 6
const char *jis_encoding_name(JisEncoding encoding);
/* Strict scalar decoder. Returns bytes consumed, or 0 for invalid/incomplete
 * input. Shift_JIS includes Windows CP932 NEC/IBM extensions. Stateless: for
 * JIS_ISO2022JP it decodes from the initial ASCII state and an escape
 * sequence counts as invalid; use jis_decode_step for ISO-2022-JP text. */
size_t jis_decode_one(const unsigned char *bytes, size_t length,
                      JisEncoding encoding, uint32_t *scalar);

/* Stateful decoder for every encoding. *state starts at JIS_STATE_INITIAL
 * (and must be carried from one call to the next; only ISO-2022-JP uses it).
 * Returns bytes consumed, or 0 for invalid/incomplete input (state is then
 * unchanged). An escape sequence, SO or SI consumes bytes, changes *state and
 * sets *scalar to JIS_NO_SCALAR. ISO-2022-JP accepts ESC ( B/J/I, ESC $ @/B,
 * ESC $ ( D (JIS X 0212), ESC $ ( O/Q (read as JIS X 0208), ESC & @ and
 * SO/SI; its JIS X 0208 set also reads the CP932 NEC rows (e.g. U+2460).
 * UTF-16 pairs surrogates; a lone surrogate or odd last byte is invalid. */
#define JIS_STATE_INITIAL 0u
#define JIS_NO_SCALAR UINT32_MAX
size_t jis_decode_step(const unsigned char *bytes, size_t length,
                       JisEncoding encoding, unsigned *state, uint32_t *scalar);
/* Length of a byte order mark for encoding at the start of bytes (3 for
 * UTF-8, 2 for UTF-16), otherwise 0. Conversions keep a BOM as U+FEFF. */
size_t jis_bom_length(const unsigned char *bytes, size_t length,
                      JisEncoding encoding);

/* UTF-8 wins when valid, otherwise the encoding with fewer invalid bytes wins
 * (Shift_JIS on ties). Supply the complete byte sequence for exact detection.
 * Only ever returns UTF-8, Shift_JIS or EUC-JP. */
JisEncoding jis_detect(const unsigned char *bytes, size_t length);
/* Any encoding: a BOM picks UTF-8/UTF-16LE/UTF-16BE; 7-bit text holding a
 * JIS escape sequence (ESC $ @, ESC $ B, ESC $ ( D, ESC ( J, ESC ( I) is
 * ISO-2022-JP; otherwise as jis_detect. UTF-16 without a BOM is not guessed. */
JisEncoding jis_detect_all(const unsigned char *bytes, size_t length);
/* Allocates a NUL-terminated output; length excludes NUL and may include embedded
 * NULs. Caller frees it. Returns 0 on success, -1 on invalid input/allocation
 * failure. Replacement consumes one invalid byte (two for UTF-16 when
 * available) and emits U+FFFD. */
int jis_to_utf8(const unsigned char *bytes, size_t length, JisEncoding encoding,
                int replace_invalid, char **output, size_t *output_length);
/* Strict conversion: unrepresentable Unicode, malformed UTF-8 and allocation
 * failure return -1. Output ownership and length follow jis_to_utf8.
 * ISO-2022-JP output switches with ESC $ B (JIS X 0208, CP932 NEC rows too),
 * ESC $ ( D (JIS X 0212), ESC ( I (halfwidth katakana), ESC ( J (U+00A5,
 * U+203E) and ends in ESC ( B; U+001B, U+000E and U+000F are
 * unrepresentable there. UTF-16 output has no BOM. */
int jis_from_utf8(const char *bytes, size_t length, JisEncoding encoding,
                  unsigned char **output, size_t *output_length);
#endif
