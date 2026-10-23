// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "rmp_kms.h"

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#define FLIP_TIMEOUT_MS 3000
#define FLIP_RECORDS 64

struct Fb {
    int fd;
    uint32_t id;
};

struct RmpKms {
    int fd;
    bool changed, pending;
    uint32_t connector, crtc, format;
    uint32_t vblank_crtc;          /* drmWaitVBlank type bits of the CRTC's index */
    drmModeModeInfo mode;
    drmModeCrtc *original;
    uint32_t *original_connectors;
    int original_connector_count;
    struct gbm_device *gbm;
    struct gbm_surface *surface;
    /* front: on screen. next: queued by a pending pageflip (becomes front
     * when the flip event arrives; the old front is released then). */
    struct gbm_bo *front, *next;
    /* Completed flips: vblank timestamps (s) and sequence numbers. */
    unsigned long flips;
    double flip_time[FLIP_RECORDS];
    unsigned flip_sequence[FLIP_RECORDS];
};

static void report(const char *operation)
{
    fprintf(stderr, "kms: %s: %s\n", operation, strerror(errno));
}

static int choose_crtc(int fd, const drmModeRes *res,
                       const drmModeConnector *connector, uint32_t *id)
{
    /* Prefer the connector's current routing to avoid touching another CRTC. */
    if (connector->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, connector->encoder_id);
        if (enc) {
            if (enc->crtc_id) {
                *id = enc->crtc_id;
                drmModeFreeEncoder(enc);
                return 0;
            }
            drmModeFreeEncoder(enc);
        }
    }
    for (int e = 0; e < connector->count_encoders; ++e) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, connector->encoders[e]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs && c < 32; ++c) {
            if (enc->possible_crtcs & (1u << c)) {
                *id = res->crtcs[c];
                drmModeFreeEncoder(enc);
                return 0;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return -1;
}

static int choose_output(RmpKms *k)
{
    drmModeRes *res = drmModeGetResources(k->fd);
    if (!res) return -1;
    bool found = false;
    for (int i = 0; i < res->count_connectors && !found; ++i) {
        drmModeConnector *conn = drmModeGetConnector(k->fd, res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
            choose_crtc(k->fd, res, conn, &k->crtc) == 0) {
            int selected = 0;
            for (int m = 0; m < conn->count_modes; ++m) {
                if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                    selected = m;
                    break;
                }
            }
            k->connector = conn->connector_id;
            k->mode = conn->modes[selected];
            found = true;
        }
        drmModeFreeConnector(conn);
    }
    if (!found) {
        errno = ENODEV;
        drmModeFreeResources(res);
        return -1;
    }
    /* drmWaitVBlank names the CRTC by its index in the resources. */
    for (int c = 0; c < res->count_crtcs; ++c) {
        if (res->crtcs[c] != k->crtc) continue;
        k->vblank_crtc = c == 0 ? 0u
                       : c == 1 ? (uint32_t)DRM_VBLANK_SECONDARY
                       : ((uint32_t)c << DRM_VBLANK_HIGH_CRTC_SHIFT) & (uint32_t)DRM_VBLANK_HIGH_CRTC_MASK;
        break;
    }
    k->original = drmModeGetCrtc(k->fd, k->crtc);
    if (!k->original) {
        drmModeFreeResources(res);
        return -1;
    }
    /* Save every connector routed to this CRTC, including clones. Connection
     * status alone is insufficient: it does not describe the original route. */
    k->original_connectors = calloc((size_t)res->count_connectors,
                                    sizeof(*k->original_connectors));
    if (!k->original_connectors && res->count_connectors) {
        drmModeFreeResources(res);
        return -1;
    }
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *conn = drmModeGetConnector(k->fd, res->connectors[i]);
        if (!conn) {
            drmModeFreeResources(res);
            return -1;
        }
        if (conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(k->fd, conn->encoder_id);
            if (!enc) {
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                return -1;
            }
            if (enc->crtc_id == k->crtc)
                k->original_connectors[k->original_connector_count++] = conn->connector_id;
            drmModeFreeEncoder(enc);
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return 0;
}

static void fb_destroy(struct gbm_bo *bo, void *data)
{
    (void)bo;
    struct Fb *fb = data;
    if (fb) {
        if (fb->fd >= 0 && fb->id) drmModeRmFB(fb->fd, fb->id);
        free(fb);
    }
}

static uint32_t bo_fb(RmpKms *k, struct gbm_bo *bo)
{
    struct Fb *fb = gbm_bo_get_user_data(bo);
    if (fb) return fb->id;
    fb = calloc(1, sizeof(*fb));
    if (!fb) return 0;
    fb->fd = k->fd;
    uint32_t handles[4] = {gbm_bo_get_handle(bo).u32, 0, 0, 0};
    uint32_t strides[4] = {gbm_bo_get_stride(bo), 0, 0, 0};
    uint32_t offsets[4] = {0};
    if (drmModeAddFB2(k->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                      gbm_bo_get_format(bo), handles, strides, offsets,
                      &fb->id, 0) < 0) {
        free(fb);
        return 0;
    }
    gbm_bo_set_user_data(bo, fb, fb_destroy);
    return fb->id;
}

static void flip_done(int fd, unsigned sequence, unsigned sec,
                      unsigned usec, void *data)
{
    (void)fd;
    RmpKms *k = data;
    if (!k->pending) return;
    k->pending = false;
    /* The previous front left scanout at this vblank: now reusable. */
    if (k->front) gbm_surface_release_buffer(k->surface, k->front);
    k->front = k->next;
    k->next = NULL;
    unsigned slot = (unsigned)(k->flips % FLIP_RECORDS);
    k->flip_time[slot] = (double)sec + (double)usec / 1e6;
    k->flip_sequence[slot] = sequence;
    k->flips++;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Handle pending DRM events without blocking. */
static int dispatch(RmpKms *k)
{
    drmEventContext ev = {0};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = flip_done;
    while (k->pending) {
        struct pollfd pfd = {k->fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 0);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) return -1;
        if (ret == 0) return 0;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
        if (!(pfd.revents & POLLIN)) return 0;
        if (drmHandleEvent(k->fd, &ev) != 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
    }
    return 0;
}

static int wait_flip(RmpKms *k)
{
    drmEventContext ev = {0};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = flip_done;
    int64_t start = monotonic_ms();
    if (start < 0) return -1;
    while (k->pending) {
        int64_t now = monotonic_ms();
        if (now < 0) return -1;
        int64_t remaining = FLIP_TIMEOUT_MS - (now - start);
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = {k->fd, POLLIN, 0};
        int ret = poll(&pfd, 1, (int)remaining);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) return -1;
        if (ret == 0) { errno = ETIMEDOUT; return -1; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        if ((pfd.revents & POLLIN) && drmHandleEvent(k->fd, &ev) != 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
    }
    return 0;
}

/* Returns 1 after a modeset (fb is on screen now), 0 after queueing a
 * vsync-aligned pageflip (no ASYNC flag, no tearing), -1 on error. Only a
 * still-pending previous flip makes this wait. */
static int present(RmpKms *k, uint32_t fb)
{
    if (!k->changed) {
        if (drmModeSetCrtc(k->fd, k->crtc, fb, 0, 0,
                           &k->connector, 1, &k->mode) != 0) return -1;
        k->changed = true;
        return 1;
    }
    /* SIGINT/SIGTERM only set the main-loop flag. Keep queued buffers alive
     * until completion even when poll is interrupted; close handles failure. */
    if (k->pending && wait_flip(k) != 0) return -1;
    if (drmModePageFlip(k->fd, k->crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, k) != 0)
        return -1;
    k->pending = true;
    return 0;
}

RmpKms *rmp_kms_open(const char *card)
{
    RmpKms *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->fd = -1;
    k->format = DRM_FORMAT_XRGB8888;
    k->fd = open(card ? card : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (k->fd < 0) goto fail;
    if (choose_output(k) < 0) goto fail;
    /* rockchip card0 is the display device; Mesa kmsro routes rendering to
     * lima. GBM and EGL must both use this device, not renderD128 directly.
     * Linear, single-plane 32-bit RGB scanout is assumed. Surface creation
     * is the format fallback point; AddFB2/SetCrtc verify actual scanout
     * support when the first frame is presented. */
    k->gbm = gbm_create_device(k->fd);
    if (!k->gbm) goto fail;
    k->surface = gbm_surface_create(k->gbm, k->mode.hdisplay, k->mode.vdisplay,
                                     k->format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!k->surface) {
        k->format = DRM_FORMAT_ARGB8888;
        k->surface = gbm_surface_create(k->gbm, k->mode.hdisplay, k->mode.vdisplay,
                                         k->format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    }
    if (!k->surface) goto fail;
    fprintf(stderr, "kms: %s connector %u CRTC %u %ux%u %s\n", card ? card : "card0",
            k->connector, k->crtc, k->mode.hdisplay, k->mode.vdisplay,
            k->format == DRM_FORMAT_XRGB8888 ? "GBM XRGB8888" : "GBM ARGB8888");
    return k;
fail:
    report("initialization");
    rmp_kms_close(k);
    return NULL;
}

int rmp_kms_fd(const RmpKms *k) { return k ? k->fd : -1; }
int rmp_kms_width(const RmpKms *k) { return k ? k->mode.hdisplay : 0; }
int rmp_kms_height(const RmpKms *k) { return k ? k->mode.vdisplay : 0; }
struct gbm_device *rmp_kms_gbm_device(RmpKms *k) { return k ? k->gbm : NULL; }
struct gbm_surface *rmp_kms_gbm_surface(RmpKms *k) { return k ? k->surface : NULL; }
uint32_t rmp_kms_format(const RmpKms *k) { return k ? k->format : 0; }

double rmp_kms_refresh_period(const RmpKms *k)
{
    if (!k || !k->mode.clock || !k->mode.htotal || !k->mode.vtotal) return 1.0 / 60;
    return (double)k->mode.htotal * k->mode.vtotal / ((double)k->mode.clock * 1000.0);
}

unsigned long rmp_kms_flip_count(const RmpKms *k) { return k ? k->flips : 0; }

int rmp_kms_flip_record(const RmpKms *k, unsigned long index, double *time, unsigned *sequence)
{
    if (!k || index >= k->flips || k->flips - index > FLIP_RECORDS) return -1;
    unsigned slot = (unsigned)(index % FLIP_RECORDS);
    if (time) *time = k->flip_time[slot];
    if (sequence) *sequence = k->flip_sequence[slot];
    return 0;
}

bool rmp_kms_flip_pending(const RmpKms *k) { return k && k->pending; }

int rmp_kms_dispatch(RmpKms *k)
{
    if (!k) { errno = EINVAL; return -1; }
    if (dispatch(k) != 0) { report("pageflip event"); return -1; }
    return 0;
}

int rmp_kms_wait_vblank(RmpKms *k, unsigned sequence)
{
    if (!k) { errno = EINVAL; return -1; }
    drmVBlank vbl;
    memset(&vbl, 0, sizeof vbl);
    /* Absolute: the kernel returns at once when the counter is already past
     * (it widens the 32-bit value around its current count). libdrm retries
     * EINTR; the kernel gives up after 3 s (CRTC off). */
    vbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_ABSOLUTE | k->vblank_crtc);
    vbl.request.sequence = sequence;
    return drmWaitVBlank(k->fd, &vbl) == 0 ? 0 : -1;
}

int rmp_kms_has_free_buffer(RmpKms *k)
{
    return k && k->surface && gbm_surface_has_free_buffers(k->surface);
}

int rmp_kms_wait_free_buffer(RmpKms *k)
{
    if (!k) { errno = EINVAL; return -1; }
    if (dispatch(k) != 0) { report("pageflip event"); return -1; }
    /* Block only when every GBM buffer is on screen, queued or in flight. */
    while (!gbm_surface_has_free_buffers(k->surface)) {
        if (!k->pending) { errno = ENOBUFS; report("no free GBM buffer"); return -1; }
        if (wait_flip(k) != 0) { report("wait for pageflip"); return -1; }
    }
    return 0;
}

int rmp_kms_present_gl(RmpKms *k)
{
    if (!k || (k->next && !k->pending)) { errno = EINVAL; return -1; }
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(k->surface);
    if (!bo) { report("lock front buffer"); return -1; }
    uint32_t fb = bo_fb(k, bo);
    int ret = fb ? present(k, fb) : -1;
    if (ret < 0) {
        report("GL present");
        /* Not referenced by the kernel: a failed flip never queues bo. */
        gbm_surface_release_buffer(k->surface, bo);
        return -1;
    }
    if (ret == 1) {
        if (k->front) gbm_surface_release_buffer(k->surface, k->front);
        k->front = bo;
    } else {
        k->next = bo;
    }
    return 0;
}

int rmp_kms_restore(RmpKms *k)
{
    if (!k || !k->changed) return 0;
    /* Restore BEFORE EGL destroys buffers. A successful legacy modeset retires
     * an outstanding pageflip; no more events are dispatched after restore. */
    if (k->pending && wait_flip(k) != 0)
        report("wait for pageflip before restore");
    /* After a failed wait the queued buffer stays locked (k->next) and is
     * only released by rmp_kms_close after the CRTC no longer scans it out. */
    drmModeCrtc *o = k->original;
    int ret = drmModeSetCrtc(k->fd, o->crtc_id, o->buffer_id, o->x, o->y,
                              o->mode_valid ? k->original_connectors : NULL,
                              o->mode_valid ? k->original_connector_count : 0,
                              o->mode_valid ? &o->mode : NULL);
    if (ret != 0) {
        report("restore original CRTC");
        /* The original framebuffer may have disappeared. Disable scanout so
         * freeing our buffers is safe, while explicitly reporting lost state. */
        if (drmModeSetCrtc(k->fd, k->crtc, 0, 0, 0, NULL, 0, NULL) != 0) {
            report("disable CRTC after failed restore");
            return -1;
        }
    }
    k->changed = false;
    k->pending = false;
    return 0;
}

void rmp_kms_close(RmpKms *k)
{
    if (!k) return;
    if (rmp_kms_restore(k) != 0) {
        /* Exceptional driver failure: keep storage alive until process exit
         * rather than unmap buffers still used by scanout. */
        fprintf(stderr, "kms: retaining scanout resources until process exit\n");
        return;
    }
    if (k->surface) {
        if (k->next) gbm_surface_release_buffer(k->surface, k->next);
        if (k->front) gbm_surface_release_buffer(k->surface, k->front);
        gbm_surface_destroy(k->surface);
    }
    if (k->gbm) gbm_device_destroy(k->gbm);
    if (k->original) drmModeFreeCrtc(k->original);
    free(k->original_connectors);
    if (k->fd >= 0) close(k->fd);
    free(k);
}
