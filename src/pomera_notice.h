// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_POMERA_NOTICE_H
#define FDTP_POMERA_NOTICE_H
#include "app.h"
/* Revision 1 §6: when the Pomera's boot check (pomera_mount_internal.sh boot)
 * found the internal memory damaged, an information box at start, once per
 * boot. Reads /var/lib/pomera-internal/check (key=value: state, mount,
 * boot_id, time, fsck_output); the boot_id shown is kept in
 * $HOME/.fdtp/internal-check-seen. Nothing when the file is missing or the
 * state is not damaged. FDTP_POMERA_CHECK and FDTP_POMERA_SEEN replace the
 * two paths (the tests); headless or under FDTP_TEST_KEYS, only with FDTP_POMERA_CHECK.
 */
void pomera_notice_start(App *a, int headless);
#endif
