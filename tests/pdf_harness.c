// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Isolated stage 3 entrance (Linux, libmupdf): pdfview plus the App parts.
 *   pdf-harness FILE.pdf SCRIPT PREFIX
 * The script is the headless key language. Unlike fdtp --headless, ticks
 * run only at WAIT, so a SNAP straight after a key shows 描画中.
 * Prints per SNAP "SNAP n depth|row0|row22|row24|row23" (row 22: the bottom
 * line of the screen's frame, revision 1 item 29; then an error box's
 * message, and " INPUT label|text" while an input box is open), per tick
 * "TICK sec", per call of the PDF menu (item 39) "PDFTOOLS ...", and at the
 * end "HISTORY n" (the input boxes' remembered answers) and "VMHWM kB" from
 * /proc/self/status. FDTP_HARNESS_SIZE=COLSxROWS (85x25) sets the grid on
 * the Pomera's 1024 x 600 canvas.
 *   pdf-harness --annots FILE.pdf [PASSWORD]
 * Item 40: prints "PAGES n", then per annotation "ANNOT page subtype x0 y0
 * x1 y1 nquads [qx0 qy0 qx1 qy1]...|contents" (fz page space, contents in
 * UTF-8), and "PERMISSION annotate 0|1".
 *   pdf-harness --glyphs FILE
 * Prints "MISSING U+XXXX" for each character of the UTF-8 file that is not
 * in the font, then "GLYPHS n" (the characters checked). */
#include "pdfview.h"
#include "app.h"
#include "font24.h"
#include "pdftools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include "config.h"
/* The App parts reference these; the harness links no filer or config. */
void filer_reload(Screen *s,App *a) { (void)s; (void)a; }
void filer_apply_settings(Screen *s,App *a) { (void)s; (void)a; }
void config_free(App *a) { (void)a; }
int config_startup(App *a) { (void)a; return 0; }
/* The PDF menu (W3's pdftools.c) is not linked: the call is shown. */
int pdftools_open(App *a,const FileSelection *s,PdfToolsEntry first,int page)
{
    printf("PDFTOOLS first=%d page=%d dir=%s current=%s count=%zu name=%s\n",(int)first,page,s->directory,s->current,s->count,s->count?s->names[0]:"");
    app_notice(a,"PDF のメニューはまだ作っていません"); return -1;
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void put(uint32_t u)
{
    char b[4]; int n;
    if(u<0x80) { b[0]=(char)u; n=1; }
    else if(u<0x800) { b[0]=(char)(0xc0|u>>6); b[1]=(char)(0x80|(u&63)); n=2; }
    else if(u<0x10000) { b[0]=(char)(0xe0|u>>12); b[1]=(char)(0x80|(u>>6&63)); b[2]=(char)(0x80|(u&63)); n=3; }
    else { b[0]=(char)(0xf0|u>>18); b[1]=(char)(0x80|(u>>12&63)); b[2]=(char)(0x80|(u>>6&63)); b[3]=(char)(0x80|(u&63)); n=4; }
    fwrite(b,1,(size_t)n,stdout);
}
static void row(const Cells *c,int y)
{
    for(int x=0;x<FD_COLS;x++) {
        const Cell *v=&c->data[y][x]; if(v->part==2) continue;
        put(v->code?v->code:' ');
    }
}
static long hwm(void)
{
    FILE *f=fopen("/proc/self/status","r"); char line[256]; long kb=-1;
    if(!f) return -1;
    while(fgets(line,sizeof(line),f)) if(!strncmp(line,"VmHWM:",6)) kb=atol(line+6);
    fclose(f); return kb;
}
static void quiet(void *user,const char *message) { (void)user; (void)message; }
static int annots(const char *path,const char *password)
{
    fz_context *ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT); int rc=0;
    if(!ctx) return 2;
    fz_set_warning_callback(ctx,quiet,NULL); fz_set_error_callback(ctx,quiet,NULL);
    fz_document *volatile doc=NULL; fz_page *volatile page=NULL;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc=fz_open_document(ctx,path);
        if(fz_needs_password(ctx,doc) && !fz_authenticate_password(ctx,doc,password?password:"")) fz_throw(ctx,FZ_ERROR_GENERIC,"password");
        int n=fz_count_pages(ctx,doc); printf("PAGES %d\n",n);
        printf("PERMISSION annotate %d\n",fz_has_permission(ctx,doc,FZ_PERMISSION_ANNOTATE));
        for(int i=0;i<n;i++) {
            page=fz_load_page(ctx,doc,i);
            pdf_page *pp=pdf_page_from_fz_page(ctx,page);
            for(pdf_annot *an=pp?pdf_first_annot(ctx,pp):NULL;an;an=pdf_next_annot(ctx,an)) {
                fz_rect r=pdf_bound_annot(ctx,an);
                int q=pdf_annot_has_quad_points(ctx,an)?pdf_annot_quad_point_count(ctx,an):0;
                printf("ANNOT %d %s %.1f %.1f %.1f %.1f %d",i,pdf_string_from_annot_type(ctx,pdf_annot_type(ctx,an)),r.x0,r.y0,r.x1,r.y1,q);
                for(int k=0;k<q;k++) { fz_rect b=fz_rect_from_quad(pdf_annot_quad_point(ctx,an,k)); printf(" %.1f %.1f %.1f %.1f",b.x0,b.y0,b.x1,b.y1); }
                const char *s=pdf_annot_contents(ctx,an); printf("|%s\n",s?s:"");
            }
            fz_drop_page(ctx,page); page=NULL;
        }
    }
    fz_always(ctx) { fz_drop_page(ctx,page); fz_drop_document(ctx,doc); }
    fz_catch(ctx) { printf("ERROR %s\n",fz_caught_message(ctx)); rc=3; }
    fz_drop_context(ctx); return rc;
}
static int glyphs(const char *path)
{
    FILE *f=fopen(path,"rb"); if(!f) return 2;
    static char text[1<<20]; size_t n=fread(text,1,sizeof(text)-1,f); fclose(f); text[n]=0;
    const char *p=text; int count=0;
    while(*p) {
        uint32_t c=font24_decode(&p);
        if(c<0x20) continue;
        count++;
        if(font24_lookup(c)->code!=c) printf("MISSING U+%04X\n",(unsigned)c);
    }
    printf("GLYPHS %d\n",count); return 0;
}
int main(int argc,char **argv)
{
    if(argc>=3 && !strcmp(argv[1],"--annots")) return annots(argv[2],argc>3?argv[3]:NULL);
    if(argc==3 && !strcmp(argv[1],"--glyphs")) return glyphs(argv[2]);
    if(argc!=4) return 2;
    const char *size=getenv("FDTP_HARNESS_SIZE"); int cols,rows;
    if(size && screen_parse(size,&cols,&rows)==0) screen_apply(screen_choose(cols,rows,0,0));
    FILE *f=fopen(argv[2],"rb"); if(!f) return 2;
    char text[65536]; size_t n=fread(text,1,sizeof(text)-1,f); fclose(f); text[n]=0;
    widgets_history_home(0);   /* the history stays in memory (never ~/.fdtp/history) */
    App a; app_init(&a); double t0=mono(); Screen *s=pdfview_create(&a,argv[1]);
    printf("OPEN %.3f\n",mono()-t0);
    if(!s) { printf("NOTICE %s\nVMHWM %ld\n",a.widgets.label,hwm()); app_destroy(&a); return 3; }
    app_push(&a,s); Canvas *c=malloc(sizeof(*c)); Cells cells; cells_init(&cells);
    if(!c) { app_destroy(&a); return 2; } canvas_init(c);
    KeyScript script; keys_script_init(&script,text); unsigned serial=0; int rc=0;
    for(;;) {
        ScriptEvent e=keys_script_next(&script);
        if(e.kind==SCRIPT_END) break;
        if(e.kind==SCRIPT_ERROR) { fprintf(stderr,"script: %s\n",script.error); rc=2; break; }
        if(e.kind==SCRIPT_WAIT) { double t=mono(); app_tick(&a,a.now+e.seconds); printf("TICK %.3f\n",mono()-t); continue; }
        if(e.kind==SCRIPT_KEY) { app_key(&a,&e.key); continue; }
        if(e.kind==SCRIPT_SNAP) {
            if(!app_top(&a)) { printf("SNAP %u 0||||\n",++serial); continue; }
            app_draw(&a,c,&cells); char path[4096]; snprintf(path,sizeof(path),"%s-%03u.ppm",argv[3],++serial);
            if(canvas_write_ppm(c,path)) { rc=2; break; }
            printf("SNAP %u %d|",serial,a.depth); row(&cells,0); putchar('|'); row(&cells,FRAME_BOTTOM); putchar('|');
            row(&cells,FD_BAND); putchar('|'); row(&cells,FD_LINE);
            /* An error box (revision 1 A) is not on row 23: its message follows. */
            if(a.widgets.kind==WIDGET_ERROR) printf(" %s",a.widgets.label);
            if(a.widgets.kind==WIDGET_INPUT) printf(" INPUT %s|%s",a.widgets.label,a.widgets.text);
            if(a.widgets.kind==WIDGET_CONFIRM || a.widgets.kind==WIDGET_CHOICE) printf(" ASK %s",a.widgets.label);
            if(a.widgets.kind==WIDGET_INFO) printf(" INFO %s|%s",a.widgets.label,a.widgets.info_count?a.widgets.info_value[0]:"");
            if(a.widgets.kind==WIDGET_HELP) printf(" HELP %s",a.widgets.label);
            putchar('\n');
        }
    }
    printf("HISTORY %d\n",a.widgets.history_count);
    printf("VMHWM %ld\n",hwm());
    app_destroy(&a); free(c); return rc;
}
