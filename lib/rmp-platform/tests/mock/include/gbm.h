// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef MOCK_GBM_H
#define MOCK_GBM_H
#include <stdint.h>
#define GBM_BO_USE_SCANOUT 1
#define GBM_BO_USE_RENDERING 4
#define GBM_BO_USE_LINEAR 16
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
union gbm_bo_handle { void *ptr; int32_t s32; uint32_t u32; uint64_t u64; };
struct gbm_device *gbm_create_device(int);
void gbm_device_destroy(struct gbm_device *);
struct gbm_surface *gbm_surface_create(struct gbm_device *,uint32_t,uint32_t,uint32_t,uint32_t);
void gbm_surface_destroy(struct gbm_surface *);
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *);
void gbm_surface_release_buffer(struct gbm_surface *,struct gbm_bo *);
int gbm_surface_has_free_buffers(struct gbm_surface *);
void *gbm_bo_get_user_data(struct gbm_bo *);
void gbm_bo_set_user_data(struct gbm_bo *,void *,void (*)(struct gbm_bo *,void *));
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *);
uint32_t gbm_bo_get_stride(struct gbm_bo *);
uint32_t gbm_bo_get_width(struct gbm_bo *);
uint32_t gbm_bo_get_height(struct gbm_bo *);
uint32_t gbm_bo_get_format(struct gbm_bo *);
#endif
