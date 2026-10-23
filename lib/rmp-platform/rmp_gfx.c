// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* GLES2 painter, layers and per-frame target/scissor handling.
 * Ported from poc/pomera-scope src/gl.c. Changes for the app:
 *  - one font atlas (16 phases) and one mask atlas instead of 128 + 49
 *    textures (see rmp_gfx_assets.h): fewer batch breaks;
 *  - vertices come from a client-side array (Mesa streams them through its
 *    upload buffer) instead of glBufferData on one VBO per flush, which
 *    reallocates the buffer each time. -DRMP_GFX_USE_VBO=1 restores the PoC
 *    path for comparison on the device. */
#include "rmp_gfx.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmp_gfx_assets.h"

#ifndef RMP_GFX_USE_VBO
#define RMP_GFX_USE_VBO 0
#endif

enum { SDF, MASK, TEXTURE, FONT, POLY, FLAT, PROGRAMS };
enum { BLEND_OFF, BLEND_OVER };
#define FLOATS 18                 /* per vertex */
#define CAPACITY (6 * 2048)       /* vertices per batch */
#define RECTS_PER_DRAW 1024

struct rmp_layer {
    GLuint texture, fbo;
    int w, h;
};

static struct {
    int ready;
    GLuint programs[PROGRAMS];
    GLint viewports[PROGRAMS];
    GLint flat_color;
    GLuint font, mask, vbo;
    int mode, blending;
    GLuint bound_texture;
    GLuint target;                /* framebuffer: screen_fbo = display */
    int vw, vh;                   /* current target size */
    int display_w, display_h;
    /* Scaled output (rmp_gfx_set_output): the display is this 1024x600
     * texture, drawn to the out_w x out_h surface at `dst` each frame.
     * screen_fbo 0: no scaling, the display is the EGL surface itself. */
    GLuint screen_fbo, screen_tex;
    int out_w, out_h;
    rmp_rect dst;
    int sharp;                    /* rmp_gfx_set_display: nearest at whole scales */
    int need_clear;
    unsigned clear_rgb;
    size_t count;
    GLfloat vertices[CAPACITY * FLOATS];
    GLfloat flat[RECTS_PER_DRAW * 12];
    rmp_painter dynamic_painter, static_painter;
} G;

static void target(GLuint fbo, int w, int h);
static void to_display(void);

static GLuint shader(GLenum kind, const char *source)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gfx: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void flush(void)
{
    if (!G.count) return;
    static const int sizes[6] = { 2, 2, 2, 4, 4, 4 }, offsets[6] = { 0, 2, 4, 6, 10, 14 };
#if RMP_GFX_USE_VBO
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(G.count * FLOATS * sizeof(GLfloat)), G.vertices, GL_STREAM_DRAW);
    for (int i = 0; i < 6; i++)
        glVertexAttribPointer((GLuint)i, sizes[i], GL_FLOAT, GL_FALSE, FLOATS * sizeof(GLfloat),
                              (const void *)((size_t)offsets[i] * sizeof(GLfloat)));
#else
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int i = 0; i < 6; i++)
        glVertexAttribPointer((GLuint)i, sizes[i], GL_FLOAT, GL_FALSE, FLOATS * sizeof(GLfloat),
                              G.vertices + offsets[i]);
#endif
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)G.count);
    G.count = 0;
}

void rmp_gfx_flush(void) { if (G.ready) flush(); }

static void forget_state(void)
{
    flush();
    G.mode = -1;
    G.blending = -1;
    G.bound_texture = (GLuint)-1;
    for (GLuint i = 0; i < 6; i++) glEnableVertexAttribArray(i);
    glActiveTexture(GL_TEXTURE0);
}

void rmp_gfx_invalidate_state(void)
{
    if (!G.ready) return;
    forget_state();
    to_display();
}

void rmp_gfx_invalidate_state_keep_target(void)
{
    if (!G.ready) return;
    forget_state();
    target(G.target, G.vw, G.vh);   /* the same framebuffer again (not a switch) */
}

static void pipeline(int mode, GLuint texture, int blend)
{
    if (G.mode != mode) {
        flush();
        G.mode = mode;
        glUseProgram(G.programs[mode]);
        glUniform2f(G.viewports[mode], (float)G.vw, (float)G.vh);
    }
    if (G.bound_texture != texture) {
        flush();
        glBindTexture(GL_TEXTURE_2D, texture);
        G.bound_texture = texture;
    }
    if (G.blending != blend) {
        flush();
        G.blending = blend;
        if (blend == BLEND_OFF) glDisable(GL_BLEND);
        else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
}

/* Pixel coordinates are top-left origin in the bound target. */
static void target(GLuint fbo, int w, int h)
{
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    G.target = fbo;
    G.vw = w;
    G.vh = h;
    G.mode = -1;
}

static void to_display(void) { target(G.screen_fbo, G.display_w, G.display_h); }

static GLfloat *vertex(void) { return &G.vertices[G.count++ * FLOATS]; }

static void quad(const float xy[8], const rmp_shape *s, float pad)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    if (G.count + 6 > CAPACITY) flush();
    for (int j = 0; j < 6; j++) {
        int k = order[j];
        GLfloat *v = vertex();
        v[0] = xy[k * 2]; v[1] = xy[k * 2 + 1];
        /* Texture quads use local as UV; shape quads use shape-local pixels.
         * Never subtract screen coordinates in the mediump fragment shader. */
        v[2] = s ? uv[k * 2] * (s->w + 2 * pad) - pad : uv[k * 2];
        v[3] = s ? uv[k * 2 + 1] * (s->h + 2 * pad) - pad : uv[k * 2 + 1];
        v[4] = s ? s->w : 1; v[5] = s ? s->h : 1;
        rmp_color top = s ? s->top : (rmp_color){ 1, 1, 1, 1 };
        rmp_color bottom = s ? s->bottom : top;
        v[6] = top.r; v[7] = top.g; v[8] = top.b; v[9] = top.a;
        v[10] = bottom.r; v[11] = bottom.g; v[12] = bottom.b; v[13] = bottom.a;
        v[14] = s ? s->radius : 0; v[15] = 0; v[16] = 0; v[17] = 0;
        if (s && G.mode == MASK) {
            unsigned index = (unsigned)lroundf(s->radius * 32), ox, oy;
            rmp_mask_tile_origin(index, &ox, &oy);
            v[14] = (float)index / 32;
            v[15] = 1 / s->h;
            v[16] = (float)ox / RMP_MASK_ATLAS;
            v[17] = (float)oy / RMP_MASK_ATLAS;
        }
    }
}

static void shape_quad(const rmp_shape *s, float pad)
{
    float x = s->x - pad, y = s->y - pad, w = s->w + 2 * pad, h = s->h + 2 * pad;
    float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    quad(xy, s, pad);
}

static void static_shape(void *ctx, const rmp_shape *s)
{
    (void)ctx;
    pipeline(SDF, G.bound_texture, BLEND_OVER);
    shape_quad(s, 1);
}

static void dynamic_shape(void *ctx, const rmp_shape *s)
{
    (void)ctx;
    /* Precomputed rounded fill mask: no length/smoothstep per fragment. */
    if (s->radius > 1.5f || !(s->radius >= 0)) pipeline(SDF, G.bound_texture, BLEND_OVER);
    else pipeline(MASK, G.mask, BLEND_OVER);
    shape_quad(s, 1);
}

static void flat_vertex(float x, float y, rmp_color c)
{
    GLfloat *v = vertex();
    v[0] = x; v[1] = y; v[2] = 0; v[3] = 0; v[4] = v[5] = 1;
    v[6] = v[10] = c.r; v[7] = v[11] = c.g; v[8] = v[12] = c.b; v[9] = v[13] = c.a;
    v[14] = v[15] = v[16] = v[17] = 0;
}

/* Convex polygon: opaque core inset by 0.5px (triangle fan) and a mitred 1px
 * strip whose alpha ramps 1 -> 0, matching rmp_paint_ref_poly_pixels'
 * clamp(0.5 + distance + expand). No per-fragment distance math. */
static void gfx_poly(void *ctx, const float *xy, int n, float expand, rmp_color c)
{
    (void)ctx;
    if (n < 3 || n > RMP_POLY_MAX) return;
    pipeline(POLY, G.bound_texture, BLEND_OVER);
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
    }
    float sign = area > 0 ? 1.f : -1.f, ox[RMP_POLY_MAX], oy[RMP_POLY_MAX];
    for (int i = 0; i < n; i++) {   /* outward unit normal of edge i */
        int j = (i + 1) % n;
        float ex = xy[j * 2] - xy[i * 2], ey = xy[j * 2 + 1] - xy[i * 2 + 1], l = hypotf(ex, ey);
        if (l <= 0) return;
        ox[i] = ey / l * sign; oy[i] = -ex / l * sign;
    }
    float in[2 * RMP_POLY_MAX], out[2 * RMP_POLY_MAX];
    for (int i = 0; i < n; i++) {
        int k = (i + n - 1) % n;
        float mx = ox[k] + ox[i], my = oy[k] + oy[i], den = 1 + ox[k] * ox[i] + oy[k] * oy[i];
        float m = den > 1e-3f ? 1 / den : 0;
        in[i * 2] = xy[i * 2] + mx * m * (expand - .5f); in[i * 2 + 1] = xy[i * 2 + 1] + my * m * (expand - .5f);
        out[i * 2] = xy[i * 2] + mx * m * (expand + .5f); out[i * 2 + 1] = xy[i * 2 + 1] + my * m * (expand + .5f);
    }
    if (G.count + (size_t)(3 * (n - 2) + 6 * n) > CAPACITY) flush();
    rmp_color clear = c;
    clear.a = 0;
    for (int i = 1; i < n - 1; i++) {
        flat_vertex(in[0], in[1], c);
        flat_vertex(in[i * 2], in[i * 2 + 1], c);
        flat_vertex(in[i * 2 + 2], in[i * 2 + 3], c);
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        flat_vertex(in[i * 2], in[i * 2 + 1], c);
        flat_vertex(out[i * 2], out[i * 2 + 1], clear);
        flat_vertex(out[j * 2], out[j * 2 + 1], clear);
        flat_vertex(in[i * 2], in[i * 2 + 1], c);
        flat_vertex(out[j * 2], out[j * 2 + 1], clear);
        flat_vertex(in[j * 2], in[j * 2 + 1], c);
    }
}

static void gfx_glyph(void *ctx, float x, float y, unsigned char ch, float size, rmp_color color)
{
    (void)ctx;
    unsigned char bitmap[5];
    if ((size != 9 && size != 10) || ch < 32 || ch > 127 || !rmp_glyph_bitmap(ch, bitmap)) {
        /* Not in the atlas: dot by dot, same coverage. */
        char s[2] = { (char)ch, 0 };
        rmp_painter dots = G.dynamic_painter;
        dots.glyph = NULL;
        rmp_paint_text(&dots, x, y, s, size, color, 0);
        return;
    }
    long snapped = lroundf(x * RMP_FONT_PHASES);
    unsigned phase = (unsigned)(((snapped % RMP_FONT_PHASES) + RMP_FONT_PHASES) % RMP_FONT_PHASES);
    float ix = (float)((snapped - (long)phase) / RMP_FONT_PHASES);
    pipeline(FONT, G.font, BLEND_OVER);
    float w = ceilf((float)phase / RMP_FONT_PHASES + size / 7 * 1.06f * 5 + .6f) + 1, h = ceilf(size + .6f) + 1;
    float xy[8] = { ix - 1, y - 1, ix - 1 + w, y - 1, ix - 1 + w, y - 1 + h, ix - 1, y - 1 + h };
    quad(xy, NULL, 0);
    unsigned cx, cy;
    rmp_font_cell_origin(phase, (unsigned)ch - 32 + (size == 9 ? 96 : 0), &cx, &cy);
    for (size_t i = G.count - 6; i < G.count; i++) {
        GLfloat *v = &G.vertices[i * FLOATS];
        float u = v[2], t = v[3];
        v[2] = ((float)cx + u * w) / (float)RMP_FONT_ATLAS_W;
        v[3] = ((float)cy + t * h) / (float)RMP_FONT_ATLAS_H;
        v[6] = color.r; v[7] = color.g; v[8] = color.b; v[9] = color.a;
    }
}

/* Opaque columns in one colour: position-only vertices, one draw per 1024. */
static void gfx_fill_rects(void *ctx, const rmp_rect *r, int n, rmp_color c)
{
    (void)ctx;
    if (n <= 0) return;
    flush();
    pipeline(FLAT, G.bound_texture, BLEND_OFF);
    glUniform4f(G.flat_color, c.r, c.g, c.b, 1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (GLuint i = 1; i < 6; i++) glDisableVertexAttribArray(i);
    for (int start = 0; start < n; start += RECTS_PER_DRAW) {
        int m = n - start < RECTS_PER_DRAW ? n - start : RECTS_PER_DRAW, k = 0;
        GLfloat *v = G.flat;
        for (int i = 0; i < m; i++) {
            const rmp_rect *q = &r[start + i];
            if (q->w <= 0 || q->h <= 0) continue;
            float x0 = (float)q->x, y0 = (float)q->y, x1 = (float)(q->x + q->w), y1 = (float)(q->y + q->h);
            const GLfloat t[12] = { x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1 };
            memcpy(v, t, sizeof t);
            v += 12;
            k++;
        }
        if (!k) continue;
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, G.flat);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(k * 6));
    }
    for (GLuint i = 1; i < 6; i++) glEnableVertexAttribArray(i);
}

const rmp_painter *rmp_gfx_painter(void) { return &G.dynamic_painter; }
const rmp_painter *rmp_gfx_static_painter(void) { return &G.static_painter; }

static GLuint texture_image(int w, int h, GLenum format, const void *p, GLint filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, w, h, 0, format, GL_UNSIGNED_BYTE, p);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

int rmp_gfx_check_error(const char *where)
{
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return 0;
    fprintf(stderr, "gfx: GL error 0x%x (%s)\n", (unsigned)e, where);
    while (glGetError() != GL_NO_ERROR) {}
    return -1;
}

int rmp_gfx_init(int display_w, int display_h)
{
    memset(&G, 0, sizeof G);
    const char *vs =
        "attribute vec2 pos;attribute vec2 local;attribute vec2 size;"
        "attribute vec4 top;attribute vec4 bottom;attribute vec4 params;"
        "uniform vec2 viewport;varying mediump vec2 p;varying mediump vec2 sz;"
        "varying mediump vec4 ct;varying mediump vec4 cb;varying mediump vec4 style;"
        "void main(){gl_Position=vec4(pos.x/viewport.x*2.0-1.0,"
        "1.0-pos.y/viewport.y*2.0,0.0,1.0);p=local;sz=size;ct=top;cb=bottom;style=params;}";
    const char *sdf =
        "precision mediump float;varying mediump vec2 p;varying mediump vec2 sz;"
        "varying mediump vec4 ct;varying mediump vec4 cb;varying mediump vec4 style;"
        "void main(){vec2 q=abs(p-sz*0.5)-(sz*0.5-vec2(style.x));"
        "float d=length(max(q,vec2(0.0)))+min(max(q.x,q.y),0.0)-style.x;"
        "vec4 c=mix(ct,cb,clamp(p.y/sz.y,0.0,1.0));"
        "gl_FragColor=vec4(c.rgb,c.a*(1.0-smoothstep(-0.6,0.6,d)));}";
    /* style.zw: atlas UV of the radius tile (inside its gutter). */
    const char *mask =
        "precision mediump float;varying mediump vec2 p;varying mediump vec2 sz;"
        "varying mediump vec4 ct;varying mediump vec4 cb;varying mediump vec4 style;uniform sampler2D tex;"
        "void main(){vec2 q=abs(p-sz*0.5)-(sz*0.5-vec2(style.x));"
        "vec2 uv=style.zw+(vec2(0.5)+clamp((q+0.75)*(127.0/3.0),0.0,127.0))*(1.0/1024.0);"
        "vec4 c=mix(ct,cb,clamp(p.y*style.y,0.0,1.0));"
        "gl_FragColor=vec4(c.rgb,c.a*texture2D(tex,uv).r);}";
    const char *textured = "precision mediump float;varying mediump vec2 p;uniform sampler2D tex;"
        "void main(){gl_FragColor=texture2D(tex,p);}";
    const char *font = "precision mediump float;varying mediump vec2 p;varying mediump vec4 ct;uniform sampler2D tex;"
        "void main(){gl_FragColor=vec4(ct.rgb,ct.a*texture2D(tex,p).a);}";
    /* LED strokes: flat colour, per-vertex alpha carries the 1px edge ramp. */
    const char *poly = "precision mediump float;varying mediump vec4 ct;void main(){gl_FragColor=ct;}";
    const char *flat_vs = "attribute vec2 pos;uniform vec2 viewport;"
        "void main(){gl_Position=vec4(pos.x/viewport.x*2.0-1.0,1.0-pos.y/viewport.y*2.0,0.0,1.0);}";
    const char *flat = "precision mediump float;uniform vec4 color;void main(){gl_FragColor=color;}";
    const char *sources[PROGRAMS] = { sdf, mask, textured, font, poly, flat };
    const char *names[6] = { "pos", "local", "size", "top", "bottom", "params" };
    uint8_t *pixels = NULL;
    for (int mode = 0; mode < PROGRAMS; mode++) {
        GLuint v = shader(GL_VERTEX_SHADER, mode == FLAT ? flat_vs : vs);
        GLuint f = shader(GL_FRAGMENT_SHADER, sources[mode]);
        if (!v || !f) {
            if (v) glDeleteShader(v);
            if (f) glDeleteShader(f);
            goto fail;
        }
        GLuint program = glCreateProgram();
        G.programs[mode] = program;
        glAttachShader(program, v);
        glAttachShader(program, f);
        for (int i = 0; i < 6; i++) glBindAttribLocation(program, (GLuint)i, names[i]);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(program, sizeof log, NULL, log);
            fprintf(stderr, "gfx: program: %s\n", log);
            goto fail;
        }
        G.viewports[mode] = glGetUniformLocation(program, "viewport");
        if (mode == FLAT) G.flat_color = glGetUniformLocation(program, "color");
        glUseProgram(program);
        GLint tex = glGetUniformLocation(program, "tex");
        if (tex >= 0) glUniform1i(tex, 0);
    }
    for (GLuint i = 0; i < 6; i++) glEnableVertexAttribArray(i);
#if RMP_GFX_USE_VBO
    glGenBuffers(1, &G.vbo);
#endif
    pixels = malloc((size_t)RMP_MASK_ATLAS * RMP_MASK_ATLAS);
    if (!pixels) goto fail;
    rmp_mask_atlas_pixels(pixels);
    G.mask = texture_image(RMP_MASK_ATLAS, RMP_MASK_ATLAS, GL_LUMINANCE, pixels, GL_LINEAR);
    rmp_font_atlas_pixels(pixels);   /* 768x768 fits in the same buffer */
    G.font = texture_image(RMP_FONT_ATLAS_W, RMP_FONT_ATLAS_H, GL_ALPHA, pixels, GL_NEAREST);
    free(pixels);
    pixels = NULL;
    G.display_w = display_w;
    G.display_h = display_h;
    G.ready = 1;
    G.dynamic_painter = (rmp_painter){ NULL, dynamic_shape, gfx_poly, gfx_glyph, gfx_fill_rects };
    G.static_painter = (rmp_painter){ NULL, static_shape, gfx_poly, NULL, gfx_fill_rects };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    G.bound_texture = 0;
    G.mode = -1;
    G.blending = -1;
    G.clear_rgb = 0x080C10;
    target(0, display_w, display_h);
    if (rmp_gfx_check_error("init") < 0) goto fail;
    return 0;
fail:
    free(pixels);
    rmp_gfx_shutdown();
    return -1;
}

void rmp_gfx_shutdown(void)
{
    if (G.screen_fbo) glDeleteFramebuffers(1, &G.screen_fbo);
    if (G.screen_tex) glDeleteTextures(1, &G.screen_tex);
    if (G.font) glDeleteTextures(1, &G.font);
    if (G.mask) glDeleteTextures(1, &G.mask);
    if (G.vbo) glDeleteBuffers(1, &G.vbo);
    for (int i = 0; i < PROGRAMS; i++)
        if (G.programs[i]) glDeleteProgram(G.programs[i]);
    memset(&G, 0, sizeof G);
}

static void clear_display(void)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor((float)((G.clear_rgb >> 16) & 255) / 255, (float)((G.clear_rgb >> 8) & 255) / 255,
                 (float)(G.clear_rgb & 255) / 255, 1);
    /* Whole frame: clearing first stops the tiler reloading the old frame. */
    glClear(GL_COLOR_BUFFER_BIT);
    G.need_clear = 0;
}

void rmp_gfx_frame_start(int clear_on_first_bind, unsigned clear_rgb)
{
    G.need_clear = clear_on_first_bind;
    G.clear_rgb = clear_rgb;
}

void rmp_gfx_clip(const rmp_rect *r)
{
    flush();
    if (G.target != G.screen_fbo) to_display();
    if (G.need_clear) clear_display();
    if (!r) { glDisable(GL_SCISSOR_TEST); return; }
    int w = r->w > 0 ? r->w : 0, h = r->h > 0 ? r->h : 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(r->x, G.display_h - (r->y + h), w, h);
}

static void paste_texture(int tw, int th, rmp_rect src, float x0, float y0, float x1, float y1);

/* Scaled output: the whole surface each frame, black bars around the
 * display texture. The next rmp_gfx_clip binds the display again. */
static void present_scaled(void)
{
    target(0, G.out_w, G.out_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pipeline(TEXTURE, G.screen_tex, BLEND_OFF);
    paste_texture(G.display_w, G.display_h, (rmp_rect){ 0, 0, G.display_w, G.display_h },
                  (float)G.dst.x, (float)G.dst.y, (float)(G.dst.x + G.dst.w), (float)(G.dst.y + G.dst.h));
    flush();
}

void rmp_gfx_frame_finish(void)
{
    flush();
    if (G.target != G.screen_fbo) to_display();
    if (G.need_clear) clear_display();
    glDisable(GL_SCISSOR_TEST);
    if (G.screen_fbo) present_scaled();
}

static void drop_screen(void)
{
    flush();
    if (G.screen_fbo) glDeleteFramebuffers(1, &G.screen_fbo);
    if (G.screen_tex) glDeleteTextures(1, &G.screen_tex);
    G.screen_fbo = G.screen_tex = 0;
    G.bound_texture = (GLuint)-1;
    target(0, G.display_w, G.display_h);
}

int rmp_gfx_set_output(int out_w, int out_h)
{
    if (!G.ready || out_w <= 0 || out_h <= 0) return -1;
    if (out_w == G.display_w && out_h == G.display_h) {
        if (G.screen_fbo) drop_screen();
        return 0;
    }
    /* One scale for both axes (aspect kept), centred: bars left/right on a
     * wider surface, top/bottom on a taller one. */
    float s = fminf((float)out_w / (float)G.display_w, (float)out_h / (float)G.display_h);
    int w = (int)lroundf((float)G.display_w * s), h = (int)lroundf((float)G.display_h * s);
    G.dst = (rmp_rect){ (out_w - w) / 2, (out_h - h) / 2, w, h };
    G.out_w = out_w;
    G.out_h = out_h;
    /* A display the app sized itself, shown 2x, 3x ...: whole texels. */
    GLint filter = G.sharp && s >= 1 && fabsf(s - roundf(s)) < 1e-4f ? GL_NEAREST : GL_LINEAR;
    if (G.screen_fbo) {                    /* the same texture, a new place */
        flush();
        glBindTexture(GL_TEXTURE_2D, G.screen_tex);
        G.bound_texture = G.screen_tex;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        return 1;
    }
    flush();
    G.screen_tex = texture_image(G.display_w, G.display_h, GL_RGBA, NULL, filter);
    G.bound_texture = G.screen_tex;        /* left bound by texture_image */
    glGenFramebuffers(1, &G.screen_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.screen_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.screen_tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gfx: display framebuffer incomplete 0x%x\n", (unsigned)status);
        drop_screen();
        return -1;
    }
    to_display();
    return 1;
}

int rmp_gfx_set_display(int display_w, int display_h, int sharp)
{
    if (!G.ready || display_w <= 0 || display_h <= 0) return -1;
    if (G.screen_fbo) drop_screen();
    G.display_w = display_w;
    G.display_h = display_h;
    G.sharp = sharp != 0;
    target(0, display_w, display_h);
    return 0;
}

rmp_layer *rmp_layer_create(int w, int h)
{
    if (!G.ready || w <= 0 || h <= 0) return NULL;
    rmp_layer *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    flush();
    l->w = w;
    l->h = h;
    l->texture = texture_image(w, h, GL_RGBA, NULL, GL_NEAREST);
    glGenFramebuffers(1, &l->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l->texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    G.bound_texture = l->texture;          /* left bound by texture_image */
    to_display();
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gfx: layer framebuffer incomplete 0x%x\n", (unsigned)status);
        rmp_layer_destroy(l);
        return NULL;
    }
    return l;
}

void rmp_layer_destroy(rmp_layer *l)
{
    if (!l) return;
    if (G.ready) {
        flush();
        if (G.target == l->fbo) to_display();
        if (G.bound_texture == l->texture) {
            glBindTexture(GL_TEXTURE_2D, 0);
            G.bound_texture = 0;
        }
        glDeleteFramebuffers(1, &l->fbo);
        glDeleteTextures(1, &l->texture);
    }
    free(l);
}

void rmp_layer_begin(rmp_layer *l, unsigned rgb)
{
    flush();
    glDisable(GL_SCISSOR_TEST);
    target(l->fbo, l->w, l->h);
    /* Clear is mandatory on lima, including a cached FBO's first use. */
    glClearColor((float)((rgb >> 16) & 255) / 255, (float)((rgb >> 8) & 255) / 255, (float)(rgb & 255) / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void rmp_layer_resume(rmp_layer *l)
{
    if (!G.ready || !l) return;
    flush();
    glDisable(GL_SCISSOR_TEST);
    target(l->fbo, l->w, l->h);
}

void rmp_layer_end(void)
{
    flush();
    to_display();
}

void rmp_layer_paste(const rmp_layer *l, rmp_rect src, int x, int y)
{
    if (src.w <= 0 || src.h <= 0) return;
    pipeline(TEXTURE, l->texture, BLEND_OFF);
    paste_texture(l->w, l->h, src, (float)x, (float)y, (float)(x + src.w), (float)(y + src.h));
}

/* Queues `src` of a framebuffer texture (tw x th) as the quad x0,y0-x1,y1.
 * The caller has selected pipeline(TEXTURE, <that texture>, ...). */
static void paste_texture(int tw, int th, rmp_rect src, float x0, float y0, float x1, float y1)
{
    float xy[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    float W = (float)tw, H = (float)th;
    /* The FBO's bottom row is texture t=0: top-left pixel maps to t=1. */
    float u0 = (float)src.x / W, v0 = 1 - (float)src.y / H;
    float u1 = (float)(src.x + src.w) / W, v1 = 1 - (float)(src.y + src.h) / H;
    quad(xy, NULL, 0);
    for (size_t i = G.count - 6; i < G.count; i++) {
        GLfloat *v = &G.vertices[i * FLOATS];
        v[2] = u0 + (u1 - u0) * v[2];
        v[3] = v0 + (v1 - v0) * v[3];
    }
}
