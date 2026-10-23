// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* GLES2 2D drawing shared by every screen (from poc/pomera-scope src/gl.c).
 * Batches quads/polygons into one client-side vertex array and flushes on a
 * program / texture / blend / target / scissor change. GL state belongs to
 * rmp_gfx: after issuing raw GL yourself, call rmp_gfx_invalidate_state().
 *
 * Pick the painter by what is drawn:
 *  - rmp_gfx_painter(): moving parts on the display. Shapes with radius
 *    <= 1.5 use the precomputed rounded-corner mask (no per-fragment
 *    distance); larger radii use the analytic SDF. Text of size 9/10 comes
 *    from the font atlas (one draw for a whole line).
 *  - rmp_gfx_static_painter(): building a static layer (once). Every shape
 *    uses the analytic SDF and text is painted dot by dot: the PoC look.
 * Opaque parts should not blend: fill_rects and layer pastes do not. */
#ifndef RMP_GFX_H
#define RMP_GFX_H

#include "rmp_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

const rmp_painter *rmp_gfx_painter(void);
const rmp_painter *rmp_gfx_static_painter(void);
/* Issues pending vertices. */
void rmp_gfx_flush(void);
/* Forget cached GL state (program, texture, blend, vertex arrays) after
 * raw GL calls, and rebind the display framebuffer and viewport (the
 * scissor is left as it is). Call rmp_gfx_flush() before the raw GL. */
void rmp_gfx_invalidate_state(void);
/* The same, but keep drawing into the current target: a layer between
 * rmp_layer_begin/rmp_layer_resume and rmp_layer_end, or the display. Its
 * framebuffer and viewport are bound again (raw GL must not have bound
 * another one); the scissor is left as it is. Use it after raw GL (e.g.
 * text) drawn into a layer that is still being built. */
void rmp_gfx_invalidate_state_keep_target(void);

/* Off-screen layer: an RGBA texture with its own framebuffer, same pixel
 * convention as the display (top-left origin). */
typedef struct rmp_layer rmp_layer;
rmp_layer *rmp_layer_create(int w, int h);
void rmp_layer_destroy(rmp_layer *layer);
/* Draw into the layer (cleared to clear_rgb, opaque). Do this before the
 * first rmp_frame_clip of the frame. */
void rmp_layer_begin(rmp_layer *layer, unsigned clear_rgb);
/* Draw into the layer again WITHOUT clearing it (scissor off), e.g. after
 * rmp_layer_end or after raw GL that bound the display. Only for a layer
 * that rmp_layer_begin has cleared before (lima needs the first use
 * cleared). Same rule as rmp_layer_begin: before the first rmp_frame_clip
 * of the frame. Costs a reload of the layer's pixels on the tiler; staying
 * in the layer (rmp_gfx_invalidate_state_keep_target) avoids it. */
void rmp_layer_resume(rmp_layer *layer);
/* Back to the display framebuffer; call rmp_frame_clip before drawing. */
void rmp_layer_end(void);
/* Opaque copy of layer pixels `src` to the display at (x, y). */
void rmp_layer_paste(const rmp_layer *layer, rmp_rect src, int x, int y);

/* Internal to the platform (rmp_platform.c). */
int  rmp_gfx_init(int display_w, int display_h);
/* The surface is out_w x out_h. Different from the display size: draw into
 * a display-sized texture and show it scaled to fit (aspect kept, centred,
 * black bars) at every rmp_gfx_frame_finish. 0 = same size (no texture),
 * 1 = scaling, -1 = error. May be called again when the surface changes
 * (an X window resized); the caller then repaints everything. */
int  rmp_gfx_set_output(int out_w, int out_h);
/* A new display size (rmp_set_display_size). Drops the scaled texture: call
 * rmp_gfx_set_output again. sharp: a whole-number scale is sampled with
 * GL_NEAREST (the default display keeps GL_LINEAR). 0, or -1 on error. */
int  rmp_gfx_set_display(int display_w, int display_h, int sharp);
void rmp_gfx_shutdown(void);
void rmp_gfx_frame_start(int clear_on_first_bind, unsigned clear_rgb);
void rmp_gfx_clip(const rmp_rect *r);
void rmp_gfx_frame_finish(void);
int  rmp_gfx_check_error(const char *where);

#ifdef __cplusplus
}
#endif
#endif
