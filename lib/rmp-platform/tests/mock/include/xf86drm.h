// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef MOCK_DRM_H
#define MOCK_DRM_H
#define DRM_EVENT_CONTEXT_VERSION 4
typedef struct { int version; void (*page_flip_handler)(int,unsigned,unsigned,unsigned,void *); } drmEventContext;
int drmIoctl(int,unsigned long,void *);
int drmHandleEvent(int,drmEventContext *);
/* Same names and values as libdrm's xf86drm.h. */
typedef enum {
    DRM_VBLANK_ABSOLUTE = 0x0,
    DRM_VBLANK_RELATIVE = 0x1,
    DRM_VBLANK_HIGH_CRTC_MASK = 0x0000003e,
    DRM_VBLANK_EVENT = 0x4000000,
    DRM_VBLANK_SECONDARY = 0x20000000
} drmVBlankSeqType;
#define DRM_VBLANK_HIGH_CRTC_SHIFT 1
typedef struct { drmVBlankSeqType type; unsigned int sequence; unsigned long signal; } drmVBlankReq;
typedef struct { drmVBlankSeqType type; unsigned int sequence; long tval_sec; long tval_usec; } drmVBlankReply;
typedef union { drmVBlankReq request; drmVBlankReply reply; } drmVBlank;
int drmWaitVBlank(int,drmVBlank *);
#endif
