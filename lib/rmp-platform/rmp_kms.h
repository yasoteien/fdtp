// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* DRM/KMS + GBM scanout (internal to the platform).
 * From poc/pomera-scope src/kms.c; the CPU (dumb buffer) mode is removed.
 *
 * All calls belong on the display thread. Presenting never waits for the
 * flip it queues: it waits only while the PREVIOUS flip is still pending.
 * Flips are vsync-aligned (DRM_MODE_PAGE_FLIP_EVENT, never ASYNC); the old
 * front buffer is released when the completion event is handled.
 * Frame: rmp_kms_wait_free_buffer() -> draw -> eglSwapBuffers() ->
 * rmp_kms_present_gl(). Restore scanout with rmp_kms_restore() (waits for a
 * pending flip first), then destroy EGL, then rmp_kms_close(). */
#ifndef RMP_KMS_H
#define RMP_KMS_H

#include <stdbool.h>
#include <stdint.h>

struct gbm_device;
struct gbm_surface;
typedef struct RmpKms RmpKms;

/* card: NULL = "/dev/dri/card0". */
RmpKms *rmp_kms_open(const char *card);
int rmp_kms_fd(const RmpKms *kms);
int rmp_kms_width(const RmpKms *kms);
int rmp_kms_height(const RmpKms *kms);
struct gbm_device *rmp_kms_gbm_device(RmpKms *kms);
struct gbm_surface *rmp_kms_gbm_surface(RmpKms *kms);
uint32_t rmp_kms_format(const RmpKms *kms);
int rmp_kms_present_gl(RmpKms *kms);
/* Handle completed flips without blocking. */
int rmp_kms_dispatch(RmpKms *kms);
/* 1 when GBM has a buffer that is neither shown nor queued. */
int rmp_kms_has_free_buffer(RmpKms *kms);
/* Block until GBM has a buffer that is neither shown nor queued. */
int rmp_kms_wait_free_buffer(RmpKms *kms);
bool rmp_kms_flip_pending(const RmpKms *kms);
/* Blocks until the CRTC's vblank counter reaches `sequence` (the counter the
 * flip records use); returns at once when it already has. A page flip
 * queued after this returns cannot latch before vblank sequence + 1.
 * 0 on success, -1 (errno set) when the driver cannot wait for vblanks. */
int rmp_kms_wait_vblank(RmpKms *kms, unsigned sequence);
/* Nominal frame period of the mode, seconds. */
double rmp_kms_refresh_period(const RmpKms *kms);
/* Completed flips (vblank time in CLOCK_MONOTONIC seconds, vblank sequence).
 * Records older than the last 64 return -1. */
unsigned long rmp_kms_flip_count(const RmpKms *kms);
int rmp_kms_flip_record(const RmpKms *kms, unsigned long index, double *time, unsigned *sequence);
/* 0: original CRTC restored (or safely disabled after restore failed).
 * -1: scanout may still reference buffers; keep EGL/GBM alive until exit. */
int rmp_kms_restore(RmpKms *kms);
void rmp_kms_close(RmpKms *kms);

#endif
