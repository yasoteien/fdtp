// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Deterministic lifecycle tests of rmp_kms.c (ported from the PoC's
 * tests/kms_mock/test_kms.c; the CPU dumb-buffer cases are gone with the
 * CPU mode). The mocks do not validate the Linux DRM ABI or the hardware.
 * Build: see Makefile (-Imock/include). */
#define open mock_open
#define close mock_close
#define poll mock_poll
#define clock_gettime mock_clock_gettime
#include "../rmp_kms.c"
#include <assert.h>
#include <stdarg.h>

struct gbm_device { int unused; };
struct gbm_bo { struct Fb *fb; void (*destroy)(struct gbm_bo *,void *); bool locked; uint32_t format; };
struct gbm_surface { struct gbm_bo bo[3]; int count; };
static int calls, fail_at, fd_count, fb_count, dumb_count, map_count, gbm_count;
static int set_count, release_count, restore_connector_count, fail_restore;
static int poll_interrupts, poll_timeout, surface_fails, releases_before_event;
static uint32_t active_fb, queued_fb, next_fb;
/* Vblank model: a non-blocking poll sees a flip event only after the test
 * advances vblanks; a blocking poll waits for the next vblank. */
static int vblanks, blocking_polls, surface_bos, miss_next, restore_after_flip;
static unsigned vblank_seq;
static void *queued_data;
static int64_t clock_ms;
/* drmWaitVBlank: last request, and a CRTC list where ours is not first. */
static unsigned wait_type, wait_seq, wait_calls;
static int wait_fails, crtc_index;

static bool failure(void) { ++calls; if (calls != fail_at) return false; errno=EIO; return true; }
static void reset(void)
{
    assert(fd_count==0 && fb_count==0 && dumb_count==0 && map_count==0 && gbm_count==0);
    calls=0; fail_at=0; set_count=0; release_count=0; fail_restore=0;
    poll_interrupts=0; poll_timeout=0; surface_fails=0; releases_before_event=0;
    active_fb=77; queued_fb=0; queued_data=NULL; next_fb=100; clock_ms=0;
    vblanks=0; blocking_polls=0; surface_bos=3; miss_next=0; restore_after_flip=0; vblank_seq=1000;
    restore_connector_count=0;
    wait_type=wait_seq=wait_calls=0;wait_fails=0;crtc_index=0;
}
static const char *expected_card="/dev/dri/card0";
int mock_open(const char *path,int flags,...)
{
    assert(!strcmp(path,expected_card)); (void)flags;
    if(failure()) return -1;
    ++fd_count; return 42;
}
int mock_close(int fd) { assert(fd==42); --fd_count; return 0; }
int mock_clock_gettime(clockid_t id,struct timespec *t)
{
    (void)id; clock_ms+=poll_timeout ? 1600 : 1;
    t->tv_sec=clock_ms/1000; t->tv_nsec=(clock_ms%1000)*1000000; return 0;
}
int mock_poll(struct pollfd *p,nfds_t n,int timeout)
{
    (void)n;
    if(poll_interrupts>0) { --poll_interrupts; errno=EINTR; return -1; }
    if(timeout!=0) ++blocking_polls;
    if(poll_timeout||!queued_data) return 0;
    if(timeout==0) { if(vblanks<=0) return 0; --vblanks; }
    p->revents=POLLIN; return 1;
}
drmModeRes *drmModeGetResources(int fd)
{
    (void)fd;if(failure())return NULL;
    drmModeRes *r=calloc(1,sizeof(*r));
    static uint32_t connectors[]={10,11},crtcs[]={20},crtcs2[]={18,20},crtcs3[]={17,18,20};
    r->count_connectors=2;r->connectors=connectors;
    r->count_crtcs=crtc_index+1;r->crtcs=crtc_index==0?crtcs:crtc_index==1?crtcs2:crtcs3;return r;
}
void drmModeFreeResources(drmModeRes *r) { free(r); }
drmModeConnector *drmModeGetConnector(int fd,uint32_t id)
{
    (void)fd;if(failure())return NULL;
    drmModeConnector *c=calloc(1,sizeof(*c));
    static drmModeModeInfo modes[]={{640,480,0,0,0,0},{1024,600,DRM_MODE_TYPE_PREFERRED,51200,1344,635}};
    static uint32_t enc[]={30};
    c->connector_id=id;c->encoder_id=30;c->connection=DRM_MODE_CONNECTED;
    c->count_modes=2;c->modes=modes;c->count_encoders=1;c->encoders=enc;return c;
}
void drmModeFreeConnector(drmModeConnector *c) { free(c); }
drmModeEncoder *drmModeGetEncoder(int fd,uint32_t id)
{
    (void)fd;(void)id;if(failure())return NULL;
    drmModeEncoder *e=calloc(1,sizeof(*e));e->crtc_id=20;e->possible_crtcs=1;return e;
}
void drmModeFreeEncoder(drmModeEncoder *e) { free(e); }
drmModeCrtc *drmModeGetCrtc(int fd,uint32_t id)
{
    (void)fd;if(failure())return NULL;
    drmModeCrtc *c=calloc(1,sizeof(*c));c->crtc_id=id;c->buffer_id=77;c->mode_valid=1;return c;
}
void drmModeFreeCrtc(drmModeCrtc *c) { free(c); }
int drmIoctl(int fd,unsigned long req,void *arg)
{
    (void)fd;
    if(req==DRM_IOCTL_MODE_DESTROY_DUMB) { --dumb_count;return 0; }
    if(failure())return -1;
    if(req==DRM_IOCTL_MODE_CREATE_DUMB) {
        struct drm_mode_create_dumb *c=arg;
        c->handle=(uint32_t)++dumb_count;c->pitch=c->width*4+16;c->size=(uint64_t)c->pitch*c->height;
    } else ((struct drm_mode_map_dumb *)arg)->offset=0;
    return 0;
}
int drmModeAddFB2(int fd,uint32_t w,uint32_t h,uint32_t format,const uint32_t handles[4],const uint32_t stride[4],const uint32_t offset[4],uint32_t *id,uint32_t flags)
{
    (void)fd;(void)w;(void)h;(void)format;(void)handles;(void)stride;(void)offset;(void)flags;
    if(failure())return -1;
    *id=++next_fb;++fb_count;return 0;
}
int drmModeRmFB(int fd,uint32_t id) { (void)fd; assert(id!=active_fb);--fb_count;return 0; }
int drmModeSetCrtc(int fd,uint32_t crtc,uint32_t fb,uint32_t x,uint32_t y,uint32_t *connectors,int count,drmModeModeInfo *mode)
{
    (void)fd;(void)crtc;(void)x;(void)y;(void)connectors;(void)mode;
    ++set_count;
    if(fail_restore && (fb==77 || fail_restore==2)) { errno=EIO;return -1; }
    if(failure())return -1;
    if(fb==77)restore_connector_count=count;
    /* Exit must not retire a queued flip by modeset: wait for its event. */
    if(fb==77&&restore_after_flip)assert(!queued_data);
    active_fb=fb;queued_fb=0;queued_data=NULL;return 0;
}
int drmModePageFlip(int fd,uint32_t crtc,uint32_t fb,uint32_t flags,void *data)
{
    (void)fd;(void)crtc;(void)flags;if(failure())return -1;
    queued_fb=fb;queued_data=data;return 0;
}
int drmHandleEvent(int fd,drmEventContext *ev)
{
    assert(queued_data);active_fb=queued_fb;queued_fb=0;
    vblank_seq+=1u+(unsigned)miss_next;miss_next=0;
    unsigned usec=vblank_seq*16667u;
    void *data=queued_data;queued_data=NULL;ev->page_flip_handler(fd,vblank_seq,usec/1000000u,usec%1000000u,data);return 0;
}
int drmWaitVBlank(int fd,drmVBlank *v)
{
    assert(fd==42);++wait_calls;wait_type=(unsigned)v->request.type;wait_seq=v->request.sequence;
    if(wait_fails) { errno=EINVAL;return -1; }
    v->reply.sequence=wait_seq;return 0;
}
struct gbm_device *gbm_create_device(int fd)
{
    (void)fd;if(failure())return NULL;
    ++gbm_count;return calloc(1,sizeof(struct gbm_device));
}
void gbm_device_destroy(struct gbm_device *g) { --gbm_count;free(g); }
struct gbm_surface *gbm_surface_create(struct gbm_device *g,uint32_t w,uint32_t h,uint32_t format,uint32_t flags)
{
    (void)g;(void)w;(void)h;assert(flags & GBM_BO_USE_LINEAR);
    if(surface_fails>0) { --surface_fails;errno=EINVAL;return NULL; }
    if(failure())return NULL;
    struct gbm_surface *s=calloc(1,sizeof(*s));
    s->count=surface_bos;
    for(int i=0;i<3;++i)s->bo[i].format=format;
    return s;
}
void gbm_surface_destroy(struct gbm_surface *s)
{
    for(int i=0;i<3;++i) { assert(!s->bo[i].locked);if(s->bo[i].destroy)s->bo[i].destroy(&s->bo[i],s->bo[i].fb); }
    free(s);
}
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s)
{
    if(failure())return NULL;
    for(int i=0;i<s->count;++i)if(!s->bo[i].locked) { s->bo[i].locked=true;return &s->bo[i]; }
    assert(0);return NULL;
}
int gbm_surface_has_free_buffers(struct gbm_surface *s)
{
    int n=0;for(int i=0;i<s->count;++i)n+=!s->bo[i].locked;return n;
}
void gbm_surface_release_buffer(struct gbm_surface *s,struct gbm_bo *b)
{
    (void)s;assert(b->locked);
    if(b->fb && (b->fb->id==active_fb || b->fb->id==queued_fb))++releases_before_event;
    assert(releases_before_event==0);b->locked=false;++release_count;
}
void *gbm_bo_get_user_data(struct gbm_bo *b) { return b->fb; }
void gbm_bo_set_user_data(struct gbm_bo *b,void *d,void (*f)(struct gbm_bo *,void *)) { b->fb=d;b->destroy=f; }
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *b) { (void)b;union gbm_bo_handle h={.u32=1};return h; }
uint32_t gbm_bo_get_stride(struct gbm_bo *b) { (void)b;return 4096; }
uint32_t gbm_bo_get_width(struct gbm_bo *b) { (void)b;return 1024; }
uint32_t gbm_bo_get_height(struct gbm_bo *b) { (void)b;return 600; }
uint32_t gbm_bo_get_format(struct gbm_bo *b) { return b->format; }

int main(void)
{
    RmpKms *k;
    /* GL: present queues without waiting; rendering continues while queued. */
    reset();k=rmp_kms_open(NULL);assert(k);
    assert(!rmp_kms_wait_free_buffer(k));assert(!rmp_kms_present_gl(k));     /* modeset, front locked */
    assert(!rmp_kms_wait_free_buffer(k));assert(!rmp_kms_present_gl(k));     /* flip queued */
    assert(rmp_kms_flip_pending(k) && release_count==0 && blocking_polls==0);
    assert(rmp_kms_has_free_buffer(k));
    assert(!rmp_kms_wait_free_buffer(k));                                 /* 3rd buffer free: no wait */
    assert(rmp_kms_flip_pending(k) && blocking_polls==0);
    assert(!rmp_kms_present_gl(k));                                       /* waits for the previous flip only */
    assert(blocking_polls==1 && release_count==1 && rmp_kms_flip_count(k)==1 && rmp_kms_flip_pending(k));
    assert(fb_count==3);
    /* Vblank timestamps and a missed vblank (sequence gap of 2). */
    miss_next=1;vblanks=1;assert(!rmp_kms_dispatch(k) && rmp_kms_flip_count(k)==2 && release_count==2);
    double t0,t1;unsigned s0,s1;
    assert(!rmp_kms_flip_record(k,0,&t0,&s0) && !rmp_kms_flip_record(k,1,&t1,&s1));
    assert(s1-s0==2 && t1>t0 && rmp_kms_flip_record(k,2,NULL,NULL)<0);
    assert(rmp_kms_refresh_period(k)>0);
    rmp_kms_close(k);

    /* No free GBM buffer: the only blocking point besides the next present. */
    reset();surface_bos=2;k=rmp_kms_open(NULL);assert(k);
    assert(!rmp_kms_present_gl(k));assert(!rmp_kms_present_gl(k));assert(rmp_kms_flip_pending(k));
    blocking_polls=0;assert(!rmp_kms_wait_free_buffer(k));
    assert(blocking_polls==1 && !rmp_kms_flip_pending(k) && release_count==1);
    rmp_kms_close(k);

    /* Exit waits for the queued flip before restoring the CRTC (GL). */
    reset();k=rmp_kms_open(NULL);assert(k);assert(!rmp_kms_present_gl(k));assert(!rmp_kms_present_gl(k));
    assert(rmp_kms_flip_pending(k));restore_after_flip=1;assert(!rmp_kms_restore(k));
    assert(rmp_kms_flip_count(k)==1 && active_fb==77);rmp_kms_close(k);

    reset();surface_fails=1;k=rmp_kms_open(NULL);assert(k && rmp_kms_format(k)==DRM_FORMAT_ARGB8888);rmp_kms_close(k);
    reset();surface_fails=2;assert(!rmp_kms_open(NULL));

    /* A flip that never completes: the next present fails, the queued buffer
     * stays locked, the new one is released; close restores the CRTC. */
    reset();k=rmp_kms_open(NULL);assert(k);assert(!rmp_kms_present_gl(k));assert(!rmp_kms_present_gl(k));
    poll_timeout=1;
    assert(rmp_kms_present_gl(k)<0 && k->pending && k->next && release_count==1);
    rmp_kms_close(k);assert(active_fb==77);

    reset();k=rmp_kms_open(NULL);assert(k);assert(!rmp_kms_present_gl(k));fail_restore=1;
    assert(!rmp_kms_restore(k));assert(active_fb==0);rmp_kms_close(k);
    reset();k=rmp_kms_open(NULL);assert(k);assert(!rmp_kms_present_gl(k));fail_restore=2;
    assert(rmp_kms_restore(k)<0);rmp_kms_close(k);assert(fd_count==1 && gbm_count==1);
    fail_restore=0;rmp_kms_close(k); /* Test can recover retained objects. */

    /* Inject one failure at every fallible API along open + three presents.
     * Some selection failures are recoverable through the second connector. */
    for(int step=1;step<40;++step) {
        reset();fail_at=step;k=rmp_kms_open(NULL);
        if(k) {
            for(int i=0;i<3;++i) if(!rmp_kms_wait_free_buffer(k))rmp_kms_present_gl(k);
            fail_at=0;rmp_kms_close(k);
        }
    }
    /* drmWaitVBlank: absolute sequence, CRTC index 0 / 1 (SECONDARY) / 2 (high bits). */
    for(int idx=0;idx<3;++idx) {
        reset();crtc_index=idx;k=rmp_kms_open(NULL);assert(k);
        assert(rmp_kms_wait_vblank(k,4294967295u)==0 && wait_calls==1 && wait_seq==4294967295u);
        unsigned want=idx==0?0u:idx==1?0x20000000u:(2u<<1);
        assert(wait_type==want);
        wait_fails=1;assert(rmp_kms_wait_vblank(k,5)<0 && errno==EINVAL);
        rmp_kms_close(k);
    }
    assert(rmp_kms_wait_vblank(NULL,1)<0);
    /* Another card path; fd accessor. */
    reset();expected_card="/dev/dri/card1";k=rmp_kms_open("/dev/dri/card1");assert(k && rmp_kms_fd(k)==42);
    rmp_kms_close(k);expected_card="/dev/dri/card0";assert(rmp_kms_fd(NULL)==-1);
    reset();puts("PASS kms mock lifecycle: non-blocking flips (draw while queued, wait only on the previous flip or no free buffer), vblank records and missed vblank, exit waits for the queued flip, restore failure paths, drmWaitVBlank absolute with the CRTC index bits (0, SECONDARY, high), one injected failure at each of 39 steps leaks nothing, card path");return 0;
}
