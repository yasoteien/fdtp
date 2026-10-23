// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef MOCK_DRM_MODE_H
#define MOCK_DRM_MODE_H
#include <stdint.h>
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_TYPE_PREFERRED (1u << 3)
#define DRM_MODE_PAGE_FLIP_EVENT 1
/* Minimal API-shaped mock types, not a substitute for real header ABI checks. */
typedef struct { uint16_t hdisplay,vdisplay; uint32_t type; uint32_t clock; uint16_t htotal,vtotal; } drmModeModeInfo;
typedef struct { int count_connectors,count_crtcs; uint32_t *connectors,*crtcs; } drmModeRes;
typedef struct { uint32_t connector_id,encoder_id; int connection,count_modes,count_encoders; drmModeModeInfo *modes; uint32_t *encoders; } drmModeConnector;
typedef struct { uint32_t crtc_id,possible_crtcs; } drmModeEncoder;
typedef struct { uint32_t crtc_id,buffer_id,x,y; int mode_valid; drmModeModeInfo mode; } drmModeCrtc;
drmModeRes *drmModeGetResources(int);
void drmModeFreeResources(drmModeRes *);
drmModeConnector *drmModeGetConnector(int,uint32_t);
void drmModeFreeConnector(drmModeConnector *);
drmModeEncoder *drmModeGetEncoder(int,uint32_t);
void drmModeFreeEncoder(drmModeEncoder *);
drmModeCrtc *drmModeGetCrtc(int,uint32_t);
void drmModeFreeCrtc(drmModeCrtc *);
int drmModeAddFB2(int,uint32_t,uint32_t,uint32_t,const uint32_t[4],const uint32_t[4],const uint32_t[4],uint32_t *,uint32_t);
int drmModeRmFB(int,uint32_t);
int drmModeSetCrtc(int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *,int,drmModeModeInfo *);
int drmModePageFlip(int,uint32_t,uint32_t,uint32_t,void *);
#endif
