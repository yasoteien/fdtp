// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* evdev keyboard (internal to the platform). Reads the keyd virtual
 * keyboard: the built-in keyboard (tc3589x-keypad) is grabbed by keyd, so
 * keys only appear on keyd's uinput device. Found by name, looked for again
 * once per second while missing (keyd restart, not started yet). */
#ifndef RMP_INPUT_H
#define RMP_INPUT_H

#include "rmp_platform.h"

#define RMP_INPUT_NAME "keyd virtual keyboard"
/* The Pomera DM250's Alt and right Shift are not on the key matrix but on
 * gpio-keys (with the power key), which keyd does not read. Read without a
 * grab (logind keeps the power key); only these two keys are taken. */
#define RMP_INPUT_EXTRA_NAME "gpio-keys"
#define RMP_INPUT_DIR "/dev/input"
#define RMP_INPUT_RESCAN 1.0      /* seconds between searches while missing */
#define RMP_INPUT_QUEUE 256

/* Opens if present (not finding it is not an error). grab: EVIOCGRAB. */
void rmp_input_open(int grab, double now);
void rmp_input_close(void);
int  rmp_input_fd(void);          /* -1 while missing */
int  rmp_input_extra_fd(void);    /* gpio-keys, -1 while missing */
int  rmp_input_connected(void);
/* Reads what is available (never blocks), rescans when missing and due.
 * Returns queued events (<= max). */
int  rmp_input_poll(rmp_key_event *out, int max, double now);
/* 1 when events are queued but not yet returned. */
int  rmp_input_pending(void);
/* State as of the events returned by rmp_input_poll. */
int  rmp_input_key_down(int code);

#endif
