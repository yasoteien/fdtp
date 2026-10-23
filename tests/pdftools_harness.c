// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 item 39: the PDF menu (pdftools.c, pdftools_job.c) on its own
 * (Linux, libmupdf), for tests/check_pdftools.py.
 *
 *   pdftools-harness run DIR SCRIPT PREFIX ENTRY PAGE SIZE NAME...
 *     ENTRY menu, split or merge (Alt+p, Alt+n, Alt+h). PAGE -1: from the
 *     filer (a cell screen stands for it); else the viewer's page, with a
 *     pdfview of NAME[0] under the tools. SIZE: 80x25 or 85x25.
 *     The script is the headless key language. WAIT:s ticks until s
 *     seconds have passed, or sooner once the tools are not on top or a box
 *     (not a notice) is open; WAIT:0 is one tick. Per SNAP: PREFIX-NNN.ppm
 *     and the lines "SNAP n depth widget", "ROW y text", "BOX col row w h
 *     kind rule", "LABEL text", "TEXT text" (an input), "INFO name|value",
 *     "GLYPH U+XXXX" (a character not in the font). At the end: "OPENED r",
 *     "RELOADS n", "HISTORY entry" (every remembered answer), "TIME s",
 *     "VMHWM kB".
 *   pdftools-harness inspect FILE [PASSWORD]
 *     "OPEN ok|password|bad", "ENCRYPTED 0|1 METHOD m BITS n", "REPAIRED
 *     0|1", "PAGES n", per page "PAGE i W H ROTATE r IMAGES n IMGW w IMGH h".
 * The order box is W1's src/order_box.c, linked with the harness. */
#include "pdftools.h"
#include "pdfview.h"
#include "app.h"
#include "config.h"
#include "font24.h"
#include "raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

static int reloads;
/* The App parts reference these; the harness links no filer or config. */
void filer_reload(Screen *s, App *a) { (void)s; (void)a; ++reloads; }
void filer_apply_settings(Screen *s, App *a) { (void)s; (void)a; }
void config_free(App *a) { (void)a; }
int config_startup(App *a) { (void)a; return 0; }

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static long hwm(void)
{
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; long kb = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) if (!strncmp(line, "VmHWM:", 6)) kb = atol(line + 6);
    fclose(f); return kb;
}

/* The filer's place: a cell screen with the frame and a line of text. */
static void fake_draw(Screen *s, App *a, Canvas *c, Cells *cells)
{
    (void)s; (void)a; (void)c;
    cells_init(cells); frame_open(cells);
    frame_title(cells, FRAME_TITLE_COL, "ファイラーの代わり（試験）", 40, NULL, 0);
    cells_text(cells, 2, 2, "pdftools-harness", WHITE, BG, 40);
}
static int fake_key(Screen *s, App *a, const Key *k) { (void)s; (void)a; (void)k; return 0; }
static void fake_destroy(Screen *s) { free(s); }

static void put_utf8(uint32_t u)
{
    char b[4]; int n;
    if (u < 0x80) { b[0] = (char)u; n = 1; }
    else if (u < 0x800) { b[0] = (char)(0xc0 | u >> 6); b[1] = (char)(0x80 | (u & 63)); n = 2; }
    else if (u < 0x10000) { b[0] = (char)(0xe0 | u >> 12); b[1] = (char)(0x80 | (u >> 6 & 63)); b[2] = (char)(0x80 | (u & 63)); n = 3; }
    else { b[0] = (char)(0xf0 | u >> 18); b[1] = (char)(0x80 | (u >> 12 & 63)); b[2] = (char)(0x80 | (u >> 6 & 63)); b[3] = (char)(0x80 | (u & 63)); n = 4; }
    fwrite(b, 1, (size_t)n, stdout);
}
static void dump(App *a, const Cells *c, unsigned serial)
{
    printf("SNAP %u %d %d\n", serial, a->depth, (int)a->widgets.kind);
    for (int y = 0; y < FD_ROWS; y++) {
        printf("ROW %d ", y);
        for (int x = 0; x < FD_COLS; x++) {
            const Cell *v = &c->data[y][x];
            if (v->part == 2) continue;
            uint32_t u = v->code ? v->code : ' ';
            put_utf8(u);
        }
        putchar('\n');
    }
    for (int y = 0; y < FD_ROWS; y++) for (int x = 0; x < FD_COLS; x++) {
        uint32_t u = c->data[y][x].code;
        if (u && c->data[y][x].part != 2 && font24_lookup(u)->code != u) printf("GLYPH U+%04X\n", (unsigned)u);
    }
    for (int i = 0; i < c->box_count; i++) {
        const CellBox *b = &c->boxes[i];
        printf("BOX %d %d %d %d %d %d\n", b->col, b->row, b->w, b->h, b->kind, b->rule);
    }
    if (a->widgets.kind != WIDGET_NONE) printf("LABEL %s\n", a->widgets.label);
    if (a->widgets.kind == WIDGET_INPUT) printf("TEXT %s\n", a->widgets.text);
    if (a->widgets.kind == WIDGET_INFO)
        for (int i = 0; i < a->widgets.info_count; i++) printf("INFO %s|%s\n", a->widgets.info_name[i], a->widgets.info_value[i]);
}

static int run(int argc, char **argv)
{
    if (argc < 9) return 2;
    const char *dir = argv[2], *prefix = argv[4], *entry = argv[5];
    int page = atoi(argv[6]), cols = 80, rows = 25;
    if (sscanf(argv[7], "%dx%d", &cols, &rows) != 2) return 2;
    FILE *f = fopen(argv[3], "rb"); if (!f) return 2;
    static char text[65536]; size_t n = fread(text, 1, sizeof(text) - 1, f); fclose(f); text[n] = 0;
    screen_apply(screen_choose(cols, rows, 0, 0));
    widgets_history_home(0);
    App a; app_init(&a); a.now = 1000;
    Canvas *c = malloc(sizeof(*c)); Cells cells; cells_init(&cells);
    Screen *filer = calloc(1, sizeof(*filer));
    if (!c || !filer) return 2;
    canvas_init(c);
    *filer = (Screen){ filer, fake_key, fake_draw, 0, NULL, fake_destroy };
    app_push(&a, filer);
    const char **names = (const char **)argv + 8; size_t count = (size_t)(argc - 8);
    if (page >= 0) {
        size_t len = strlen(dir) + strlen(names[0]) + 2; char *path = malloc(len);
        snprintf(path, len, "%s/%s", dir, names[0]);
        Screen *v = pdfview_create(&a, path); free(path);
        if (!v || app_push(&a, v) < 0) { printf("VIEWER failed %s\n", a.widgets.label); return 3; }
        app_tick(&a, a.now += 0.1);   /* the page drawn */
    }
    int under_depth = a.depth;
    FileSelection sel = { dir, names[0], names, count };
    PdfToolsEntry e = !strcmp(entry, "split") ? PDFTOOLS_SPLIT : !strcmp(entry, "merge") ? PDFTOOLS_MERGE : PDFTOOLS_MENU;
    double t0 = mono();
    int opened = pdftools_open(&a, &sel, e, page);
    printf("OPENED %d\n", opened);
    /* The tools' screen (the order box may already be over it). */
    Screen *tools = a.depth > under_depth ? a.screens[under_depth] : NULL;
    KeyScript script; keys_script_init(&script, text); unsigned serial = 0; int rc = 0;
    for (;;) {
        ScriptEvent ev = keys_script_next(&script);
        if (ev.kind == SCRIPT_END) break;
        if (ev.kind == SCRIPT_ERROR) { fprintf(stderr, "script: %s\n", script.error); rc = 2; break; }
        if (ev.kind == SCRIPT_WAIT) {
            double end = mono() + ev.seconds;
            for (;;) {
                app_tick(&a, a.now += 0.05);
                if (mono() >= end || !tools || app_top(&a) != tools) break;
                if (a.widgets.kind != WIDGET_NONE && a.widgets.kind != WIDGET_NOTICE) break;
            }
            continue;
        }
        if (ev.kind == SCRIPT_KEY) { app_key(&a, &ev.key); continue; }
        if (ev.kind == SCRIPT_SNAP) {
            if (!app_top(&a)) { printf("SNAP %u 0 0\n", ++serial); continue; }
            app_draw(&a, c, &cells);
            Screen *top = app_top(&a);
            if (!top->pixel) { FDRect rects[FD_ROWS_MAX]; raster_update(c, &cells, rects); }
            char path[4096]; snprintf(path, sizeof(path), "%s-%03u.ppm", prefix, ++serial);
            if (canvas_write_ppm(c, path)) { rc = 2; break; }
            dump(&a, &cells, serial);
        }
    }
    printf("RELOADS %d\n", reloads);
    for (int k = 1; k < WIDGET_HISTORY_KINDS; k++)
        for (int i = 0; i < widgets_history_count((WidgetHistoryKind)k); i++) printf("HISTORY %s\n", widgets_history_entry((WidgetHistoryKind)k, i));
    printf("TIME %.3f\nVMHWM %ld\n", mono() - t0, hwm());
    app_destroy(&a); free(c); return rc;
}

static void quiet(void *user, const char *message) { (void)user; (void)message; }
static int inspect(int argc, char **argv)
{
    if (argc < 3) return 2;
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) return 2;
    fz_set_warning_callback(ctx, quiet, NULL); fz_set_error_callback(ctx, quiet, NULL);
    pdf_document *doc = NULL; int rc = 0;
    fz_var(doc); fz_var(rc);
    fz_try(ctx) {
        doc = pdf_open_document(ctx, argv[2]);
        int repaired = pdf_was_repaired(ctx, doc);
        if (pdf_needs_password(ctx, doc) && (argc < 4 || !pdf_authenticate_password(ctx, doc, argv[3]))) printf("OPEN password\n");
        else {
            printf("OPEN ok\n");
            printf("ENCRYPTED %d METHOD %s BITS %d\n", doc->crypt != NULL, doc->crypt ? pdf_crypt_method(ctx, doc->crypt) : "none",
                   doc->crypt ? pdf_crypt_length(ctx, doc->crypt) : 0);
            printf("REPAIRED %d\n", repaired);
            int n = pdf_count_pages(ctx, doc);
            printf("PAGES %d\n", n);
            for (int i = 0; i < n; i++) {
                pdf_obj *page = pdf_lookup_page_obj(ctx, doc, i);
                fz_rect box = pdf_dict_get_inheritable_rect(ctx, page, PDF_NAME(MediaBox));
                int rotate = pdf_dict_get_inheritable_int(ctx, page, PDF_NAME(Rotate));
                pdf_obj *xobjects = pdf_dict_get(ctx, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)), PDF_NAME(XObject));
                int images = 0, w = 0, h = 0;
                for (int k = 0; k < pdf_dict_len(ctx, xobjects); k++) {
                    pdf_obj *x = pdf_dict_get_val(ctx, xobjects, k);
                    if (!pdf_name_eq(ctx, pdf_dict_get(ctx, x, PDF_NAME(Subtype)), PDF_NAME(Image))) continue;
                    images++;
                    int iw = pdf_dict_get_int(ctx, x, PDF_NAME(Width)), ih = pdf_dict_get_int(ctx, x, PDF_NAME(Height));
                    if (iw > w) w = iw;
                    if (ih > h) h = ih;
                }
                printf("PAGE %d %.2f %.2f ROTATE %d IMAGES %d IMGW %d IMGH %d\n", i, box.x1 - box.x0, box.y1 - box.y0, rotate, images, w, h);
            }
        }
    }
    fz_always(ctx) pdf_drop_document(ctx, doc);
    fz_catch(ctx) { printf("OPEN bad\n"); rc = 0; }
    fz_drop_context(ctx);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "run")) return run(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "inspect")) return inspect(argc, argv);
    fprintf(stderr, "usage: pdftools-harness run DIR SCRIPT PREFIX ENTRY PAGE SIZE NAME... | inspect FILE [PASSWORD]\n");
    return 2;
}
