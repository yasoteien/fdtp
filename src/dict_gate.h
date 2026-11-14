// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_DICT_GATE_H
#define FDTP_DICT_GATE_H

/* Only these fixed files may be used after the gate succeeds. No runtime
 * path override: dictionary data stays on the owner's mounted eMMC. */
#define DICT_GATE_DIR "/mnt/pomera-dict/JDS"
enum {
    DICT_GATE_KR = 1u << 0,
    DICT_GATE_MK = 1u << 1,
    DICT_GATE_EJ = 1u << 2,
    DICT_GATE_JE = 1u << 3
};
/* Bit set for each usable book, zero when the mount's provenance fails.
 * This checks access only; it neither parses entries nor copies data.
 * Linux only in the build; other platforms return zero if compiled. */
unsigned dict_gate_ok(void);
/* One book (a DICT_GATE_* bit) opened read-only after the same checks, for
 * the reader; -1 when the gate fails for it. The caller closes it. */
int dict_gate_open(unsigned book);
/* The stock ATOK28 dictionaries beside them (ATOK/share/atok28 on the same
 * mount): only these files, after the mount's checks and a check of the
 * file's head. -1 when the gate fails. */
enum { DICT_GATE_ATOK_SPORTS, DICT_GATE_ATOK_ANIME, DICT_GATE_ATOK_ENTERTAINMENT, DICT_GATE_ATOK_NAMES,
       DICT_GATE_ATOK_COUNT };
int dict_gate_open_atok(unsigned which);

#endif
