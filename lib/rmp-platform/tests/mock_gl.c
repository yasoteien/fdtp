// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mock_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mock_event mock_log[MOCK_LOG];
int mock_log_n;
int mock_age;
int mock_swap_fails;
int mock_partial_update = 1;
EGLint mock_damage_rects[4 * 64];
int mock_damage_n;
GLuint mock_programs_linked;
static double zero_time(void) { return 0; }
double (*mock_time)(void) = zero_time;
double mock_last_swap_time;

static struct {
    GLuint fb, program, next_name;
    int scissor_on, blend, sx, sy, sw, sh;
    GLenum error;
} S = { 0, 0, 100, 0, 0, 0, 0, 0, 0, GL_NO_ERROR };

static void logev(int type, int a, int b, int c, int d)
{
    if (mock_log_n >= MOCK_LOG) return;
    mock_event e = { type, a, b, c, d, S.fb, S.scissor_on, S.sx, S.sy, S.sw, S.sh, S.program, S.blend, mock_time() };
    mock_log[mock_log_n++] = e;
}
void mock_log_external(int what, int a) { logev(EV_EXTERNAL, what, a, 0, 0); }
void mock_gl_reset_log(void) { mock_log_n = 0; }
int mock_find(int type, int from)
{
    for (int i = from < 0 ? 0 : from; i < mock_log_n; i++) if (mock_log[i].type == type) return i;
    return -1;
}
int mock_count(int type, int from, int to)
{
    int n = 0;
    for (int i = from; i < to && i < mock_log_n; i++) n += mock_log[i].type == type;
    return n;
}

/* ---- EGL ---- */
static int dummy_display, dummy_surface, dummy_context, dummy_config;
static EGLDisplay EGLAPIENTRY get_platform_display(EGLenum platform, void *native, const EGLint *attrs)
{
    (void)platform; (void)native; (void)attrs;
    return (EGLDisplay)&dummy_display;
}
static EGLBoolean EGLAPIENTRY set_damage_region(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n)
{
    (void)d; (void)s;
    mock_damage_n = n;
    memcpy(mock_damage_rects, rects, sizeof(EGLint) * 4 * (size_t)n);
    logev(EV_DAMAGE, n, 0, 0, 0);
    return EGL_TRUE;
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
    if (!strcmp(name, "eglGetPlatformDisplayEXT")) return (__eglMustCastToProperFunctionPointerType)get_platform_display;
    if (!strcmp(name, "eglSetDamageRegionKHR")) return (__eglMustCastToProperFunctionPointerType)set_damage_region;
    return NULL;
}
EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) { (void)d; if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGLBoolean eglBindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *configs, EGLint size, EGLint *n)
{
    (void)d; (void)attrs;
    *n = 1;
    if (configs && size > 0) configs[0] = (EGLConfig)&dummy_config;
    return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *value)
{
    (void)d; (void)c;
    if (attr == EGL_NATIVE_VISUAL_ID) *value = 0x34325258;   /* XRGB8888 */
    return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs)
{
    (void)d; (void)c; (void)share; (void)attrs;
    return (EGLContext)&dummy_context;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *attrs)
{
    (void)d; (void)c; (void)w; (void)attrs;
    return (EGLSurface)&dummy_surface;
}
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface a, EGLSurface b, EGLContext c) { (void)d; (void)a; (void)b; (void)c; return EGL_TRUE; }
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return EGL_TRUE; }
const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d; (void)name;
    return mock_partial_update ? "EGL_KHR_platform_gbm EGL_EXT_buffer_age EGL_KHR_partial_update" : "EGL_EXT_buffer_age";
}
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *value)
{
    (void)d; (void)s;
    if (attr == 0x313D) { *value = mock_age; logev(EV_AGE, mock_age, 0, 0, 0); return EGL_TRUE; }
    return EGL_FALSE;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    (void)d; (void)s;
    logev(EV_SWAP, 0, 0, 0, 0);
    mock_last_swap_time = mock_time();
    return mock_swap_fails ? EGL_FALSE : EGL_TRUE;
}
EGLint eglGetError(void) { return EGL_SUCCESS; }
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { (void)d; (void)c; return EGL_TRUE; }
EGLBoolean eglTerminate(EGLDisplay d) { (void)d; logev(EV_TERMINATE, 0, 0, 0, 0); return EGL_TRUE; }

/* ---- GLES2 ---- */
GLuint glCreateShader(GLenum t) { (void)t; return S.next_name++; }
void glShaderSource(GLuint s, GLsizei n, const GLchar *const *src, const GLint *len) { (void)s; (void)n; (void)src; (void)len; }
void glCompileShader(GLuint s) { (void)s; }
const GLubyte *glGetString(GLenum name) { return (const GLubyte *)(name == GL_RENDERER ? (getenv("MOCK_GL_RENDERER") ? getenv("MOCK_GL_RENDERER") : "Mali400") : ""); }
void glGetShaderiv(GLuint s, GLenum p, GLint *v) { (void)s; (void)p; *v = GL_TRUE; }
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *log) { (void)s; (void)n; if (len) *len = 0; if (log) log[0] = 0; }
void glDeleteShader(GLuint s) { (void)s; }
GLuint glCreateProgram(void) { return S.next_name++; }
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint i, const GLchar *n) { (void)p; (void)i; (void)n; }
void glLinkProgram(GLuint p) { (void)p; mock_programs_linked++; }
void glGetProgramiv(GLuint p, GLenum n, GLint *v) { (void)p; (void)n; *v = GL_TRUE; }
void glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *len, GLchar *log) { (void)p; (void)n; if (len) *len = 0; if (log) log[0] = 0; }
GLint glGetUniformLocation(GLuint p, const GLchar *n) { (void)p; return strcmp(n, "tex") ? 1 : 0; }
void glUseProgram(GLuint p) { S.program = p; }
void glUniform1i(GLint l, GLint v) { (void)l; (void)v; }
void glUniform2f(GLint l, GLfloat a, GLfloat b) { (void)l; (void)a; (void)b; }
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)l; (void)a; (void)b; (void)c; (void)d; }
void glEnableVertexAttribArray(GLuint i) { (void)i; }
void glDisableVertexAttribArray(GLuint i) { (void)i; }
void glGenBuffers(GLsizei n, GLuint *b) { for (GLsizei i = 0; i < n; i++) b[i] = S.next_name++; }
void glBindBuffer(GLenum t, GLuint b) { (void)t; (void)b; }
void glBufferData(GLenum t, GLsizeiptr n, const void *d, GLenum u) { (void)t; (void)n; (void)d; (void)u; }
void glDeleteBuffers(GLsizei n, const GLuint *b) { (void)n; (void)b; }
void glVertexAttribPointer(GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride, const void *p)
{ (void)i; (void)n; (void)t; (void)norm; (void)stride; (void)p; }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { (void)mode; (void)first; logev(EV_DRAW, count, 0, 0, 0); }
void glGenTextures(GLsizei n, GLuint *t) { for (GLsizei i = 0; i < n; i++) t[i] = S.next_name++; }
void glBindTexture(GLenum t, GLuint x) { (void)t; (void)x; }
void glTexImage2D(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b, GLenum f2, GLenum ty, const void *p)
{ (void)t; (void)l; (void)f; (void)b; (void)f2; (void)ty; (void)p; logev(EV_TEXIMAGE, w, h, (int)f, 0); }
void glTexParameteri(GLenum t, GLenum p, GLint v) { (void)t; if (p == GL_TEXTURE_MAG_FILTER) logev(EV_MAGFILTER, v, 0, 0, 0); }
void glDeleteTextures(GLsizei n, const GLuint *t) { (void)n; (void)t; }
void glActiveTexture(GLenum t) { (void)t; }
void glEnable(GLenum c) { if (c == GL_SCISSOR_TEST) S.scissor_on = 1; if (c == GL_BLEND) S.blend = 1; }
void glDisable(GLenum c)
{
    if (c == GL_SCISSOR_TEST) { S.scissor_on = 0; logev(EV_SCISSOR_OFF, 0, 0, 0, 0); }
    if (c == GL_BLEND) S.blend = 0;
}
void glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }
void glBindFramebuffer(GLenum t, GLuint f) { (void)t; S.fb = f; logev(EV_BINDFB, (int)f, 0, 0, 0); }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
void glGenFramebuffers(GLsizei n, GLuint *f) { for (GLsizei i = 0; i < n; i++) { f[i] = S.next_name++; logev(EV_GENFB, (int)f[i], 0, 0, 0); } }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { (void)t; (void)a; (void)tt; (void)tex; (void)l; }
GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
void glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n; (void)f; }
void glDeleteProgram(GLuint p) { (void)p; logev(EV_DELPROGRAM, 0, 0, 0, 0); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
void glClear(GLbitfield m) { (void)m; logev(EV_CLEAR, 0, 0, 0, 0); }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sx = x; S.sy = y; S.sw = w; S.sh = h; logev(EV_SCISSOR, x, y, w, h); }
GLenum glGetError(void) { GLenum e = S.error; S.error = GL_NO_ERROR; return e; }
