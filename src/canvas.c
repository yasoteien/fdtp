// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "canvas.h"
#include "cells.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int canvas_w = 1024, canvas_h = 600, canvas_grid_x = 32;
void canvas_clear(Canvas *c, uint32_t rgb)
{
    size_t n=(size_t)c->w*(size_t)c->h;
    unsigned char *p=c->rgba;
    for (size_t i=0;i<n;++i,p+=4) { p[0]=rgb>>16; p[1]=rgb>>8; p[2]=rgb; p[3]=255; }
    c->full_dirty=1;
}
void canvas_init(Canvas *c) { c->w=canvas_w; c->h=canvas_h; canvas_clear(c,0x000055); }
int canvas_fit(Canvas *c)
{
    if (c->w==canvas_w && c->h==canvas_h) return 0;
    canvas_init(c); return 1;
}
static int write_ppm(const Canvas *c, const char *path);
/* Pixel comparison of two builds (tools/pixel_compare.py): with
 * $FDTP_PPM_COPY set, every snapshot is also written there, named after its
 * path and how many times that path was written before. */
static void copy_for_compare(const Canvas *c, const char *path)
{
    const char *dir=getenv("FDTP_PPM_COPY");
    if (!dir || !*dir) return;
    char name[1024], full[2048]; size_t n=0;
    for (const char *p=path; *p && n+1<sizeof(name); ++p) name[n++]=*p=='/' ? '_' : *p;
    name[n]=0;
    for (int i=0;i<10000;++i) {
        snprintf(full,sizeof(full),"%s/%s.%d",dir,name,i);
        if (access(full,F_OK)) { write_ppm(c,full); return; }
    }
}
int canvas_write_ppm(const Canvas *c, const char *path)
{
    int result=write_ppm(c,path);
    if (!result) copy_for_compare(c,path);
    return result;
}
static int write_ppm(const Canvas *c, const char *path)
{
    FILE *f=fopen(path,"wb");
    if (!f) return -1;
    int failed=fprintf(f,"P6\n%d %d\n255\n",c->w,c->h)<0;
    for (int y=0;y<c->h && !failed;++y) for (int x=0;x<c->w;++x)
        if (fwrite(CANVAS_PX(c,x,y),1,3,f)!=3) { failed=1; break; }
    if (fclose(f)) failed=1;
    return failed ? -1 : 0;
}

/* ---- the screen size ------------------------------------------------------ */
int screen_parse(const char *text, int *cols, int *rows)
{
    if (!text) return -1;
    while (*text==' ' || *text=='\t') ++text;
    if (!strncmp(text,"auto",4) && (!text[4] || text[4]==' ' || text[4]=='\t')) { *cols=*rows=0; return 0; }
    char *end; long c=strtol(text,&end,10);
    if (end==text || (*end!='x' && *end!='X')) return -1;
    const char *r=end+1; long n=strtol(r,&end,10);
    if (end==r) return -1;
    while (*end==' ' || *end=='\t') ++end;
    if (*end || c<1 || n<1) return -1;
    *cols=c<80 ? 80 : c>FD_COLS_MAX ? FD_COLS_MAX : (int)c;
    *rows=n<25 ? 25 : n>FD_ROWS_MAX ? FD_ROWS_MAX : (int)n;
    return 0;
}
static int clamp(int v, int lo, int hi) { return v<lo ? lo : v>hi ? hi : v; }
ScreenSize screen_choose(int cols, int rows, int out_w, int out_h)
{
    const ScreenSize pomera={80,25,1024,600};
    if (cols>0 && rows>0) {
        cols=clamp(cols,80,FD_COLS_MAX); rows=clamp(rows,25,FD_ROWS_MAX);
        /* 80..85 x 25 fit the Pomera's 1024 x 600: drawn there unscaled. */
        if (cols*12<=1024 && rows*24<=600) return (ScreenSize){cols,rows,1024,600};
        return (ScreenSize){cols,rows,cols*12,rows*24};
    }
    /* No display (the headless runs of the tests): the 80 x 25 screen. */
    if (out_w<=0 || out_h<=0) return pomera;
    /* The Pomera's panel, and anything no larger (its own X window, scaled
     * to fit): 85 x 25, so the 2 x 32 dots beside the 80 columns are used
     * too (revision 1 item 21; 1020 x 600 leaves 2 dots on each side). */
    if (out_w<=1024 && out_h<=600) return (ScreenSize){85,25,1024,600};
    /* One step of scale for every 1080 rows (a 4K screen draws 1920 x 1080
     * twice over), kept at least 960 x 600 and at most the largest canvas. */
    int k=out_h/1080; if (k<1) k=1;
    while (k>1 && (out_w/k<960 || out_h/k<600)) --k;
    while (out_w/k>CANVAS_W_MAX || out_h/k>CANVAS_H_MAX) ++k;
    int lw=out_w/k, lh=out_h/k;
    cols=clamp(lw/12,80,FD_COLS_MAX); rows=clamp(lh/24,25,FD_ROWS_MAX);
    int cw=lw>cols*12 ? lw : cols*12, ch=lh>rows*24 ? lh : rows*24;
    return (ScreenSize){cols,rows,cw,ch};
}
void screen_apply(ScreenSize s)
{
    fd_set_size(s.cols,s.rows);
    canvas_w=s.canvas_w<FD_W ? FD_W : s.canvas_w>CANVAS_W_MAX ? CANVAS_W_MAX : s.canvas_w;
    canvas_h=s.canvas_h<FD_H ? FD_H : s.canvas_h>CANVAS_H_MAX ? CANVAS_H_MAX : s.canvas_h;
    canvas_grid_x=(canvas_w-FD_W)/2;
}
