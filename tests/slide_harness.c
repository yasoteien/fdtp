// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Test entrance for stage 6 (tests/check_slide.py): opens a pptx directly
 * (the launcher already calls slideview_create), runs a key script and
 * writes each SNAP as PPM and cells JSON.
 *   slide_harness open FILE SCRIPT PREFIX
 *   slide_harness make-jpeg OUT        32x32, top half blue, bottom yellow */
#include "slideview.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

/* app.c's links to the filer and the settings, which this entrance leaves
 * out (as tests/sheet_harness.c). */
void filer_reload(Screen *s, App *a) { (void)s; (void)a; }
void filer_apply_settings(Screen *s, App *a) { (void)s; (void)a; }
/* UNDO_LEVELS (revision 1 item 41) from $FDTP_UNDO_LEVELS, else the default. */
int config_startup(App *a)
{
    const char *e = getenv("FDTP_UNDO_LEVELS");
    a->settings.undo_levels = e && atoi(e) > 0 ? atoi(e) : APP_UNDO_DEFAULT;
    return 0;
}
void config_free(App *a) { (void)a; }
int config_first_run_key(App *a, const Key *k) { (void)a; (void)k; return 0; }
void config_first_run_draw(App *a, Cells *cells) { (void)a; (void)cells; }

/* Standalone platform callbacks: no real window is opened by this harness. */
static int test_fullscreen, test_fullscreen_sets;
static int test_is_fullscreen(void) { return test_fullscreen; }
static void test_set_fullscreen(int on) { test_fullscreen = !!on; test_fullscreen_sets++; }

static int make_jpeg(const char *path)
{
    unsigned char rgb[32 * 3];
    FILE *f = fopen(path, "wb");
    if (!f) return 2;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = c.image_height = 32;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < 32) {
        int top = c.next_scanline < 16;
        for (int x = 0; x < 32; ++x) {
            rgb[x * 3] = top ? 0 : 255;
            rgb[x * 3 + 1] = top ? 0 : 255;
            rgb[x * 3 + 2] = top ? 255 : 0;
        }
        JSAMPROW row = rgb;
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return fclose(f) ? 2 : 0;
}
static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 32) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static int snapshot(App *a, Canvas *canvas, Cells *cells, const char *prefix, unsigned serial)
{
    app_draw(a, canvas, cells);
    char path[4096];
    snprintf(path, sizeof(path), "%s-%03u.ppm", prefix, serial);
    if (canvas_write_ppm(canvas, path)) return -1;
    snprintf(path, sizeof(path), "%s-%03u.cells", prefix, serial);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\"rows\":[\n");
    for (int y = 0; y < FD_ROWS; ++y) {
        fputc('[', f);
        for (int x = 0; x < FD_COLS; ++x) {
            Cell v = cells->data[y][x];
            fprintf(f, "%s[%u,%u,%u,%u]", x ? "," : "", v.code, v.fg, v.bg, v.part);
        }
        fprintf(f, "]%s\n", y + 1 < FD_ROWS ? "," : "");
    }
    fprintf(f, "],\"depth\":%d,\"fullscreen\":%d,\"notice\":", a->depth, test_fullscreen);
    json_string(f, widgets_message(&a->widgets) ? a->widgets.label : "");
    fprintf(f, ",\"fullscreen_sets\":%d", test_fullscreen_sets);
    fprintf(f, "}\n");
    return fclose(f) ? -1 : 0;
}
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *p = malloc(65536);
    size_t n = p ? fread(p, 1, 65535, f) : 0;
    fclose(f);
    if (p) p[n] = 0;
    return p;
}
static int open_deck(const char *file, const char *script_path, const char *prefix)
{
    const char *env_script = getenv("FDTP_TEST_KEYS");
    const char *env_prefix = getenv("FDTP_SNAPSHOT_PREFIX");
    char *script = env_script ? strdup(env_script) : slurp(script_path);
    if (env_prefix && *env_prefix) prefix = env_prefix;
    if (!script) { perror(script_path); return 2; }
    /* $FDTP_TEST_SIZE (85x25: the Pomera) instead of the tests' 80x25. */
    const char *size = getenv("FDTP_TEST_SIZE");
    int cols, rows;
    if (size && !screen_parse(size, &cols, &rows)) screen_apply(screen_choose(cols, rows, 1024, 600));
    App a;
    app_init(&a);
    test_fullscreen_sets = 0;
    test_fullscreen = getenv("FDTP_TEST_FULLSCREEN") && atoi(getenv("FDTP_TEST_FULLSCREEN"));
    a.is_fullscreen = test_is_fullscreen;
    a.set_fullscreen = test_set_fullscreen;
    Screen *s = slideview_create(&a, file);
    if (!s) {
        printf("NOTICE %s\n", a.widgets.label);
        free(script);
        app_destroy(&a);
        return 3;
    }
    if (app_push(&a, s)) { s->destroy(s); free(script); app_destroy(&a); return 2; }
    Canvas *canvas = malloc(sizeof(*canvas));
    Cells cells;
    cells_init(&cells);
    if (!canvas) { free(script); app_destroy(&a); return 2; }
    canvas_init(canvas);
    KeyScript k;
    keys_script_init(&k, script);
    unsigned serial = 0;
    int rc = 0;
    for (;;) {
        ScriptEvent e = keys_script_next(&k);
        if (e.kind == SCRIPT_END) break;
        if (e.kind == SCRIPT_ERROR) { fprintf(stderr, "script: %s\n", k.error); rc = 2; break; }
        if (e.kind == SCRIPT_WAIT) { app_tick(&a, a.now + e.seconds); continue; }
        if (e.kind == SCRIPT_DISPLAY) {
            screen_apply(screen_choose(0, 0, e.width, e.height));
            canvas_fit(canvas);
            cells_init(&cells);
            continue;
        }
        if (e.kind == SCRIPT_MODS) { app_modifiers(&a, e.mods); continue; }
        if (e.kind == SCRIPT_KEY) {
            if (!app_top(&a)) { fprintf(stderr, "script: key after the screen closed\n"); rc = 4; break; }
            app_key(&a, &e.key);
            continue;
        }
        if (!app_top(&a)) {
            printf("SNAP %u 0 %s\n", ++serial, widgets_message(&a.widgets) ? a.widgets.label : "");
            continue;
        }
        if (snapshot(&a, canvas, &cells, prefix, ++serial)) { perror(prefix); rc = 2; break; }
        printf("SNAP %u %d %s\n", serial, a.depth, widgets_message(&a.widgets) ? a.widgets.label : "");
    }
    printf("END %d\n", a.depth);
    app_destroy(&a);
    printf("FULLSCREEN %d\n", test_fullscreen);
    free(canvas);
    free(script);
    return rc;
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "open")) return open_deck(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "make-jpeg")) return make_jpeg(argv[2]);
    fprintf(stderr, "usage: see tests/slide_harness.c\n");
    return 2;
}
