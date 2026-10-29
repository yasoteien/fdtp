// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
#include "version.h"
#include "fdscreen.h"
#include "font24.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
static void text(Cells *c, int x, int y, const char *s, int color, int n)
{ cells_text(c,x,y,s,color,BG,n); }
static void number(char out[32], uint64_t value)
{
    char raw[32];
    snprintf(raw,sizeof(raw),"%" PRIu64,value);
    size_t n = strlen(raw), j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i && (n-i)%3 == 0) out[j++] = ',';
        out[j++] = raw[i];
    }
    out[j] = 0;
}
void fdscreen_number(Cells *c, int end, int row, uint64_t value, int min_col)
{
    /* Too wide for its field (from min_col to end): show it in K, M or G
     * (units of 1024, rounded down) instead of running into the label. */
    static const char units[] = "KMG";
    char s[32]; number(s,value);
    int width = (int)strlen(s);
    for (int u = 0; width > end-min_col+1 && u < 3; ++u) {
        value /= 1024;
        number(s,value);
        size_t n = strlen(s);
        s[n] = units[u]; s[n+1] = 0;
        width = (int)n+1;
    }
    text(c,end-width+1,row,s,WHITE,width);
}
const char *fdscreen_program = "FDTP";
int fdscreen_slots(int columns)
{
    switch (columns) {
    case 1: return 1;
    case 3: return FD_COLS / 26;
    case 5: return (FD_COLS + 1) / 16;
    default: return FD_COLS / 40;
    }
}
int fdscreen_slot_width(int columns)
{
    return columns == 5 ? 16 : FD_COLS / fdscreen_slots(columns);
}
int fdscreen_slot_x(int columns, int slot) { return slot * fdscreen_slot_width(columns); }
void fdscreen_build(Cells *cells, const DirList *list, size_t cursor, int columns, time_t now)
{
    Cells next;
    cells_init(&next);
    /* Extra columns (0 on the Pomera): the title moves half of them right,
     * the date and the numbers on the right all of them; the fields between
     * the row 1 dividers, the path and the Info name widen. */
    const int X = FD_XCOLS;
    char title[64], name[32];
    snprintf(name,sizeof(name),"%.4s Version " FDTP_VERSION,fdscreen_program);
    snprintf(title,sizeof(title)," << %s >> ",name);
    int title_x = 35 + X/2 - cells_width(title)/2;
    next.title_start = title_x; next.title_end = title_x + cells_width(title);
    next.date_start = 60 + X; next.date_end = 76 + X;
    text(&next,title_x,0,title,WHITE,40);
    text(&next,title_x+4,0,name,YELLOW,17);
    struct tm *tm = localtime(&now);
    char s[128];
    if (tm) strftime(s,sizeof(s)," %y-%m-%d %H:%M ",tm); else strcpy(s," -- -- -- --:-- ");
    text(&next,60+X,0,s,YELLOW,16);
    if (columns != 1 && columns != 3 && columns != 5) columns = 2;
    const size_t per = (size_t)FDSCREEN_ROWS*fdscreen_slots(columns);
    size_t page = cursor/per;
    snprintf(s,sizeof(s),"Page:%2zu/%2zu",page+1,list->count ? (list->count+per-1)/per : 1);
    text(&next,2,1,s,CYAN,11);
    const int d0 = fd_divider(0), d1 = fd_divider(1), d2 = fd_divider(2);
    text(&next,d0+2,1,"Total:",CYAN,6); fdscreen_number(&next,d1-2,1,list->total,d0+9);
    text(&next,d1+2,1,"Used:",CYAN,5); fdscreen_number(&next,d2-2,1,list->used,d1+8);
    text(&next,d2+2,1,"Free:",CYAN,5); fdscreen_number(&next,FD_COLS-3,1,list->free_bytes,d2+8);
    text(&next,2,3,"Path=",CYAN,5);
    const char *path = list->path ? list->path : "";
    /* ~ for $HOME, "…" and the end when too wide (cells_path). */
    cells_path(&next,7,3,path,WHITE,BG,44+X);
    fdscreen_number(&next,57+X,3,0,52+X); text(&next,58+X,3,"Marked",CYAN,6); fdscreen_number(&next,77+X,3,0,64+X);
    text(&next,2,4,"Info:",CYAN,5);
    if (cursor < list->count) {
        const DirEntry *e = &list->entries[cursor];
        text(&next,7,4,e->name,WHITE,17+X);
        if (e->is_dir) text(&next,26+X,4,"<DIR>",WHITE,5);
        else fdscreen_number(&next,30+X,4,e->size,24+X);
        tm = localtime(&e->mtime);
        if (tm) {
            strftime(s,sizeof(s),"%y-%m-%d",tm); text(&next,32+X,4,s,WHITE,8);
            snprintf(s,sizeof(s),"%2d:%02d",tm->tm_hour,tm->tm_min); text(&next,41+X,4,s,WHITE,5);
        }
        int flags[] = {e->readonly,e->hidden,e->special,e->archive};
        const char *labels = "RHSA";
        for (int i=0;i<4;++i) cells_put(&next,47+X+2*i,4,flags[i] ? labels[i] : '_',flags[i] ? YELLOW : WHITE,BG);
    }
    fdscreen_number(&next,57+X,4,list->file_count,55+X); text(&next,58+X,4,"Files",CYAN,5);
    fdscreen_number(&next,77+X,4,list->bytes,63+X);
    for (size_t i=page*per; i<list->count && i<(page+1)*per; ++i) {
        int slot=(int)((i%per)/FDSCREEN_ROWS), y=6+(int)(i%FDSCREEN_ROWS);
        fdscreen_entry(&next,&list->entries[i],i==cursor,columns,slot,y);
    }
    const char *bands[] = {"Logdsk"," eXec "," Copy ","Delete","Rename"," Sort "," Find "," Tree ","Editor","Unpack"};
    int hot[] = {0,2,1,0,0,1,1,1,0,0};
    for (int k=0;k<10;++k) cells_band_item(&next,k<5 ? 4+7*k : 42+7*(k-5),6,bands[k],hot[k]);
    /* Diff the finished screen, rather than the intermediate clear/rebuild. */
    for (int y=0;y<FD_ROWS;++y) for (int x=0;x<FD_COLS;++x) {
        Cell a=cells->data[y][x], b=next.data[y][x];
        next.dirty[y][x] = cells->dirty[y][x] || a.code!=b.code || a.fg!=b.fg || a.bg!=b.bg || a.part!=b.part;
    }
    next.lines_dirty = cells->lines_dirty || cells->title_start!=next.title_start ||
        cells->title_end!=next.title_end || cells->date_start!=next.date_start || cells->date_end!=next.date_end;
    cells_copy(cells,&next);
}

void fdscreen_entry(Cells *c,const DirEntry *e,int selected,int columns,int slot,int y)
{

    int color=e->hidden ? MAGENTA : e->is_dir ? CYAN : YELLOW;
    int fg=selected ? BG : color, bg=selected ? color : BG;
    if(columns==1 || columns==3) {
        /* 1: one list across, the name 40 columns plus the extra ones;
         * 3: lists of 26 or more, the size at the right of each. */
        const int X=columns==1 ? FD_XCOLS : 0;
        int b=fdscreen_slot_x(columns,slot), end=columns==1 ? 78+X : b+fdscreen_slot_width(columns)-2;
        for(int j=b+2;j<=end;++j) cells_put(c,j,y,' ',fg,bg);
        cells_text(c,b+2,y,columns==1 ? e->name : e->display,fg,bg,columns==1 ? 40+X : 13);
        char s[64];
        if(e->is_dir) strcpy(s,"<DIR>");
        else snprintf(s,sizeof(s),"%" PRIu64,e->size);
        int right=columns==1 ? 52+X : end;
        int max=columns==1 ? 10 : 7;
        if((int)strlen(s)>max) snprintf(s,sizeof(s),"%" PRIu64 "K",e->size/1024);
        cells_text(c,right-(int)strlen(s)+1,y,s,fg,bg,max);
        if(columns==1) {
            struct tm *mt=localtime(&e->mtime);
            if(mt) { strftime(s,sizeof(s),"%y-%m-%d",mt); cells_text(c,54+X,y,s,fg,bg,8);
                snprintf(s,sizeof(s),"%2d:%02d",mt->tm_hour,mt->tm_min); cells_text(c,63+X,y,s,fg,bg,5); }
            char mode[11]="----------";
            mode[0]=e->is_link ? 'l' : e->is_dir ? 'd' : S_ISSOCK(e->mode) ? 's' :
                S_ISFIFO(e->mode) ? 'p' : S_ISCHR(e->mode) ? 'c' : S_ISBLK(e->mode) ? 'b' : '-';
            const char *bits="rwxrwxrwx";
            for(int j=0;j<9;++j) if(e->mode & (1ul<<(8-j))) mode[j+1]=bits[j];
            if(e->mode & 04000) mode[3]=(e->mode & 0100) ? 's' : 'S';
            if(e->mode & 02000) mode[6]=(e->mode & 0010) ? 's' : 'S';
            if(e->mode & 01000) mode[9]=(e->mode & 0001) ? 't' : 'T';
            cells_text(c,69+X,y,mode,fg,bg,10);
        }
        return;
    }
        if (columns == 5) {
            int x = 2+slot*16;
            for (int j=0;j<13;++j) cells_put(c,x+j,y,' ',fg,bg);
            cells_text(c,x,y,e->display,fg,bg,13);
            return;
        }
        /* FD's two columns of 40: name 8.4 at +2 (13 cells), size (or <DIR>) ending at
         * +22, date at +25, time at +34; the cursor bar spans +2..+38. A wider
         * list (WIDE_NOTES.md) puts its extra columns E before the size. */
        int b = fdscreen_slot_x(2,slot), E = fdscreen_slot_width(2)-40;
        for (int j=2;j<=38+E;++j) cells_put(c,b+j,y,' ',fg,bg);
        cells_text(c,b+2,y,e->display,fg,bg,13);
        b += E;
        if (e->is_dir) cells_text(c,b+18,y,"<DIR>",fg,bg,5);
        else {
            char size[32];
            snprintf(size,sizeof(size),"%" PRIu64,(uint64_t)e->size);
            int w = (int)strlen(size);
            /* 7 digits keep a gap after the 13-cell name; larger sizes in K. */
            if (w > 7) { snprintf(size,sizeof(size),"%" PRIu64 "K",(uint64_t)e->size/1024); w = (int)strlen(size); }
            cells_text(c,b+23-w,y,size,fg,bg,w);
        }
        struct tm *mt = localtime(&e->mtime);
        if (mt) {
            char d[16];
            strftime(d,sizeof(d),"%y-%m-%d",mt); cells_text(c,b+25,y,d,fg,bg,8);
            snprintf(d,sizeof(d),"%2d:%02d",mt->tm_hour,mt->tm_min); cells_text(c,b+34,y,d,fg,bg,5);
        }
}
