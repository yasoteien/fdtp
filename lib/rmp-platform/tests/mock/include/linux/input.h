// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Minimal stand-in for <linux/input.h> (Mac tests only; values of the
 * codes match input-event-codes.h, the ioctl numbers are mock values). */
#ifndef MOCK_LINUX_INPUT_H
#define MOCK_LINUX_INPUT_H
#include <stdint.h>
#include <sys/time.h>
struct input_event { struct timeval time; uint16_t type, code; int32_t value; };
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_MSC 0x04
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_Q 16
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_C 46
#define KEY_F9 67
#define KEY_MAX 0x2ff
#define EVIOCGNAME(len) (0x10000ul | (unsigned long)(len))
#define EVIOCGKEY(len) (0x20000ul | (unsigned long)(len))
#define EVIOCGRAB 0x30000ul
#endif
