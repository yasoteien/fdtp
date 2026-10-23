// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include "fileops.h"
#include "i18n.h"
#include "app.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif
/* Revision 1 item 27: the tree and the directory search are in FD's frame
 * (rows 0 .. FD_LINE - 1, the title in its top line), the message line and
 * the band under it. The tree's rows 1 .. TREE_ROWS (19 at 25 rows), a line,
 * the path, the bottom of the frame. */
#define TREE_ROWS (FD_ROWS - 6)
#define TREE_PATH (FD_ROWS - 4)

extern int filer_change_directory(Screen *, App *, const char *);
extern const char *filer_destination(Screen *);
extern int fileops_transfer_to(App *, const FileSelection *, const char *, int);

typedef struct Node Node;
struct Node { char *path; Node *parent; Node **children; size_t count; int loaded, open; };
typedef struct Scan Scan;
struct Scan { char *path; DIR *dir; Scan *parent; };
typedef struct {
    Screen screen, *filer;
    int kind, stage, transfer;
    char *directory, *current, **names;
    size_t count;
    Node *root, *cursor;
    Node **visible;
    size_t visible_count, visible_cap, top;
    char pattern[WIDGET_CAP], found[WIDGET_CAP];
    Scan *scan;
    int scan_depth;
    struct statvfs fs;
    char info_path[WIDGET_CAP], device[WIDGET_CAP], type[128], mount[WIDGET_CAP];
    /* transfer==3: F8 in an input (revision 1 §2). The hidden input, put back
     * with the chosen directory (RET) or as it was (Esc). */
    char input_label[256], input_text[WIDGET_CAP], input_directory[WIDGET_CAP];
    int input_kind;
} TreeOp;
enum { OP_TREE, OP_INFO, OP_FIND };

static char *join_path(const char *base,const char *name)
{
    size_t n=strlen(base)+strlen(name)+2;
    char *p=malloc(n);
    if(p) snprintf(p,n,"%s%s%s",base,strcmp(base,"/")?"/":"",name);
    return p;
}
static void free_node(Node *n)
{
    if(!n) return;
    for(size_t i=0;i<n->count;i++) free_node(n->children[i]);
    free(n->children); free(n->path); free(n);
}
static void free_scan(TreeOp *o)
{
    while(o->scan) { Scan *s=o->scan; o->scan=s->parent; closedir(s->dir); free(s->path); free(s); }
    o->scan_depth=0;
}
static void tree_destroy(Screen *s)
{
    TreeOp *o=s->self;
    free_node(o->root); free(o->visible); free_scan(o);
    for(size_t i=0;i<o->count;i++) free(o->names[i]);
    free(o->names); free(o->directory); free(o->current); free(o);
}
static int node_cmp(const void *a,const void *b)
{ return strcmp((*(Node *const *)a)->path,(*(Node *const *)b)->path); }
static int load_node(Node *n)
{
    if(n->loaded) { n->open=1; return 0; }
    DIR *d=opendir(n->path);
    if(!d) return -1;
    struct dirent *e;
    int failed=0;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char *p=join_path(n->path,e->d_name); struct stat st;
        if(!p) { failed=1; break; }
        if(lstat(p,&st) || !S_ISDIR(st.st_mode)) { free(p); continue; }
        Node *child=calloc(1,sizeof(*child));
        Node **list=realloc(n->children,(n->count+1)*sizeof(*list));
        if(!child || !list) { free(child); free(p); if(list) n->children=list; failed=1; break; }
        n->children=list; child->path=p; child->parent=n; n->children[n->count++]=child;
    }
    closedir(d);
    if(failed) {
        for(size_t i=0;i<n->count;i++) free_node(n->children[i]);
        free(n->children); n->children=NULL; n->count=0; return -1;
    }
    if(n->count>1) qsort(n->children,n->count,sizeof(*n->children),node_cmp);
    n->loaded=1; n->open=1; return 0;
}
static int collect(TreeOp *o,Node *n)
{
    if(o->visible_count==o->visible_cap) {
        size_t cap=o->visible_cap?o->visible_cap*2:64;
        Node **v=realloc(o->visible,cap*sizeof(*v));
        if(!v) return -1;
        o->visible=v; o->visible_cap=cap;
    }
    o->visible[o->visible_count++]=n;
    if(n->open) for(size_t i=0;i<n->count;i++) if(collect(o,n->children[i])) return -1;
    return 0;
}
static size_t cursor_index(TreeOp *o)
{
    o->visible_count=0; collect(o,o->root);
    for(size_t i=0;i<o->visible_count;i++) if(o->visible[i]==o->cursor) return i;
    return 0;
}
static void reopen_input(App *a,const char *label,const char *text,const char *directory,int kind)
{ widgets_input_history(&a->widgets,label,text,directory,(WidgetHistoryKind)kind); a->changed=1; }
static void finish_path(TreeOp *o,App *a,const char *path)
{
    if(o->transfer==3) {
        char label[sizeof(o->input_label)],directory[sizeof(o->input_directory)]; int kind=o->input_kind;
        snprintf(label,sizeof(label),"%s",o->input_label); snprintf(directory,sizeof(directory),"%s",o->input_directory);
        char *chosen=strdup(path);
        app_pop(a);
        reopen_input(a,label,chosen?chosen:"",directory,kind); free(chosen);
        return;
    }
    /* app_pop destroys o; keep all data needed by the next workflow alive. */
    char *chosen=strdup(path);
    if(!chosen) { app_notice(a,T("メモリが足りません")); return; }
    Screen *filer=o->filer;
    if(o->transfer) {
        FileSelection sel={o->directory,o->current,(const char *const *)o->names,o->count};
        int move=o->transfer==2;
        /* Detach the owned strings before popping. */
        char *directory=o->directory,*current=o->current; char **names=o->names; size_t count=o->count;
        o->directory=NULL; o->current=NULL; o->names=NULL; o->count=0;
        app_pop(a);
        fileops_transfer_to(a,&sel,chosen,move);
        for(size_t i=0;i<count;i++) free(names[i]);
        free(names); free(directory); free(current);
    } else { app_pop(a); filer_change_directory(filer,a,chosen); }
    free(chosen);
}
static int push_scan(TreeOp *o,const char *path)
{
    DIR *d=opendir(path); if(!d) return -1;
    Scan *s=calloc(1,sizeof(*s));
    if(!s) { closedir(d); return -1; }
    s->path=strdup(path);
    if(!s->path) { free(s); closedir(d); return -1; }
    s->dir=d; s->parent=o->scan; o->scan=s; o->scan_depth++; return 0;
}
static void scan_tick(Screen *screen,App *a,double now)
{
    (void)now; TreeOp *o=screen->self;
    if(o->kind!=OP_FIND || o->stage!=1) return;
    for(int budget=0;budget<64 && o->scan;budget++) {
        Scan *s=o->scan; struct dirent *e=readdir(s->dir);
        if(!e) { o->scan=s->parent; closedir(s->dir); free(s->path); free(s); o->scan_depth--; continue; }
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char *p=join_path(s->path,e->d_name); struct stat st;
        if(!p) { free_scan(o); o->stage=3; app_notice(a,T("メモリが足りません")); return; }
        if(lstat(p,&st) || !S_ISDIR(st.st_mode)) { free(p); continue; }
        int matches=fnmatch(o->pattern,e->d_name,0)==0;
        if(o->scan_depth<256) push_scan(o,p);
        if(matches && strlen(p)<sizeof(o->found)) {
            strcpy(o->found,p); free(p); o->stage=2;
            char label[256]; snprintf(label,sizeof(label),T("%.24s ここへ移動しますか？"),e->d_name);
            widgets_confirm(&a->widgets,label); a->changed=1; return;
        }
        free(p);
    }
    if(!o->scan) { o->stage=3; app_notice(a,T("ディレクトリ検索が終わりました")); }
    a->changed=1;
}
#ifndef __APPLE__
static void mount_unescape(char *text)
{
    char *src=text,*dst=text;
    while(*src) {
        if(src[0]=='\\' && strlen(src)>=4 && src[1]>='0' && src[1]<='7' && src[2]>='0' && src[2]<='7' && src[3]>='0' && src[3]<='7') {
            *dst++=(char)((src[1]-'0')*64+(src[2]-'0')*8+src[3]-'0'); src+=4;
        } else *dst++=*src++;
    }
    *dst=0;
}
#endif
static void mount_info(TreeOp *o)
{
    strcpy(o->device,T("不明")); strcpy(o->type,T("不明")); strcpy(o->mount,T("不明"));
#ifdef __APPLE__
    struct statfs st;
    if(!statfs(o->info_path,&st)) {
        snprintf(o->device,sizeof(o->device),"%s",st.f_mntfromname);
        snprintf(o->mount,sizeof(o->mount),"%s",st.f_mntonname);
        snprintf(o->type,sizeof(o->type),"%s",st.f_fstypename);
    }
#else
    FILE *f=fopen("/proc/mounts","r");
    if(f) {
        char dev[4096],mnt[4096],type[128],opts[4096]; int dump,pass; size_t best=0;
        while(fscanf(f,"%4095s %4095s %127s %4095s %d %d",dev,mnt,type,opts,&dump,&pass)==6) {
            mount_unescape(mnt); mount_unescape(dev);
            size_t n=strlen(mnt);
            if(n>=best && !strncmp(o->info_path,mnt,n) && (n==1 || !o->info_path[n] || o->info_path[n]=='/')) {
                best=n; snprintf(o->device,sizeof(o->device),"%s",dev);
                snprintf(o->mount,sizeof(o->mount),"%s",mnt); snprintf(o->type,sizeof(o->type),"%s",type);
            }
        }
        fclose(f);
    }
#endif
}
static int operation_key(Screen *s,App *a,const Key *k)
{
    TreeOp *o=s->self;
    if(o->kind==OP_TREE) {
        size_t index=cursor_index(o);
        if(k->key==KESC) {
            if(o->transfer==3) reopen_input(a,o->input_label,o->input_text,o->input_directory,o->input_kind);
            return 1;
        }
        if(k->key==KRET) { finish_path(o,a,o->cursor->path); return 0; }
        if(k->key==KUP && index) index--;
        if(k->key==KDOWN && index+1<o->visible_count) index++;
        if(k->key==KPGUP) index=index>(size_t)TREE_ROWS?index-TREE_ROWS:0;
        if(k->key==KPGDN && o->visible_count) index=index+TREE_ROWS<o->visible_count?index+TREE_ROWS:o->visible_count-1;
        if(k->key==KHOME) index=0;
        if(k->key==KEND && o->visible_count) index=o->visible_count-1;
        o->cursor=o->visible[index];
        if(k->key==KRIGHT && load_node(o->cursor)) app_notice(a,T("ディレクトリを読めません"));
        if(k->key==KLEFT) {
            if(o->cursor->open) o->cursor->open=0;
            else if(o->cursor->parent) o->cursor=o->cursor->parent;
        }
        return 0;
    }
    if(o->kind==OP_FIND && o->stage==3) return 1;
    if(o->kind==OP_FIND && o->stage==1) return k->key==KESC?1:0;
    WidgetResult r=widgets_key(&a->widgets,k);
    if(r==WIDGET_CANCEL) {
        if(o->kind==OP_FIND && o->stage==2 && k->key!=KESC) { o->stage=1; return 0; }
        return 1;
    }
    if(r!=WIDGET_ACCEPT) return 0;
    if(o->kind==OP_FIND) {
        if(o->stage==2) { finish_path(o,a,o->found); return 0; }
        snprintf(o->pattern,sizeof(o->pattern),"%s",a->widgets.text);
        if(!o->pattern[0]) strcpy(o->pattern,"*");
        if(push_scan(o,o->directory)) { app_notice(a,T("ディレクトリを読めません")); return 1; }
        o->stage=1; scan_tick(s,a,a->now); return 0;
    }
    char *p=a->widgets.text[0]=='/'?strdup(a->widgets.text):join_path(o->directory,a->widgets.text);
    char *resolved=p?realpath(p,NULL):NULL; free(p);
    if(!resolved || strlen(resolved)>=sizeof(o->info_path) || statvfs(resolved,&o->fs)) {
        free(resolved); app_notice(a,T("ファイルシステムの情報を読めません")); return 1;
    }
    strcpy(o->info_path,resolved); free(resolved); mount_info(o);
    /* Revision 1 §1 8: the box of widgets_info; this screen then closes. */
    char v[8][160]; uint64_t block=o->fs.f_frsize?o->fs.f_frsize:o->fs.f_bsize;
    snprintf(v[0],sizeof(v[0]),T("%llu バイト"),(unsigned long long)(o->fs.f_blocks*block));
    snprintf(v[1],sizeof(v[1]),T("%llu バイト"),(unsigned long long)((o->fs.f_blocks-o->fs.f_bfree)*block));
    snprintf(v[2],sizeof(v[2]),T("%llu バイト（利用可能 %llu）"),(unsigned long long)(o->fs.f_bfree*block),(unsigned long long)(o->fs.f_bavail*block));
    snprintf(v[3],sizeof(v[3]),T("総数 %llu / 空き %llu"),(unsigned long long)o->fs.f_files,(unsigned long long)o->fs.f_ffree);
    snprintf(v[4],sizeof(v[4]),T("%lu バイト / 割当単位 %llu"),(unsigned long)o->fs.f_bsize,(unsigned long long)block);
    const char *names[]={T("パス"),T("装置名"),T("種類"),T("マウント先"),T("総容量"),T("使用容量"),T("空き容量"),T("i ノード"),T("ブロック")};
    const char *values[]={o->info_path,o->device,o->type,o->mount,v[0],v[1],v[2],v[3],v[4]};
    widgets_info(&a->widgets,T("ファイルシステムの情報"),names,values,9);
    return 1;
}
static void line(Cells *c,int row,const char *label,const char *value)
{ cells_text(c,3,row,label,CYAN,BG,19); cells_text(c,22,row,value,WHITE,BG,FD_COLS-24); }
static void path_line(Cells *c,int row,const char *label,const char *path)
{ cells_text(c,3,row,label,CYAN,BG,19); cells_path(c,22,row,path,WHITE,BG,FD_COLS-24); }
/* The screen's frame (cells.h frame_open) with the title in its top line. */
static int frame(Cells *c,const char *title)
{
    int box=frame_open(c);
    if(box>=0) frame_title(c,FRAME_TITLE_COL,title,FD_COLS-8,NULL,0);
    return box;
}
static void operation_draw(Screen *s,App *a,Canvas *canvas,Cells *c)
{
    TreeOp *o=s->self;
    if(o->kind==OP_INFO) {
        /* Only the path input (a floating box) over the filer below. */
        if(o->filer && o->filer->draw) o->filer->draw(o->filer,a,canvas,c);
        return;
    }
    if(o->kind==OP_TREE) {
        char title[300]; snprintf(title,sizeof(title),T("%sのツリー"),o->input_label);
        frame(c,o->transfer==3?title:o->transfer==1?T("コピー先のツリー"):o->transfer==2?T("移動先のツリー"):T("ディレクトリのツリー"));
        size_t selected=cursor_index(o);
        if(selected<o->top) o->top=selected;
        if(selected>=o->top+TREE_ROWS) o->top=selected-(TREE_ROWS-1);
        for(size_t i=o->top;i<o->visible_count && i<o->top+TREE_ROWS;i++) {
            Node *n=o->visible[i]; int depth=0; for(Node *p=n->parent;p;p=p->parent) depth++;
            int col=2+depth*2; if(col>56+FD_XCOLS) col=56+FD_XCOLS;
            const char *name=strrchr(n->path,'/'); name=name&&name[1]?name+1:"/";
            int row=(int)(i-o->top)+1, fg=n==o->cursor?BG:CYAN,bg=n==o->cursor?CYAN:BG;
            /* Guide lines: │ while an ancestor has later siblings, then ├ or └. */
            for(Node *p=n;p->parent;p=p->parent) {
                int level=0; for(Node *q=p->parent;q->parent;q=q->parent) level++;
                int x=2+level*2; if(x>54+FD_XCOLS) continue;
                int last=p->parent->children[p->parent->count-1]==p;
                if(p==n) cells_text(c,x,row,last?"└":"├",CYAN,BG,2);
                else if(!last) cells_text(c,x,row,"│",CYAN,BG,2);
            }
            cells_put(c,col,row,n->loaded&&!n->count?' ':n->open?'-':'+',CYAN,BG);
            cells_text(c,col+1,row,name,fg,bg,FD_COLS-3-col);
        }
        frame_hline(c,TREE_PATH-1);
        cells_path(c,2,TREE_PATH,o->cursor->path,WHITE,BG,FD_COLS-4);
        const FrameKey keys[]={{"↑↓",T("選択")},{"→",T("開く")},{"←",T("閉じる/親")},{"RET",T("決定")},{"Esc",T("取消")}};
        frame_keys(c,2,keys,5);
    } else {
        frame(c,T("ディレクトリ検索"));
        path_line(c,2,T("検索の起点"),o->directory);
        if(o->stage) line(c,4,T("パターン"),o->pattern);
        if(o->stage) frame_hline(c,6);
        if(o->stage==2) { cells_text(c,3,7,T("見つかった物"),CYAN,BG,19); cells_path(c,22,7,o->found,YELLOW,BG,FD_COLS-24); }
        if(o->stage==1) { cells_text(c,0,FD_LINE,T("検索中  Esc で打ち切り"),CYAN,BG,FD_COLS); if(o->scan) cells_path(c,3,7,o->scan->path,CYAN,BG,FD_COLS-6); }
        const FrameKey keys[]={{"Esc",T("戻る")}};
        frame_keys(c,2,keys,1);
    }
}
static int begin(App *a,const FileSelection *sel,int kind,int transfer)
{
    if(!sel || !sel->directory) return -1;
    TreeOp *o=calloc(1,sizeof(*o)); if(!o) return -1;
    o->screen=(Screen){.self=o,.key=operation_key,.draw=operation_draw,.tick=scan_tick,.destroy=tree_destroy};
    o->filer=app_top(a); o->kind=kind; o->transfer=transfer;
    o->directory=strdup(sel->directory); o->current=strdup(sel->current?sel->current:"");
    if(!o->directory || !o->current) goto fail;
    if(transfer && transfer!=3 && sel->count) {
        o->names=calloc(sel->count,sizeof(*o->names)); if(!o->names) goto fail;
        for(size_t i=0;i<sel->count;i++) { o->names[i]=strdup(sel->names[i]); if(!o->names[i]) goto fail; o->count++; }
    }
    if(kind==OP_TREE) {
        o->root=calloc(1,sizeof(*o->root)); if(!o->root) goto fail;
        o->root->path=strdup("/"); if(!o->root->path) goto fail;
        o->cursor=o->root;
        const char *initial=transfer==3?sel->current:transfer?filer_destination(o->filer):NULL;
        char *path=realpath(initial?initial:sel->directory,NULL);
        if(path) {
            Node *n=o->root; char *part=path+1;
            while(*part) {
                char *end=strchr(part,'/'); if(end) *end=0;
                if(load_node(n)) break;
                Node *next=NULL;
                for(size_t i=0;i<n->count;i++) { const char *base=strrchr(n->children[i]->path,'/'); if(base && !strcmp(base+1,part)) { next=n->children[i]; break; } }
                if(!next) break;
                n=next; o->cursor=n;
                if(!end) break;
                part=end+1;
            }
            free(path);
        }
        load_node(o->cursor);
        if(collect(o,o->root)) goto fail;
    }
    if(app_push(a,&o->screen)) goto fail;
    if(kind==OP_INFO) widgets_input(&a->widgets,T("情報を調べるパス"),sel->directory,sel->directory);
    if(kind==OP_FIND) widgets_input(&a->widgets,T("ディレクトリのパターン"),"*",sel->directory);
    return 0;
fail:
    tree_destroy(&o->screen); app_notice(a,T("メモリが足りません")); return -1;
}
/* F8 in a destination or path input: hide it and choose in the tree, which
 * starts at the typed directory when there is one, else at the input's. */
static void input_tree(void *context)
{
    App *a=context; Widgets *w=&a->widgets;
    char label[256],text[WIDGET_CAP],directory[WIDGET_CAP]; int kind=(int)w->history_kind;
    snprintf(label,sizeof(label),"%s",w->label); snprintf(text,sizeof(text),"%s",w->text); snprintf(directory,sizeof(directory),"%s",w->directory);
    char *typed=!*text?NULL:text[0]=='/'?strdup(text):join_path(directory,text); struct stat st;
    const char *start=typed && !stat(typed,&st) && S_ISDIR(st.st_mode)?typed:directory;
    FileSelection sel={directory,start,NULL,0};
    w->kind=WIDGET_NONE;
    if(begin(a,&sel,OP_TREE,3)) { free(typed); reopen_input(a,label,text,directory,kind); return; }
    TreeOp *o=app_top(a)->self;
    snprintf(o->input_label,sizeof(o->input_label),"%s",label); snprintf(o->input_text,sizeof(o->input_text),"%s",text);
    snprintf(o->input_directory,sizeof(o->input_directory),"%s",directory); o->input_kind=kind;
    free(typed);
}
void fileops_tree_install(App *a) { widgets_tree_hook(&a->widgets,input_tree,a); }
int fileops_tree(App *a,const FileSelection *s) { return begin(a,s,OP_TREE,0); }
int fileops_copy_tree(App *a,const FileSelection *s) { return begin(a,s,OP_TREE,1); }
int fileops_move_tree(App *a,const FileSelection *s) { return begin(a,s,OP_TREE,2); }
int fileops_info(App *a,const FileSelection *s) { return begin(a,s,OP_INFO,0); }
int fileops_find_directory(App *a,const FileSelection *s) { return begin(a,s,OP_FIND,0); }
