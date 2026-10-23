// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdint.h>
#define DRM_IOCTL_MODE_CREATE_DUMB 1ul
#define DRM_IOCTL_MODE_MAP_DUMB 2ul
#define DRM_IOCTL_MODE_DESTROY_DUMB 3ul
struct drm_mode_create_dumb { uint32_t width,height,bpp,handle,pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };
