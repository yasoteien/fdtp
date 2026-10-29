// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _XOPEN_SOURCE 700
#include "filer.h"
#include "version.h"
#include "app.h"
#include "fdscreen.h"
#include "font24.h"
#include "fileops.h"
#include "textview.h"
#include "hexview.h"
#include "archive.h"
#include "config.h"
#include "i18n.h"
#include "launcher.h"
#include "noveledit.h"
#include "sysinfo.h"
#include "splitjoin.h"
#include "pdftools.h"
#include "localsend_ui.h"
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#define WINDOWS 5
#define PATH_CAP 4096
#define DIRECTORY_HISTORY 16
typedef struct {
    DirList list;
    size_t *visible, count, cursor;
    int columns, rows, sort, descending, hide, type, links, shift;
    size_t anchor; /* Shift+click's other end (filer_mouse) */
    char filter[PATH_CAP], previous[PATH_CAP];
    char *stack[5]; int stacked;
    char *history[DIRECTORY_HISTORY]; int history_count;
} Window;
typedef enum { P_NONE, P_QUIT, P_PATH, P_HISTORY, P_MARK, P_FILTER, P_SORT, P_ORDER, P_SEARCH, P_NEWFILE } Pending;
typedef struct {
    Window windows[WINDOWS]; int count, active;
    Pending pending;
    int search_direction, sort_next;
    const char **selection; size_t selection_capacity;
} Filer;
static int filer_key(Screen *,App *,const Key *);
static Window *current(Filer *f) { return &f->windows[f->active]; }
static const char *cursor_name(Window *w)
{ return w->cursor<w->count ? w->list.entries[w->visible[w->cursor]].name : NULL; }
static DirEntry *cursor_entry(Window *w)
{ return w->cursor<w->count ? &w->list.entries[w->visible[w->cursor]] : NULL; }
static char *join(const char *directory,const char *name)
{
    if(!name) return NULL;
    if(name[0]=='/') return strdup(name);
    size_t n=strlen(directory)+strlen(name)+2; char *s=malloc(n);
    if(s) snprintf(s,n,"%s/%s",directory,name);
    return s;
}
static const char *extension(const char *name)
{ const char *p=strrchr(name,'.'); return p && p!=name ? p+1 : ""; }
static int compare(Window *w,size_t ai,size_t bi)
{
    DirEntry *a=&w->list.entries[ai],*b=&w->list.entries[bi];
    int ap=!strcmp(a->name,".."),bp=!strcmp(b->name,"..");
    if(ap!=bp) return bp-ap;
    if(a->is_dir!=b->is_dir) return b->is_dir-a->is_dir;
    int r=0;
    switch(w->sort) {
    case 1: r=strcmp(extension(a->name),extension(b->name)); break;
    case 2: r=(a->size>b->size)-(a->size<b->size); break;
    case 3: r=(a->mtime>b->mtime)-(a->mtime<b->mtime); break;
    case 4: r=cells_width(a->name)-cells_width(b->name); break;
    case 5: return (a->ordinal>b->ordinal)-(a->ordinal<b->ordinal);
    default: break;
    }
    if(!r) r=strcmp(a->name,b->name);
    return w->descending ? -r : r;
}
static void merge(Window *w,size_t *tmp,size_t lo,size_t hi)
{
    if(hi-lo<2) return;
    size_t mid=lo+(hi-lo)/2; merge(w,tmp,lo,mid); merge(w,tmp,mid,hi);
    size_t i=lo,j=mid,k=lo;
    while(i<mid || j<hi) tmp[k++]=(j==hi || (i<mid && compare(w,w->visible[i],w->visible[j])<=0)) ? w->visible[i++] : w->visible[j++];
    memcpy(w->visible+lo,tmp+lo,(hi-lo)*sizeof(*tmp));
}
static int rebuild(Window *w,const char *name)
{
    size_t capacity=w->list.count ? w->list.count : 1;
    size_t *v=malloc(capacity*sizeof(*v)),*tmp=malloc(capacity*sizeof(*tmp));
    if(!v || !tmp) { free(v); free(tmp); return -1; }
    size_t n=0;
    for(size_t i=0;i<w->list.count;++i) {
        DirEntry *e=&w->list.entries[i]; int parent=!strcmp(e->name,".."),stacked=0;
        for(int j=0;j<w->stacked;++j) if(!strcmp(e->name,w->stack[j])) stacked=1;
        if(stacked || (!parent && ((w->hide && e->hidden) || (*w->filter && fnmatch(w->filter,e->name,0))))) continue;
        v[n++]=i;
    }
    free(w->visible); w->visible=v; w->count=n;
    merge(w,tmp,0,n); free(tmp);
    if(name) {
        for(size_t i=0;i<n;++i) if(!strcmp(name,w->list.entries[v[i]].name)) { w->cursor=i; break; }
    }
    if(w->cursor>=n) w->cursor=n ? n-1 : 0;
    return 0;
}
static void window_free(Window *w)
{
    dirlist_free(&w->list); free(w->visible);
    for(int i=0;i<w->stacked;++i) free(w->stack[i]);
    for(int i=0;i<w->history_count;++i) free(w->history[i]);
    memset(w,0,sizeof(*w));
}
static void remember(Window *w,const char *path)
{
    if(w->history_count && !strcmp(w->history[w->history_count-1],path)) return;
    char *s=strdup(path); if(!s) return;
    if(w->history_count==DIRECTORY_HISTORY) {
        free(w->history[0]); memmove(w->history,w->history+1,(DIRECTORY_HISTORY-1)*sizeof(*w->history)); --w->history_count;
    }
    w->history[w->history_count++]=s;
}
static int window_init(Window *w,const char *path)
{
    memset(w,0,sizeof(*w)); w->columns=2; w->rows=FDSCREEN_ROWS;
    if(dirlist_read(&w->list,path)<0) return -1;
    if(rebuild(w,NULL)<0) { window_free(w); return -1; }
    remember(w,w->list.path); return 0;
}
/* Stage 1c: the ~/.fdtprc variables. FDclone's SORTTYPE numbers differ
 * from the pane's own (0 name ... 4 length, 5 unsorted). */
static void apply_settings(Window *w,const AppSettings *settings)
{
    int sort=settings->sorttype%100;
    w->columns=settings->displaymode==1 || settings->displaymode==3 || settings->displaymode==5 ? settings->displaymode : 2;
    w->sort=sort>=1 && sort<=5 ? sort-1 : 5; w->descending=w->sort!=5 && settings->sorttype>=100;
    w->hide=settings->dotfile!=0; w->links=settings->symlink!=0; w->type=settings->filetype!=0;
    const char *n=cursor_name(w); char *name=n ? strdup(n) : NULL;
    rebuild(w,name); free(name);
}
void filer_apply_settings(Screen *s,App *a)
{
    if(!s || s->key!=filer_key) return;
    Filer *f=s->self; for(int i=0;i<f->count;++i) apply_settings(&f->windows[i],&a->settings);
    a->changed=1;
}
static int go(Window *w,App *a,const char *path,int parent)
{
    char *full=NULL;
    if(!strcmp(path,"-")) { if(*w->previous) full=strdup(w->previous); }
    else if(path[0]=='~' && (path[1]=='/' || !path[1])) {
        const char *home=getenv("HOME"); if(home) full=join(home,path[1] ? path+2 : ".");
    } else full=join(w->list.path,path);
    if(!full) { app_notice(a,T("移動先がありません")); return -1; }
    DirList list={0};
    if(dirlist_read(&list,full)<0) { free(full); app_notice(a,T("ディレクトリを開けません")); return -1; }
    free(full);
    char old[PATH_CAP],child[PATH_CAP]; snprintf(old,sizeof(old),"%s",w->list.path);
    const char *base=strrchr(old,'/'); snprintf(child,sizeof(child),"%s",base ? base+1 : old);
    dirlist_free(&w->list); w->list=list;
    free(w->visible); w->visible=NULL; w->count=0;
    snprintf(w->previous,sizeof(w->previous),"%s",old);
    w->cursor=0; w->shift=0; w->filter[0]=0;
    for(int i=0;i<w->stacked;++i) free(w->stack[i]);
    w->stacked=0; remember(w,w->list.path);
    if(rebuild(w,parent ? child : NULL)<0) { app_notice(a,T("メモリが足りません")); return -1; }
    return 0;
}
static void reload_window(Window *w,App *a)
{
    const char *n=cursor_name(w); char *name=n ? strdup(n) : NULL;
    DirList next={0};
    if(dirlist_read(&next,w->list.path)<0) { free(name); app_notice(a,T("ディレクトリを読み直せません")); return; }
    for(size_t j=0;j<w->list.count;++j) if(w->list.entries[j].marked)
        for(size_t i=0;i<next.count;++i)
            if(!strcmp(next.entries[i].name,w->list.entries[j].name)) { next.entries[i].marked=1; break; }
    dirlist_free(&w->list); w->list=next;
    free(w->visible); w->visible=NULL; w->count=0;
    if(rebuild(w,name)<0) app_notice(a,T("メモリが足りません"));
    free(name);
}
void filer_reload(Screen *s,App *a)
{
    if(!s || s->key!=filer_key) return;
    Filer *f=s->self; for(int i=0;i<f->count;++i) reload_window(&f->windows[i],a);
}
int filer_marked(Screen *s)
{
    if(!s || s->key!=filer_key) return 0;
    Window *w=current(s->self);
    for(size_t i=0;i<w->list.count;++i) if(w->list.entries[i].marked) return 1;
    return 0;
}
const char *filer_destination(Screen *s)
{
    if(!s || s->key!=filer_key) return NULL;
    Filer *f=s->self;
    return f->count>1 ? f->windows[(f->active+1)%f->count].list.path : NULL;
}
int filer_change_directory(Screen *s,App *a,const char *path)
{
    if(!s || s->key!=filer_key) return -1;
    return go(current(s->self),a,path,0);
}
int filer_selection(Screen *s,FileSelection *out)
{
    if(!s || s->key!=filer_key || !out) return -1;
    Filer *f=s->self; Window *w=current(f);
    if(f->selection_capacity<w->list.count+1) {
        const char **v=realloc(f->selection,(w->list.count+1)*sizeof(*v));
        if(!v) return -1;
        f->selection=v; f->selection_capacity=w->list.count+1;
    }
    size_t n=0;
    for(size_t i=0;i<w->list.count;++i) if(w->list.entries[i].marked) f->selection[n++]=w->list.entries[i].name;
    const char *name=cursor_name(w);
    if(!n && name && strcmp(name,"..")) f->selection[n++]=name;
    *out=(FileSelection){w->list.path,name,f->selection,n}; return 0;
}
static const char *offset(const char *s,int n)
{ while(n-- && *s) font24_decode(&s); return s; }
static void display_entry(Cells *cells,Window *w,size_t pos,int slot,int row,int selected)
{
    DirEntry copy=w->list.entries[w->visible[pos]];
    const char *name=copy.name; char shown[PATH_CAP+4];
    if(selected && w->shift) name=offset(name,w->shift);
    char suffix=0;
    if(w->type) {
        if(copy.is_link) suffix='@'; else if(copy.is_dir) suffix='/';
        else if(S_ISSOCK(copy.mode)) suffix='='; else if(S_ISFIFO(copy.mode)) suffix='|';
        else if(copy.mode & 0111) suffix='*';
    }
    if(suffix) { snprintf(shown,sizeof(shown),"%s%c",name,suffix); name=shown; }
    copy.name=(char *)name;
    if((selected && w->shift) || suffix) dirlist_display_name(name,copy.display);
    fdscreen_entry(cells,&copy,selected,w->columns,slot,row);
    int name_width=w->columns==1 ? 40+FD_XCOLS : 13;
    const char *rendered=w->columns==1 ? copy.name : copy.display;
    int left=fdscreen_slot_x(w->columns,slot);
    if(suffix && cells_width(rendered)>name_width) {
        int color=copy.hidden ? MAGENTA : copy.is_dir ? CYAN : YELLOW;
        cells_put(cells,left+2+name_width-1,row,(uint32_t)suffix,selected ? BG : color,selected ? color : BG);
    }
    if(copy.marked) cells_put(cells,left+1,row,'*',YELLOW,BG);
}
/* Panes' rows add up to the list's (FDSCREEN_ROWS, less a row between
 * panes); after the screen changed size they are shared out again. */
static void layout(Filer *f)
{
    int available=FDSCREEN_ROWS-(f->count-1);
    for(int i=0;i<f->count;++i) f->windows[i].rows=available/f->count+(i<available%f->count);
}
static void fit_layout(Filer *f)
{
    int total=f->count-1;
    for(int i=0;i<f->count;++i) total+=f->windows[i].rows;
    if(total!=FDSCREEN_ROWS) layout(f);
}
static size_t page_size(const Window *w) { return (size_t)w->rows*(size_t)fdscreen_slots(w->columns); }
static void filer_draw(Screen *s,App *a,Canvas *canvas,Cells *cells)
{
    (void)canvas; Filer *f=s->self; Window *w=current(f);
    fit_layout(f);
    DirList view=w->list;
    DirEntry *entries=malloc((w->count ? w->count : 1)*sizeof(*entries));
    if(!entries) { cells_text(cells,2,FD_LINE,T("メモリが足りません"),WHITE,BG,FD_COLS-4); return; }
    for(size_t i=0;i<w->count;++i) entries[i]=w->list.entries[w->visible[i]];
    view.entries=entries; view.count=w->count;
    fdscreen_build(cells,&view,w->cursor,w->columns,(time_t)a->now);
    free(entries);
    /* Page is relative to the active window's available rows. */
    size_t per=page_size(w);
    char buf[128]; snprintf(buf,sizeof(buf),"Page:%2zu/%2zu",w->cursor/per+1,w->count ? (w->count+per-1)/per : 1);
    cells_text(cells,2,1,buf,CYAN,BG,11);
    for(int r=6;r<=FD_LINE;++r) for(int x=0;x<FD_COLS;++x) cells_put(cells,x,r,' ',WHITE,BG);
    cells_no_separators(cells);
    int row=6;
    for(int wi=0;wi<f->count;++wi) {
        Window *v=&f->windows[wi]; size_t vp=page_size(v);
        size_t first=v->cursor/vp*vp;
        for(size_t pos=first;pos<v->count && pos<first+vp;++pos) {
            size_t index=pos-first;
            display_entry(cells,v,pos,(int)(index/v->rows),row+(int)(index%v->rows),wi==f->active && pos==v->cursor);
        }
        row+=v->rows;
        if(wi+1<f->count) cells_separator(cells,row++);
    }
    uint64_t bytes=0; size_t count=0;
    for(size_t i=0;i<w->list.count;++i) if(w->list.entries[i].marked) {
        ++count; uint64_t size=w->list.entries[i].size; bytes=UINT64_MAX-bytes<size ? UINT64_MAX : bytes+size;
    }
    const int X=FD_XCOLS;
    for(int x=52+X;x<58+X;++x) cells_put(cells,x,3,' ',WHITE,BG);
    for(int x=64+X;x<78+X;++x) cells_put(cells,x,3,' ',WHITE,BG);
    fdscreen_number(cells,57+X,3,count,52+X); fdscreen_number(cells,77+X,3,bytes,64+X);
    DirEntry *e=cursor_entry(w);
    if(e && (w->shift || (w->links && e->is_link))) {
        for(int x=7;x<24+X;++x) cells_put(cells,x,4,' ',WHITE,BG);
        const char *name=w->links && e->is_link && e->link_target ? e->link_target : e->name;
        cells_text(cells,7,4,offset(name,w->shift),WHITE,BG,17+X);
    }
    if(a->mods & MOD_SHIFT) {
        /* FD 3.13's band while Shift is held: Shift+F1..F10 (revision 1 §2). */
        static const char *const bands[]={" Attr "," Info "," Move ","rmDir ","mKdir ","sHell ","Write ","Backup"," View "," Pack "};
        static const int hot[]={1,1,1,2,1,1,0,0,1,1};
        for(int k=0;k<10;++k) cells_band_item(cells,k<5 ? 4+7*k : 42+7*(k-5),6,bands[k],hot[k]);
    }
    if(w->links) cells_put(cells,0,FD_BAND,'S',CYAN,BG);
    if(w->type) cells_put(cells,1,FD_BAND,'T',CYAN,BG);
    if(w->hide) cells_put(cells,2,FD_BAND,'H',CYAN,BG);
    if(w->stacked && a->widgets.kind==WIDGET_NONE) {
        const int last=FD_COLS-1;
        int width=0; for(int i=0;i<w->stacked;++i) width+=cells_width(w->stack[i])+1;
        int x=width<last-3 ? last-width : 3;
        for(int i=0;i<w->stacked && x<last;++i) { x+=cells_text(cells,x,FD_LINE,w->stack[i],YELLOW,BG,last-x); if(x<last) cells_put(cells,x++,FD_LINE,' ',WHITE,BG); }
    }
}
/* h ?: the help box over the filer (revision 1 item 25, widgets_help). */
static void show_help(App *a)
{
    int count; const WidgetHelpItem *items=commands_help(&count);
    char title[96];
    snprintf(title,sizeof(title),T("FDTP キー一覧  Version %s"),FDTP_VERSION);
    widgets_help(&a->widgets,title,items,count);
}
static void mark(Window *w,int move,int stay)
{
    DirEntry *e=cursor_entry(w); if(e && !e->is_dir) e->marked=!e->marked;
    size_t per=page_size(w);
    if(move && w->cursor+1<w->count && (!stay || (w->cursor+1)/per==w->cursor/per)) ++w->cursor;
}
static void search(Window *w,const char *text,int direction,int next)
{
    size_t length=strlen(text); if(!w->count || !length) return;
    size_t start=w->cursor;
    for(size_t step=next ? 1 : 0;step<=w->count;++step) {
        size_t delta=step%w->count;
        size_t i=direction>0 ? (start+delta)%w->count : (start+w->count-delta)%w->count;
        if(!strncmp(w->list.entries[w->visible[i]].name,text,length)) { w->cursor=i; return; }
    }
}
/* Revision 1 §1 11: Shift+letter moves to the next name beginning with that
 * letter (either case), round to the top after the last; .. is skipped. */
static void jump(Window *w,char letter)
{
    if(!w->count) return;
    for(size_t step=1;step<=w->count;++step) {
        size_t i=(w->cursor+step)%w->count;
        const char *name=w->list.entries[w->visible[i]].name;
        if(strcmp(name,"..") && (name[0]==letter || name[0]==letter-'A'+'a')) { if(i!=w->cursor) w->shift=0; w->cursor=i; return; }
    }
}
/* Environment editor arguments: quotes and backslashes, no shell expansion. */
static int editor_words(char *buffer,char **words,int max)
{
    char *r=buffer,*w=buffer; int n=0;
    while(*r) {
        while(*r==' ' || *r=='\t') ++r;
        if(!*r) break;
        if(n>=max) return -1;
        words[n++]=w; int quote=0;
        while(*r) {
            int ch=(unsigned char)*r++;
            if(!quote && (ch==' ' || ch=='\t')) break;
            if(ch=='\\' && quote!='\'') { if(!*r) return -1; *w++=*r++; }
            else if(ch=='\'' || ch=='"') { if(!quote) quote=ch; else if(quote==ch) quote=0; else *w++=(char)ch; }
            else *w++=(char)ch;
        }
        if(quote) return -1;
        *w++=0;
    }
    return n;
}
static void editor(Window *w,App *a)
{
    DirEntry *e=cursor_entry(w); if(!e || e->is_dir) return;
    const char *program=a->settings.editor[0] ? a->settings.editor : getenv("VISUAL"); if(!program || !*program) program=getenv("EDITOR"); if(!program || !*program) program="vi";
    char *buffer=strdup(program),*path=join(w->list.path,e->name);
    if(!buffer || !path) { free(buffer); free(path); app_notice(a,T("メモリが足りません")); return; }
    char *argv[APP_EXTERNAL_ARGS]; int n=editor_words(buffer,argv,APP_EXTERNAL_ARGS-2);
    if(n<=0) app_notice(a,T("エディタの指定が正しくありません"));
    else {
        argv[n++]=path; argv[n]=NULL;
        if(app_request_external_at(a,(const char *const *)argv,APP_EXTERNAL_TERMINAL,w->list.path)<0) app_notice(a,T("外部コマンドを準備できません"));
    }
    free(buffer); free(path);
}
static void pending_key(Filer *f,App *a,const Key *k)
{
    Window *w=current(f); Widgets *ui=&a->widgets;
    if(f->pending==P_SEARCH && k->key==KCHAR && (k->mods & MOD_CTRL) && (k->ch=='s' || k->ch=='r')) {
        f->search_direction=k->ch=='s' ? 1 : -1; search(w,ui->text,f->search_direction,1); return;
    }
    WidgetResult result=widgets_key(ui,k);
    if(f->pending==P_SEARCH && result==WIDGET_HANDLED) search(w,ui->text,f->search_direction,0);
    if(result!=WIDGET_ACCEPT && result!=WIDGET_CANCEL) return;
    Pending action=f->pending; f->pending=P_NONE;
    if(result==WIDGET_CANCEL) return;
    switch(action) {
    case P_QUIT: a->quit=1; break;
    case P_PATH:
        if(!strcmp(ui->text,"?")) {
            if(!w->history_count) break;
            const char *options[DIRECTORY_HISTORY]; for(int i=0;i<w->history_count;++i) options[i]=w->history[i];
            widgets_choice(ui,T("移動履歴"),options,w->history_count); f->pending=P_HISTORY;
        } else if(*ui->text) go(w,a,ui->text,!strcmp(ui->text,".."));
        break;
    case P_HISTORY: go(w,a,w->history[ui->selected],0); break;
    case P_NEWFILE: {
        /* Revision 1 §18: the editor opens a name that does not exist yet as a
         * new, empty document and writes it when saved; an existing one opens. */
        if(!*ui->text) { app_notice(a,T("名前が空です")); break; }
        char *path=join(w->list.path,ui->text); struct stat st;
        if(!path) app_notice(a,T("メモリが足りません"));
        else if(!stat(path,&st) && S_ISDIR(st.st_mode)) app_notice(a,T("ディレクトリなので開けません"));
        else { Screen *ed=noveledit_create(a,path); if(ed && app_push(a,ed)<0 && ed->destroy) ed->destroy(ed); }
        free(path); break;
    }
    case P_MARK:
        for(size_t i=0;i<w->count;++i) { DirEntry *e=&w->list.entries[w->visible[i]]; if(!e->is_dir && !fnmatch(ui->text,e->name,0)) e->marked=1; }
        break;
    case P_FILTER: snprintf(w->filter,sizeof(w->filter),"%s",ui->text); w->cursor=0; rebuild(w,NULL); break;
    case P_SORT: {
        f->sort_next=ui->selected;
        if(f->sort_next==5) { w->sort=5; w->descending=0; rebuild(w,NULL); }
        else { static const char *const options[]={N_("昇順"),N_("降順")}; const char *translated[2]; for(int i=0;i<2;++i) translated[i]=T(options[i]); widgets_choice(ui,T("並び順"),translated,2); f->pending=P_ORDER; }
        break;
    }
    case P_ORDER: {
        const char *name=cursor_name(w); char *copy=name ? strdup(name) : NULL;
        w->sort=f->sort_next; w->descending=ui->selected; rebuild(w,copy); free(copy); break;
    }
    default: break;
    }
}
void filer_command(Screen *s,App *a,const char *id)
{
    if(!s || s->key!=filer_key || !id) return;
    Filer *f=s->self; Window *w=current(f); size_t old=w->cursor,per=page_size(w);
    DirEntry *e=cursor_entry(w);
#define IS(cmd) (!strcmp(id,#cmd))
    if(IS(CUR_UP)) { if(w->cursor) --w->cursor; }
    else if(IS(CUR_DOWN)) { if(w->cursor+1<w->count) ++w->cursor; }
    else if(IS(CUR_LEFT) || IS(ROLL_DOWN)) { size_t n=IS(CUR_LEFT) ? (size_t)w->rows : per; w->cursor=w->cursor>=n ? w->cursor-n : 0; }
    else if(IS(CUR_RIGHT) || IS(ROLL_UP)) { size_t n=IS(CUR_RIGHT) ? (size_t)w->rows : per; w->cursor=w->cursor+n<w->count ? w->cursor+n : w->count ? w->count-1 : 0; }
    else if(IS(CUR_TOP)) w->cursor=0;
    else if(IS(CUR_BOTTOM)) w->cursor=w->count ? w->count-1 : 0;
    else if(IS(ONE_COLUMNS)) w->columns=1;
    else if(IS(TWO_COLUMNS)) w->columns=2;
    else if(IS(THREE_COLUMNS)) w->columns=3;
    else if(IS(FIVE_COLUMNS)) w->columns=5;
    else if(IS(FNAME_RIGHT)) { if(e && *offset(e->name,w->shift)) ++w->shift; }
    else if(IS(FNAME_LEFT)) { if(w->shift) --w->shift; }
    else if(IS(MARK_FILE)) mark(w,0,0);
    else if(IS(MARK_FILE2)) mark(w,1,0);
    else if(IS(MARK_FILE3)) mark(w,1,1);
    else if(IS(MARK_ALL) || IS(MARK_REVERSE)) {
        for(size_t i=0;i<w->count;++i) { DirEntry *v=&w->list.entries[w->visible[i]]; if(!v->is_dir) v->marked=IS(MARK_ALL) ? 1 : !v->marked; }
    }
    else if(IS(MARK_FIND)) { widgets_input(&a->widgets,T("マーク"), "",w->list.path); f->pending=P_MARK; }
    else if(IS(OUT_DIR)) go(w,a,"..",1);
    else if(IS(LOG_TOP)) go(w,a,"/",0);
    else if(IS(REREAD_DIR)) reload_window(w,a);
    else if(IS(LOG_DIR)) { widgets_input_history(&a->widgets,T("移動先"),"",w->list.path,WIDGET_HISTORY_PATH); f->pending=P_PATH; }
    else if(IS(IN_DIR)) {
        if(e && e->is_dir) go(w,a,e->name,!strcmp(e->name,".."));
        else if(e) { char *path=join(w->list.path,e->name); if(path) { launcher_open(a,path); free(path); } }
    }
    else if(IS(PUSH_FILE)) {
        if(w->stacked==5) app_notice(a,T("スタックは5個までです"));
        else if(e && strcmp(e->name,"..")) {
            char *name=strdup(e->name); if(name) { w->stack[w->stacked++]=name; rebuild(w,NULL); }
        }
    }
    else if(IS(POP_FILE)) {
        if(w->stacked) {
            size_t at=w->cursor; char *name=w->stack[--w->stacked]; rebuild(w,name);
            size_t from=w->cursor;
            if(!cursor_name(w) || strcmp(cursor_name(w),name)) app_notice(a,T("積んだ項目はもうありません"));
            else if(w->count && from!=at && at<w->count) {
                size_t item=w->visible[from];
                if(from>at) memmove(w->visible+at+1,w->visible+at,(from-at)*sizeof(*w->visible));
                else memmove(w->visible+from,w->visible+from+1,(at-from)*sizeof(*w->visible));
                w->visible[at]=item; w->cursor=at;
            }
            free(name);
        }
    }
    else if(IS(SORT_DIR)) {
        static const char *const options[]={N_("名前"),N_("拡張子"),N_("サイズ"),N_("日付"),N_("長さ"),N_("並べない")};
        const char *translated[6]; for(int i=0;i<6;++i) translated[i]=T(options[i]);
        widgets_choice(&a->widgets,T("並べ替え"),translated,w->sort==5 ? 5 : 6); f->pending=P_SORT;
    }
    else if(IS(FIND_FILE)) { widgets_input(&a->widgets,T("検索表示"),w->filter,w->list.path); f->pending=P_FILTER; }
    else if(IS(SEARCH_FORW) || IS(SEARCH_BACK)) {
        f->search_direction=IS(SEARCH_FORW) ? 1 : -1;
        widgets_input(&a->widgets,T("検索"),"",w->list.path); f->pending=P_SEARCH;
    }
    else if(IS(SYMLINK_MODE)) w->links=!w->links;
    else if(IS(FILETYPE_MODE)) w->type=!w->type;
    else if(IS(DOTFILE_MODE)) {
        char *name=cursor_name(w) ? strdup(cursor_name(w)) : NULL;
        w->hide=!w->hide; rebuild(w,name); free(name);
    }
    else if(IS(FILEFLG_MODE)) app_notice(a,T("この OS では使えません"));
    else if(IS(SPLIT_WINDOW)) {
        if(f->count==WINDOWS) app_notice(a,T("窓は5個までです"));
        else if(window_init(&f->windows[f->count],w->list.path)<0) app_notice(a,T("窓を開けません"));
        else { apply_settings(&f->windows[f->count],&a->settings); ++f->count; layout(f); }
    }
    else if(IS(NEXT_WINDOW)) f->active=(f->active+1)%f->count;
    else if(IS(WIDEN_WINDOW) || IS(NARROW_WINDOW)) {
        if(f->count>1) {
            Window *other=&f->windows[(f->active+1)%f->count];
            if(IS(WIDEN_WINDOW) && other->rows>1) { ++w->rows; --other->rows; }
            else if(IS(NARROW_WINDOW) && w->rows>1) { --w->rows; ++other->rows; }
        }
    }
    else if(IS(KILL_WINDOW)) {
        if(f->count>1) {
            window_free(w); memmove(w,w+1,(size_t)(f->count-f->active-1)*sizeof(*w));
            memset(&f->windows[--f->count],0,sizeof(*w)); if(f->active==f->count) --f->active; layout(f);
        }
    }
    else if(IS(HELP_MESSAGE)) show_help(a);
    else if(IS(QUIT_SYSTEM)) { widgets_confirm(&a->widgets,T("FDTP を終了しますか？")); f->pending=P_QUIT; }
    else if(IS(EDIT_FILE)) editor(w,a);
    else if(IS(EDIT_NOVEL)) {
        /* Revision 1 §1 1: every file goes to the built-in editor (binary
         * shows garbled, saved in the encoding it was read in); C-e is the
         * external one. On .. or a directory, §18: ask for a new file's name. */
        if(e && e->is_dir) { widgets_input_history(&a->widgets,T("新しいファイル名"),"",w->list.path,WIDGET_HISTORY_NAME); f->pending=P_NEWFILE; }
        else if(e) {
            char *path=join(w->list.path,e->name);
            if(!path) app_notice(a,T("メモリが足りません"));
            else { Screen *ed=noveledit_create(a,path); if(ed && app_push(a,ed)<0 && ed->destroy) ed->destroy(ed); }
            free(path);
        }
    }
    else if(IS(PLAY_MIDI)) {
        /* Revision 1 §1 9: C-p plays any file with rmp-jukebox. */
        if(e && !e->is_dir) { char *path=join(w->list.path,e->name); if(path) launcher_jukebox(a,path); else app_notice(a,T("メモリが足りません")); free(path); }
        else app_notice(a,T("ファイルを選んでください"));
    }
    else if(IS(VIEW_FILE) || IS(HEX_FILE)) {
        if(e && !e->is_dir) {
            char *path=join(w->list.path,e->name);
            /* v opens by extension, like RET (launcher); b is always hex. */
            if(path && IS(VIEW_FILE)) { launcher_open(a,path); free(path); }
            else if(path) { Screen *v=hexview_create(a,path); if(v && app_push(a,v)<0 && v->destroy) v->destroy(v); free(path); }
        } else app_notice(a,T("ファイルを選んでください"));
    }
    else if(IS(EDIT_CONFIG)) config_edit(a,&a->commands);
    else if(IS(INFO_SYSTEM)) {
        /* Revision 1 item 31: the system information; the free space is this directory's. */
        Screen *v=sysinfo_create(a,w->list.path); if(v && app_push(a,v)<0 && v->destroy) v->destroy(v);
    }
    else {
        FileSelection selection;
        if(filer_selection(s,&selection)<0) { app_notice(a,T("メモリが足りません")); return; }
        if(IS(EXECUTE_FILE)) fileops_execute(a,&selection);
        else if(IS(COPY_FILE)) fileops_copy(a,&selection);
        else if(IS(DELETE_FILE)) fileops_delete(a,&selection);
        else if(IS(RENAME_FILE)) fileops_rename(a,&selection);
        else if(IS(ATTR_FILE)) fileops_attributes(a,&selection);
        else if(IS(INFO_FILESYS)) fileops_info(a,&selection);
        else if(IS(INFO_FILE)) fileops_file_info(a,&selection);
        else if(IS(MOVE_FILE)) fileops_move(a,&selection);
        else if(IS(DELETE_DIR)) fileops_rmdir(a,&selection);
        else if(IS(MAKE_DIR)) fileops_mkdir(a,&selection);
        else if(IS(EXECUTE_SH)) fileops_shell(a,&selection);
        else if(IS(WRITE_DIR)) fileops_write_directory(a,&selection);
        else if(IS(TREE_DIR) || IS(LOG_TREE)) fileops_tree(a,&selection);
        else if(IS(COPY_TREE)) fileops_copy_tree(a,&selection);
        else if(IS(MOVE_TREE)) fileops_move_tree(a,&selection);
        else if(IS(ATTR_DIR)) fileops_attributes_directory(a,&selection);
        else if(IS(FIND_DIR)) fileops_find_directory(a,&selection);
        else if(IS(UNPACK_FILE)) archive_unpack(a,&selection);
        else if(IS(UNPACK_TREE)) archive_unpack_tree(a,&selection);
        else if(IS(PACK_FILE)) archive_pack(a,&selection);
        else if(IS(SPLIT_FILE)) splitjoin_split(a,&selection);
        else if(IS(JOIN_FILE)) splitjoin_join(a,&selection);
        else if(IS(PDF_MENU)) pdftools_open(a,&selection,PDFTOOLS_MENU,-1);
        else if(IS(LOCALSEND)) localsend_ui_open(a,&selection);
    }
    if(f->active<f->count && current(f)==w && old!=w->cursor) w->shift=0;
    a->changed=1;
#undef IS
}
static int filer_key(Screen *s,App *a,const Key *k)
{
    Filer *f=s->self;
    if(f->pending!=P_NONE) { pending_key(f,a,k); return 0; }
    /* Stage 1c: `bind KEY "command"` and the second command for directories. */
    const CommandBinding *b=commands_find(&a->commands,k);
    const char *shell=commands_shell(&a->commands,b);
    DirEntry *e=cursor_entry(current(f));
    if(shell) config_run_shell(a,s,shell);
    else if(b) filer_command(s,a,b->dir_id && e && e->is_dir ? b->dir_id : b->id);
    else if(k->key==KCHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->ch>='A' && k->ch<='Z') { jump(current(f),(char)k->ch); a->changed=1; }
    return 0;
}
/* ---- the mouse (the macOS window, docs/FDTP_MACOS.md) ---- */
/* The pane and the entry at cell (col,row), as filer_draw lays them out. */
static int hit(Filer *f,int col,int row,int *pane,size_t *pos)
{
    int top=6;
    for(int wi=0;wi<f->count;++wi) {
        Window *v=&f->windows[wi];
        if(row>=top && row<top+v->rows) {
            int slots=fdscreen_slots(v->columns),width=fdscreen_slot_width(v->columns);
            int slot=col/width; if(slot>=slots || col<0) return 0;
            size_t per=page_size(v),first=v->cursor/per*per;
            size_t p=first+(size_t)slot*(size_t)v->rows+(size_t)(row-top);
            if(p>=v->count) return 0;
            *pane=wi; *pos=p; return 1;
        }
        top+=v->rows+1;
    }
    return 0;
}
/* The line-23 notice ("完了 1 件" after a copy) is no box: a key passes it
 * to the filer, and so does the mouse (a drop onto a window that has just
 * taken one was refused until a key was pressed). */
static int mouse_ready(Screen *s,App *a)
{
    return s && s->key==filer_key && app_top(a)==s && (a->widgets.kind==WIDGET_NONE || a->widgets.kind==WIDGET_NOTICE)
        && ((Filer *)s->self)->pending==P_NONE;
}
int filer_mouse(Screen *s,App *a,int col,int row,int action)
{
    if(!mouse_ready(s,a)) return 0;
    Filer *f=s->self; int pane; size_t pos;
    if(!hit(f,col,row,&pane,&pos)) return 0;
    if(a->widgets.kind==WIDGET_NOTICE) a->widgets.kind=WIDGET_NONE;   /* as a key clears it */
    if(f->active!=pane) { f->active=pane; current(f)->anchor=current(f)->cursor; }
    Window *w=current(f);
    DirEntry *e=&w->list.entries[w->visible[pos]];
    if(action==FILER_SELECT) {
        for(size_t i=0;i<w->list.count;++i) w->list.entries[i].marked=0;
        w->anchor=pos;
    } else if(action==FILER_TOGGLE) { if(!e->is_dir) e->marked=!e->marked; w->anchor=pos; }
    else if(action==FILER_RANGE) {
        /* Finder's Shift+click: from the last plain or Cmd click; directories cannot be marked (as with Space). */
        size_t from=w->anchor<w->count ? w->anchor : w->cursor;
        size_t lo=from<pos ? from : pos,hi=from<pos ? pos : from;
        for(size_t i=0;i<w->list.count;++i) w->list.entries[i].marked=0;
        for(size_t i=lo;i<=hi;++i) { DirEntry *v=&w->list.entries[w->visible[i]]; if(!v->is_dir) v->marked=1; }
    }
    if(w->cursor!=pos) w->shift=0;
    w->cursor=pos; a->changed=1; return 1;
}
int filer_mouse_marked(Screen *s,App *a,int col,int row)
{
    if(!mouse_ready(s,a)) return 0;
    Filer *f=s->self; int pane; size_t pos;
    return hit(f,col,row,&pane,&pos) && f->windows[pane].list.entries[f->windows[pane].visible[pos]].marked;
}
int filer_drag_selection(Screen *s,App *a,int col,int row,FileSelection *out)
{
    if(!mouse_ready(s,a)) return -1;
    Filer *f=s->self; int pane; size_t pos;
    if(!hit(f,col,row,&pane,&pos) || pane!=f->active) return -1;
    Window *w=current(f); DirEntry *e=&w->list.entries[w->visible[pos]];
    if(!strcmp(e->name,"..")) return -1;
    if(filer_selection(s,out)<0) return -1;
    /* A marked entry drags every marked one, any other just itself (Finder). */
    if(!e->marked || !out->count) { f->selection[0]=e->name; out->names=f->selection; out->count=1; }
    return 0;
}
char *filer_drop_directory(Screen *s,App *a,int col,int row)
{
    if(!mouse_ready(s,a)) return NULL;
    Filer *f=s->self; int pane; size_t pos;
    if(!hit(f,col,row,&pane,&pos)) return strdup(current(f)->list.path);
    Window *w=&f->windows[pane]; DirEntry *e=&w->list.entries[w->visible[pos]];
    if(e->is_dir && strcmp(e->name,"..")) return join(w->list.path,e->name);
    return strdup(w->list.path);
}
static void filer_destroy(Screen *s)
{
    Filer *f=s->self; for(int i=0;i<f->count;++i) window_free(&f->windows[i]);
    free(f->selection); free(f); free(s);
}
Screen *filer_create(const char *directory)
{
    Screen *s=calloc(1,sizeof(*s)); Filer *f=calloc(1,sizeof(*f));
    if(!s || !f) { free(s); free(f); return NULL; }
    if(window_init(&f->windows[0],directory)<0) { free(s); free(f); return NULL; }
    f->count=1; s->self=f; s->key=filer_key; s->draw=filer_draw; s->destroy=filer_destroy;
    return s;
}
