// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Headless entrance for the novel editor tests (tests/check_novel.py):
 *   novel-harness FILE SCRIPT PREFIX
 * Opens FILE in noveledit, runs the key script (the .keys syntax; WAIT
 * advances the virtual clock and ticks), and for every SNAP writes
 * PREFIX-NNN.ppm and prints one JSON line with the editor state. */
#include "noveledit.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_text(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}
int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s FILE SCRIPT PREFIX\n", argv[0]); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    static char text[1 << 20];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    App *a = calloc(1, sizeof(*a));
    Canvas *c = malloc(sizeof(*c));
    char *state = malloc(1 << 20);
    if (!a || !c || !state) return 2;
    app_init(a);
    app_tick(a, 1000.0);
    Screen *s = noveledit_create(a, argv[1]);
    if (!s) { printf("{\"open\":false,\"notice\":"); json_text(a->widgets.label); printf("}\n"); app_destroy(a); free(a); free(c); free(state); return 3; }
    if (app_push(a, s)) { s->destroy(s); return 2; }
    Cells cells; cells_init(&cells); canvas_init(c);
    KeyScript script; keys_script_init(&script, text);
    unsigned serial = 0; int rc = 0;
    for (;;) {
        ScriptEvent e = keys_script_next(&script);
        if (e.kind == SCRIPT_END) break;
        if (e.kind == SCRIPT_ERROR) { fprintf(stderr, "script: %s\n", script.error); rc = 2; break; }
        if (e.kind == SCRIPT_WAIT) { app_tick(a, a->now + e.seconds); continue; }
        if (e.kind == SCRIPT_KEY) {
            if (!app_top(a)) { fprintf(stderr, "key after the editor closed\n"); rc = 4; break; }
            app_key(a, &e.key);
            continue;
        }
        ++serial;
        printf("{\"serial\":%u,\"depth\":%d,\"notice\":", serial, a->depth);
        json_text(widgets_message(&a->widgets) ? a->widgets.label : "");
        /* The App widget over the editor (WidgetKind) with its label and text. */
        printf(",\"widget\":%d,\"widget_label\":", (int)a->widgets.kind);
        json_text(a->widgets.kind != WIDGET_NONE ? a->widgets.label : "");
        printf(",\"widget_text\":");
        json_text(a->widgets.kind == WIDGET_INPUT ? a->widgets.text : "");
        /* An information box: its rows [name, value]; a choice: its options. */
        printf(",\"widget_rows\":[");
        if (a->widgets.kind == WIDGET_INFO)
            for (int i = 0; i < a->widgets.info_count; ++i) {
                printf("%s[", i ? "," : "");
                json_text(a->widgets.info_name[i]); putchar(','); json_text(a->widgets.info_value[i]); putchar(']');
            }
        if (a->widgets.kind == WIDGET_CHOICE)
            for (int i = 0; i < a->widgets.option_count; ++i) { if (i) putchar(','); json_text(a->widgets.options[i]); }
        /* A help box: [key, text], key null for a heading (item 25). */
        if (a->widgets.kind == WIDGET_HELP)
            for (int i = 0; i < a->widgets.help_count; ++i) {
                const WidgetHelpItem *it = &a->widgets.help[i];
                printf("%s[", i ? "," : "");
                if (it->key) json_text(it->key); else printf("null");
                putchar(','); json_text(it->text); putchar(']');
            }
        printf("],\"widget_selected\":%d", a->widgets.kind == WIDGET_CHOICE ? a->widgets.selected : -1);
        /* The first row shown of a box that scrolls (item 25). */
        printf(",\"widget_top\":%d", a->widgets.kind == WIDGET_HELP ? a->widgets.help_top : a->widgets.kind == WIDGET_INFO ? a->widgets.info_top : 0);
        if (app_top(a)) {
            app_draw(a, c, &cells);
            char path[4096];
            snprintf(path, sizeof(path), "%s-%03u.ppm", argv[3], serial);
            if (canvas_write_ppm(c, path)) { rc = 2; break; }
            noveledit_debug(app_top(a), state, 1 << 20);
            printf(",\"state\":%s", state);
        }
        printf("}\n");
    }
    app_destroy(a);
    free(a); free(c); free(state);
    return rc;
}
