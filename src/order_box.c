// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "order_box.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Screen screen, *below;
    char *title, **names;
    size_t count, selected, top;
    OrderBoxDone done;
    void *context;
} Order;
static size_t room(void) { return (size_t)(FD_ROWS - 8); }
static void destroy(Screen *s)
{
    Order *o=s->self;
    for(size_t i=0;i<o->count;++i) free(o->names[i]);
    free(o->names); free(o->title); free(o);
}
static int key(Screen *s,App *a,const Key *k)
{
    Order *o=s->self;
    if(k->key==KESC || k->key==KRET) {
        int accepted=k->key==KRET;
        OrderBoxDone done=o->done; void *context=o->context;
        char **names=o->names; size_t count=o->count;
        o->names=NULL; o->count=0;
        app_pop(a);
        if(done) done(a,context,accepted?(const char *const *)names:NULL,accepted?count:0,accepted);
        for(size_t i=0;i<count;++i) free(names[i]);
        free(names); return 0;
    }
    int up=(k->key==KCHAR && !k->mods && k->ch=='u') || (k->key==KUP && k->mods==MOD_SHIFT);
    int down=(k->key==KCHAR && !k->mods && k->ch=='d') || (k->key==KDOWN && k->mods==MOD_SHIFT);
    size_t next=o->selected;
    if((up || k->key==KUP) && next) --next;
    else if((down || k->key==KDOWN) && next+1<o->count) ++next;
    else if(k->key==KHOME) next=0;
    else if(k->key==KEND && o->count) next=o->count-1;
    else if(k->key==KPGUP) next=next>room()?next-room():0;
    else if(k->key==KPGDN && o->count) next=next+room()<o->count?next+room():o->count-1;
    if((up || down) && o->count) { char *p=o->names[next]; o->names[next]=o->names[o->selected]; o->names[o->selected]=p; }
    o->selected=next;
    if(next<o->top) o->top=next;
    if(next>=o->top+room()) o->top=next-room()+1;
    a->changed=1; return 0;
}
static void draw(Screen *s,App *a,Canvas *canvas,Cells *c)
{
    Order *o=s->self;
    if(o->below && o->below->draw) o->below->draw(o->below,a,canvas,c);
    size_t visible=o->count<room()?o->count:room();
    int w=FD_COLS-8, h=(int)visible+4, x=(FD_COLS-w)/2, y=(FD_ROWS-h)/2;
    int box=cells_box(c,x,y,w,h,CELL_BOX_FRAME|CELL_BOX_SHADOW);
    if(box<0) return;
    int tw=cells_text(c,x+3,y,o->title,YELLOW,BG,w-6);
    c->boxes[box].top_from=x+2; c->boxes[box].top_to=x+4+tw;
    for(size_t i=0;i<visible;++i) {
        int chosen=o->top+i==o->selected;
        for(int col=x+2;col<x+w-2;++col) cells_put(c,col,y+1+(int)i,' ',chosen?BG:WHITE,chosen?CYAN:BG);
        cells_text(c,x+2,y+1+(int)i,o->names[o->top+i],chosen?BG:WHITE,chosen?CYAN:BG,w-4);
    }
    cells_text(c,x+2,y+h-2,T("u:上へ d:下へ Enter:決定 Esc:やめる"),CYAN,BG,w-4);
    if(o->count>visible) {
        char mark[96]; snprintf(mark,sizeof(mark),"↑↓ %zu-%zu/%zu",o->top+1,o->top+visible,o->count);
        int mw=cells_width(mark), col=x+w-3-mw;
        if(col>=x+2) { c->boxes[box].bottom_from=col-1; c->boxes[box].bottom_to=col+mw+1; cells_text(c,col,y+h-1,mark,CYAN,BG,mw); }
    }
}
int order_box_open(App *a,const char *title,const char *const *names,size_t count,OrderBoxDone done,void *context)
{
    Order *o=calloc(1,sizeof(*o));
    if(!o) goto failed;
    o->title=strdup(title?title:T("連結するファイルと連結の順番"));
    o->names=calloc(count?count:1,sizeof(*o->names));
    if(!o->title || !o->names) goto cleanup;
    for(size_t i=0;i<count;++i) { o->names[i]=strdup(names[i]); if(!o->names[i]) goto cleanup; ++o->count; }
    o->below=app_top(a); o->done=done; o->context=context;
    o->screen=(Screen){o,key,draw,0,NULL,destroy};
    if(!app_push(a,&o->screen)) return 0;
cleanup: destroy(&(Screen){.self=o});
failed: app_notice(a,T("メモリが足りません")); return -1;
}
