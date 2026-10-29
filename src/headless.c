// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "headless.h"
#include "filer.h"
#include "fileops.h"
#include "config.h"
#include "pomera_notice.h"
#include "raster.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int json_string(FILE *f,const char *s)
{
    if(fputc('"',f)==EOF) return -1;
    for(const unsigned char *p=(const unsigned char *)s;*p;++p) {
        if(*p=='"' || *p=='\\') { if(fputc('\\',f)==EOF || fputc(*p,f)==EOF) return -1; }
        else if(*p<32) { if(fprintf(f,"\\u%04x",*p)<0) return -1; }
        else if(fputc(*p,f)==EOF) return -1;
    }
    return fputc('"',f)==EOF ? -1 : 0;
}
static int write_cells(App *a,const Cells *c,const char *path)
{
    FILE *f=fopen(path,"w"); if(!f) return -1;
    fprintf(f,"{\"rows\":[\n");
    for(int y=0;y<FD_ROWS;++y) {
        fputc('[',f);
        for(int x=0;x<FD_COLS;++x) { Cell v=c->data[y][x]; fprintf(f,"%s[%u,%u,%u,%u]",x ? "," : "",v.code,v.fg,v.bg,v.part); }
        fprintf(f,"]%s\n",y+1<FD_ROWS ? "," : "");
    }
    fprintf(f,"],\"boxes\":[");
    for(int i=0;i<c->box_count;++i) { const CellBox *b=&c->boxes[i]; fprintf(f,"%s[%d,%d,%d,%d,%d,%d]",i ? "," : "",b->col,b->row,b->w,b->h,b->rule,b->kind); }
    /* The gaps in each box's top and bottom lines, the lines inside the boxes
     * (cells.h CellBox, CellLine): [at, from, to, vertical, box]. */
    fprintf(f,"],\"gaps\":[");
    for(int i=0;i<c->box_count;++i) { const CellBox *b=&c->boxes[i]; fprintf(f,"%s[%d,%d,%d,%d]",i ? "," : "",b->top_from,b->top_to,b->bottom_from,b->bottom_to); }
    fprintf(f,"],\"lines\":[");
    for(int i=0;i<c->line_count;++i) { const CellLine *l=&c->lines[i]; fprintf(f,"%s[%d,%d,%d,%d,%d]",i ? "," : "",l->at,l->from,l->to,l->vertical,l->box); }
    char separators[64]; cells_separator_text(c,separators,sizeof(separators));
    fprintf(f,"],\"widget\":%d,\"quit\":%s,\"depth\":%d,\"separators\":%s,\"external\":{\"pending\":%s,\"flags\":%u,\"cwd\":",(int)a->widgets.kind,a->quit ? "true" : "false",a->depth,separators,a->external.pending ? "true" : "false",a->external.flags);
    json_string(f,a->external.cwd ? a->external.cwd : ""); fprintf(f,",\"argv\":[");
    for(int i=0;a->external.argv[i];++i) { if(i) fputc(',',f); json_string(f,a->external.argv[i]); }
    fprintf(f,"]}}\n");
    int failed=ferror(f); if(fclose(f)) failed=1; return failed ? -1 : 0;
}
int headless_snapshot(App *a,Canvas *c,Cells *cells,const char *prefix,unsigned serial)
{
    if(a->changed) app_draw(a,c,cells);
    Screen *s=app_top(a);
    if(!s || !s->pixel) { FDRect rects[FD_ROWS_MAX]; raster_update(c,cells,rects); }
    size_t n=strlen(prefix)+40; char *path=malloc(n); if(!path) return -1;
    snprintf(path,n,"%s-%03u.ppm",prefix,serial);
    int result=canvas_write_ppm(c,path);
    if(!result) { snprintf(path,n,"%s-%03u.cells",prefix,serial); result=write_cells(a,cells,path); }
    free(path); return result;
}
void headless_script_init(ScriptRunner *r,const char *text,const char *prefix,double now)
{
    memset(r,0,sizeof(*r)); keys_script_init(&r->parser,text); r->ready_at=now; r->prefix=prefix;
    r->size_cols=r->size_rows=-1;
}
void headless_fit(ScriptRunner *r,App *a,Canvas *c,Cells *cells)
{
    ScreenSize want=app_screen_choice(a,r->size_cols,r->size_rows,r->display_w,r->display_h);
    if(!app_screen_differs(want)) return;
    screen_apply(want); canvas_fit(c); cells_invalidate(cells); a->changed=1;
}
_Static_assert(FILER_POINT==0 && FILER_SELECT==1 && FILER_TOGGLE==2 && FILER_RANGE==3, "CLICK: in keys.c");
int headless_script_step(ScriptRunner *r,App *a,Canvas *c,Cells *cells,double now)
{
    if(r->ended) return 2;
    if(now<r->ready_at) return 0;
    ScriptEvent e=keys_script_next(&r->parser);
    switch(e.kind) {
    case SCRIPT_KEY:
        /* A prior headless external request remains inspectable until the next
         * key. Linux executes/clears it before calling this function again. */
        if(a->external.pending) app_external_done(a,0);
        if(a->quit) { fprintf(stderr,"script: key after application quit\n"); return -1; }
        app_key(a,&e.key); return 1;
    case SCRIPT_SNAP:
        if(headless_snapshot(a,c,cells,r->prefix,++r->snapshots)<0) { perror(r->prefix); return -1; }
        return 1;
    case SCRIPT_MODS: app_modifiers(a,e.mods); return 1;
    case SCRIPT_DISPLAY: r->display_w=e.width; r->display_h=e.height; return 1;
    case SCRIPT_CLICK: filer_mouse(a->depth ? a->screens[0] : NULL,a,e.width,e.height,e.mods); return 1;
    case SCRIPT_WAIT: r->ready_at=now+e.seconds; return 0;
    case SCRIPT_REALWAIT: {
        struct timespec start, current;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            app_tick(a, now);
            clock_gettime(CLOCK_MONOTONIC, &current);
            double elapsed = current.tv_sec - start.tv_sec + (current.tv_nsec - start.tv_nsec) / 1e9;
            if (elapsed >= e.seconds) break;
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        return 1;
    }
    case SCRIPT_END: r->ended=1; return 2;
    default: fprintf(stderr,"script: %s\n",r->parser.error); return -1;
    }
}
static char *read_script(const char *path)
{
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    size_t size=0,cap=4096; char *text=malloc(cap);
    if(!text) { fclose(f); return NULL; }
    for(;;) {
        if(size==cap-1) { if(cap>16*1024*1024) { free(text); fclose(f); errno=EFBIG; return NULL; } cap*=2; char *p=realloc(text,cap); if(!p) { free(text); fclose(f); return NULL; } text=p; }
        size_t n=fread(text+size,1,cap-size-1,f); size+=n;
        if(!n) break;
    }
    int failed=ferror(f); if(fclose(f)) failed=1;
    if(failed) { free(text); return NULL; } text[size]=0; return text;
}
static int pixels(const char *text,int *w,int *h)
{
    char *end; long a=strtol(text,&end,10),b=0;
    if(end==text || (*end!='x' && *end!='X')) return -1;
    const char *r=end+1; b=strtol(r,&end,10);
    if(end==r || *end || a<1 || b<1 || a>16384 || b>16384) return -1;
    *w=(int)a; *h=(int)b; return 0;
}
int headless_main(int argc,char **argv)
{
    const char *directory=".",*output=NULL,*prefix="fdtp-snap",*script=NULL,*script_path=NULL;
    if(getenv("FDTP_SNAPSHOT_PREFIX")) prefix=getenv("FDTP_SNAPSHOT_PREFIX");
    int columns=2,have_dir=0,size_cols=-1,size_rows=-1,display_w=0,display_h=0;
    double now=(double)time(NULL); char *owned=NULL;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--headless")) continue;
        if(!strcmp(argv[i],"--keys") || !strcmp(argv[i],"--script") || !strcmp(argv[i],"--out") || !strcmp(argv[i],"--time") || !strcmp(argv[i],"--columns") ||
           !strcmp(argv[i],"--size") || !strcmp(argv[i],"--display")) {
            const char *flag=argv[i]; if(++i>=argc) goto usage;
            if(!strcmp(flag,"--keys")) { if(script || script_path) goto usage; script=argv[i]; }
            else if(!strcmp(flag,"--script")) { if(script || script_path) goto usage; script_path=argv[i]; }
            else if(!strcmp(flag,"--out")) prefix=argv[i];
            else if(!strcmp(flag,"--columns")) { char *end; long n=strtol(argv[i],&end,10); if(*end || (n!=1 && n!=2 && n!=3 && n!=5)) goto usage; columns=(int)n; }
            else if(!strcmp(flag,"--size")) { if(screen_parse(argv[i],&size_cols,&size_rows)) goto usage; }
            else if(!strcmp(flag,"--display")) { if(pixels(argv[i],&display_w,&display_h)) goto usage; }
            else { char *end; errno=0; now=strtod(argv[i],&end); if(end==argv[i] || *end || errno || !isfinite(now) || now<0 || now>253402300799.0) goto usage; }
        } else if(argv[i][0]=='-') goto usage;
        else if(!have_dir) { directory=argv[i]; have_dir=1; }
        else if(!output) output=argv[i];
        else { if(i+1!=argc || (strcmp(argv[i],"1") && strcmp(argv[i],"2") && strcmp(argv[i],"3") && strcmp(argv[i],"5"))) goto usage; columns=atoi(argv[i]); }
    }
    if(script_path) { owned=read_script(script_path); if(!owned) { perror(script_path); return 1; } script=owned; }
    if(!script) script=getenv("FDTP_TEST_KEYS");
    /* A test run keeps input history in memory unless $FDTP_HISTORY names a file. */
    widgets_history_home(0);
    App a; app_init(&a); fileops_tree_install(&a);
    /* The size first (SCREENSIZE is read by app_init): the filer lays out
     * its panes for it. */
    ScriptRunner runner; headless_script_init(&runner,script ? script : "",prefix,now);
    runner.size_cols=size_cols; runner.size_rows=size_rows; runner.display_w=display_w; runner.display_h=display_h;
    screen_apply(app_screen_choice(&a,size_cols,size_rows,display_w,display_h));
    Screen *s=filer_create(directory);
    if(!s) { perror(directory); app_destroy(&a); free(owned); return 1; }
    app_push(&a,s);
    config_first_run(&a, 1);
    pomera_notice_start(&a, 1);
    if(columns!=2) { Key key={KCHAR,(uint32_t)('0'+columns),0}; app_key(&a,&key); }
    Canvas *canvas=malloc(sizeof(*canvas)); Cells cells; cells_init(&cells);
    if(!canvas) { app_destroy(&a); free(owned); return 1; }
    canvas_init(canvas); app_tick(&a,now); int result=0;
    if(script) {
        for(;;) {
            app_tick(&a,now); headless_fit(&runner,&a,canvas,&cells);
            int status=headless_script_step(&runner,&a,canvas,&cells,now);
            if(status<0) { result=1; break; }
            if(status==2) break;
            if(status==0) now=runner.ready_at;
        }
    }
    if(!result && (output || !script)) {
        headless_fit(&runner,&a,canvas,&cells);
        if(a.changed) app_draw(&a,canvas,&cells);
        Screen *top=app_top(&a);
        if(!top || !top->pixel) { FDRect rects[FD_ROWS_MAX]; raster_update(canvas,&cells,rects); }
        if(canvas_write_ppm(canvas,output ? output : "preview.ppm")<0) { perror(output ? output : "preview.ppm"); result=1; }
    }
    free(canvas); app_destroy(&a); free(owned); return result;
usage:
    fprintf(stderr,"usage: %s [--headless] [--script FILE|--keys TEXT] [--out PREFIX] [--time EPOCH] [--columns N] [--size auto|COLSxROWS] [--display WxH] [DIRECTORY [OUTPUT.ppm [COLUMNS]]]\n",argv[0]); return 2;
}
