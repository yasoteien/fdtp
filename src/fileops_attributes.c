// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "fileops.h"
#include "app.h"
#include "i18n.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>

typedef struct {
    Screen screen;
    Screen *filer;
    char *directory, **names;
    size_t count;
    int recursive, marked, choice, row, column, mode_changed, date_changed;
    int owner_changed, group_changed, input;
    char mode[10], date[9], clock[6], owner[64], group[64];
    size_t done, failed, links;
    char **queue;
    size_t queued, capacity, position;
    int running, cancelled;
    uid_t uid;
    gid_t gid;
    char first_error[180];
} Attributes;
static const mode_t bits[9]={S_IRUSR,S_IWUSR,S_IXUSR,S_IRGRP,S_IWGRP,S_IXGRP,S_IROTH,S_IWOTH,S_IXOTH};
static char *attr_path(const char *directory,const char *name)
{
    size_t n=strlen(directory)+strlen(name)+2;
    char *p=malloc(n);
    if(p) snprintf(p,n,"%s/%s",directory,name);
    return p;
}
static void attr_destroy(Screen *screen)
{
    Attributes *a=screen->self;
    for(size_t i=0;i<a->count;i++) free(a->names[i]);
    for(size_t i=0;i<a->queued;i++) free(a->queue[i]);
    free(a->queue); free(a->names); free(a->directory); free(a);
}
static void attr_error(Attributes *a,const char *path)
{
    a->failed++;
    if(!a->first_error[0]) snprintf(a->first_error,sizeof(a->first_error),"%.90s: %.70s",path,strerror(errno));
}
static mode_t attr_mode(const Attributes *a,mode_t original)
{
    mode_t mode=original & 07777;
    for(int i=0;i<9;i++) {
        char ch=a->mode[i];
        if(ch=='*') continue;
        int enabled=ch!='-';
        if(ch=='X') enabled=S_ISDIR(original) || (original&0111);
        if(ch=='!') enabled=!(original&bits[i]);
        if(ch=='S' || ch=='T') enabled=0;
        if(enabled) mode|=bits[i]; else mode&=~bits[i];
        if(i%3==2 && !a->recursive) {
            mode_t special=i==2 ? S_ISUID : i==5 ? S_ISGID : S_ISVTX;
            mode&=~special;
            if(ch=='s'||ch=='S'||ch=='t'||ch=='T') mode|=special;
        }
    }
    return mode;
}
static void attr_stamp(time_t when,char date[9],char clock[6])
{
    struct tm t;
    if(!localtime_r(&when,&t)) { strcpy(date,"00-01-01"); strcpy(clock,"00:00"); return; }
    strftime(date,9,"%y-%m-%d",&t); strftime(clock,6,"%H:%M",&t);
}
static int attr_time(const Attributes *a,time_t original,time_t *result)
{
    char date[9],clock[6]; struct tm t;
    if(!localtime_r(&original,&t)) return -1;
    attr_stamp(original,date,clock);
    for(int i=0;i<8;i++) if(a->date[i]!='*') date[i]=a->date[i];
    for(int i=0;i<5;i++) if(a->clock[i]!='*') clock[i]=a->clock[i];
    int year,month,day,hour,minute;
    if(sscanf(date,"%2d-%2d-%2d",&year,&month,&day)!=3 || sscanf(clock,"%2d:%2d",&hour,&minute)!=2) return -1;
    /* Retain the original century: editing an old file must not move it 100 years. */
    t.tm_year=(t.tm_year/100)*100+year; t.tm_mon=month-1; t.tm_mday=day;
    t.tm_hour=hour; t.tm_min=minute; t.tm_isdst=-1;
    struct tm expected=t;
    time_t value=mktime(&t);
    if(t.tm_year!=expected.tm_year || t.tm_mon!=expected.tm_mon || t.tm_mday!=expected.tm_mday ||
       t.tm_hour!=expected.tm_hour || t.tm_min!=expected.tm_min) return -1;
    *result=value; return 0;
}
static int attr_id(const char *text,int group,unsigned long *id)
{
    char *end; errno=0; unsigned long n=strtoul(text,&end,10);
    if(*text && !*end && !errno && text[0]!='-') {
        if(group ? (unsigned long)(gid_t)n!=n : (unsigned long)(uid_t)n!=n) return -1;
        if(n==(unsigned long)(group ? (gid_t)-1 : (uid_t)-1)) return -1;
        *id=n; return 0;
    }
    if(group) { struct group *g=getgrnam(text); if(g) { *id=g->gr_gid; return 0; } }
    else { struct passwd *p=getpwnam(text); if(p) { *id=p->pw_uid; return 0; } }
    return -1;
}
static int attr_queue(Attributes *a,const char *path)
{
    if(a->queued==a->capacity) {
        size_t capacity=a->capacity?a->capacity*2:64;
        char **queue=realloc(a->queue,capacity*sizeof(*queue));
        if(!queue) return -1;
        a->queue=queue; a->capacity=capacity;
    }
    char *copy=strdup(path);
    if(!copy) return -1;
    a->queue[a->queued++]=copy; return 0;
}
static void attr_collect(Attributes *a,const char *path,unsigned depth)
{
    struct stat st;
    if(lstat(path,&st)<0) { attr_error(a,path); return; }
    if(S_ISLNK(st.st_mode)) { a->links++; return; }
    if(depth>512) { errno=ELOOP; attr_error(a,path); return; }
    if(a->recursive && S_ISDIR(st.st_mode)) {
        int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        DIR *dir=fd>=0?fdopendir(fd):NULL;
        if(!dir) { if(fd>=0) close(fd); attr_error(a,path); }
        else {
            struct dirent *entry;
            for(;;) {
                errno=0; entry=readdir(dir);
                if(!entry) { if(errno) attr_error(a,path); break; }
                if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
                char *child=attr_path(path,entry->d_name);
                if(child) { attr_collect(a,child,depth+1); free(child); }
                else { errno=ENOMEM; attr_error(a,path); }
            }
            closedir(dir);
        }
    }
    /* Parents run last, so removing search permission does not block children. */
    if(attr_queue(a,path)<0) { errno=ENOMEM; attr_error(a,path); }
}
static void attr_apply_one(Attributes *a,const char *path)
{
    struct stat st,actual;
    if(lstat(path,&st)<0) { attr_error(a,path); return; }
    if(S_ISLNK(st.st_mode)) { a->links++; return; }
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0 && !S_ISDIR(st.st_mode)) fd=open(path,O_WRONLY|O_NOFOLLOW|O_NONBLOCK);
    /* An owner may chmod a 000 file without opening it for reading/writing.
       The fallback requires matching identity and NOFOLLOW for every mutation. */
    int result=fd>=0?fstat(fd,&actual):lstat(path,&actual);
    if(result<0 || actual.st_dev!=st.st_dev || actual.st_ino!=st.st_ino || S_ISLNK(actual.st_mode)) {
        errno=ESTALE; attr_error(a,path); if(fd>=0) close(fd); return;
    }
    int failed=0;
    time_t modified=st.st_mtime;
    if(a->date_changed && attr_time(a,modified,&modified)<0) { errno=EINVAL; failed=1; }
    if(!failed && (a->owner_changed||a->group_changed)) {
        result=fd>=0?fchown(fd,a->uid,a->gid):fchownat(AT_FDCWD,path,a->uid,a->gid,AT_SYMLINK_NOFOLLOW);
        if(result<0) failed=1;
    }
    if(!failed && a->mode_changed) {
        mode_t mode=attr_mode(a,st.st_mode);
        result=fd>=0?fchmod(fd,mode):fchmodat(AT_FDCWD,path,mode,AT_SYMLINK_NOFOLLOW);
        if(result<0) failed=1;
    }
    if(!failed && a->date_changed) {
        struct timespec times[2];
#if defined(__APPLE__)
        times[0]=st.st_atimespec; times[1]=st.st_mtimespec;
#else
        times[0]=st.st_atim; times[1]=st.st_mtim;
#endif
        times[1].tv_sec=modified; times[1].tv_nsec=0;
        result=fd>=0?futimens(fd,times):utimensat(AT_FDCWD,path,times,AT_SYMLINK_NOFOLLOW);
        if(result<0) failed=1;
    }
    if(failed) attr_error(a,path); else a->done++;
    if(fd>=0) close(fd);
}
static void attr_tick(Screen *screen,App *app,double now)
{
    Attributes *a=screen->self; (void)now;
    if(!a->running) return;
    if(!a->cancelled && a->position<a->queued) {
        const char *path=a->queue[a->position++];
        attr_apply_one(a,path);
        char message[256];
        snprintf(message,sizeof(message),T("属性変更中 %zu/%zu  %.160s  Esc:中断"),a->position,a->queued,path);
        widgets_notice(&app->widgets,message); app->changed=1; return;
    }
    filer_reload(a->filer,app);
    char message[256];
    if(a->cancelled) snprintf(message,sizeof(message),T("属性変更を中断しました: 変更 %zu / 失敗 %zu"),a->done,a->failed);
    else if(a->failed) snprintf(message,sizeof(message),T("変更 %zu / 失敗 %zu: %.150s"),a->done,a->failed,a->first_error);
    else snprintf(message,sizeof(message),T("属性を変更しました: %zu 個（リンク %zu 個は対象外）"),a->done,a->links);
    app_pop(app); app_notice(app,message);
}
static int attr_finish(Attributes *a,App *app)
{
    unsigned long owner=0,group=0;
    if((a->owner_changed && attr_id(a->owner,0,&owner)<0) ||
       (a->group_changed && attr_id(a->group,1,&group)<0)) {
        app_notice(app,T("持ち主またはグループが見つかりません")); return 0;
    }
    /* Validate dates before changing any file, including every masked original. */
    if(a->date_changed) for(size_t i=0;i<a->count;i++) {
        char *path=attr_path(a->directory,a->names[i]); struct stat st; time_t out;
        int invalid=path && !lstat(path,&st) && !S_ISLNK(st.st_mode) && attr_time(a,st.st_mtime,&out)<0;
        free(path);
        if(invalid) { app_notice(app,T("日時が正しくありません")); return 0; }
    }
    a->uid=a->owner_changed?(uid_t)owner:(uid_t)-1;
    a->gid=a->group_changed?(gid_t)group:(gid_t)-1;
    for(size_t i=0;i<a->count;i++) {
        char *path=attr_path(a->directory,a->names[i]);
        if(path) { attr_collect(a,path,0); free(path); }
        else { errno=ENOMEM; attr_error(a,""); }
    }
    a->running=1;
    char message[256];
    snprintf(message,sizeof(message),T("属性変更中 0/%zu  Esc:中断"),a->queued);
    widgets_notice(&app->widgets,message); app->changed=1;
    return 0;
}
/* Revision 1 item 27: a floating box (frame and shadow) over the bottom of
 * the list: rows 10..21 at 25 rows (the shadow on row 22), columns 1..77.
 * The title in its top line, the name, a line, the five fields, a line, two
 * rows of hints, the keys in its bottom line. */
#define ATTR_TOP (10+FD_XROWS)
#define ATTR_FIELDS (ATTR_TOP+3)
static void attr_draw(Screen *screen,App *app,Canvas *canvas,Cells *cells)
{
    Attributes *a=screen->self;
    if(a->filer && a->filer->draw) a->filer->draw(a->filer,app,canvas,cells);
    const int top=ATTR_TOP,w=FD_COLS-3,x0=3,v0=21,inner=w-4;
    for(int y=top;y<top+12;y++) cells_separator_off(cells,y);
    int box=cells_box(cells,1,top,w,12,CELL_BOX_FRAME|CELL_BOX_SHADOW);
    if(box<0) return;
    CellBox *b=&cells->boxes[box];
    int tw=cells_text(cells,x0,top,a->recursive?T("下の階層も属性変更（モードのみ）"):T("属性変更"),YELLOW,BG,inner);
    b->top_from=(short)(x0-1); b->top_to=(short)(x0+tw+1);
    cells_text(cells,x0,top+1,a->names[0],WHITE,BG,inner);
    cells_box_line(cells,0,top+2,1,w);
    cells_text(cells,x0,ATTR_FIELDS,T("a モード"),CYAN,BG,18);
    cells_text(cells,v0,ATTR_FIELDS,a->mode,WHITE,BG,9);
    if(!a->recursive) {
        cells_text(cells,x0,ATTR_FIELDS+1,T("d 日付"),CYAN,BG,18); cells_text(cells,v0,ATTR_FIELDS+1,a->date,WHITE,BG,8);
        cells_text(cells,x0,ATTR_FIELDS+2,T("t 時刻"),CYAN,BG,18); cells_text(cells,v0,ATTR_FIELDS+2,a->clock,WHITE,BG,5);
        cells_text(cells,x0,ATTR_FIELDS+3,T("o 持ち主"),CYAN,BG,18); cells_text(cells,v0,ATTR_FIELDS+3,a->owner,WHITE,BG,w-v0-1);
        cells_text(cells,x0,ATTR_FIELDS+4,T("g グループ"),CYAN,BG,18); cells_text(cells,v0,ATTR_FIELDS+4,a->group,WHITE,BG,w-v0-1);
    }
    cells_box_line(cells,0,top+8,1,w);
    cells_text(cells,x0,top+9,a->recursive?T("SPC: x/X/!/-/*  X:条件付き実行  !:反転"):T("SPC:切替  数字:日時  m:*（元のまま）  o/g→SPC:名前入力"),CYAN,BG,inner);
    cells_text(cells,x0,top+10,geteuid()==0?T("持ち主・グループを変更できます"):T("持ち主・グループは表示のみ（root 以外）"),WHITE,BG,inner);
    const char *keys=T("←→↑↓:移動  a/d/t/o/g:行頭  RET:反映  Esc:取消");
    int kw=cells_width(keys);
    if(kw<=inner) {
        int kx=1+w-3-kw;
        b->bottom_from=(short)(kx-1); b->bottom_to=(short)(kx+kw+1);
        cells_text(cells,kx,top+11,keys,CYAN,BG,kw);
    }
    if(a->choice || a->running) return;
    const char *value=a->row==0?a->mode:a->row==1?a->date:a->row==2?a->clock:a->row==3?a->owner:a->group;
    int row=ATTR_FIELDS+a->row;
    cells_put(cells,v0+a->column,row,(unsigned char)value[a->column],BG,WHITE);
}
static void attr_next(Attributes *a,int direction)
{
    int max=a->row==0?8:a->row==1?7:4;
    a->column+=direction;
    if(a->column<0) a->column=0;
    if(a->column>max) a->column=max;
    if(a->row==1 && (a->column==2||a->column==5)) a->column+=direction;
    if(a->row==2 && a->column==2) a->column+=direction;
}
static int attr_key(Screen *screen,App *app,const Key *key)
{
    Attributes *a=screen->self;
    if(a->running) {
        if(key->key==KESC) a->cancelled=1;
        return 0;
    }
    if(a->choice || a->input) {
        WidgetResult result=widgets_key(&app->widgets,key);
        if(result==WIDGET_CANCEL) { if(a->choice) { filer_reload(a->filer,app); return 1; } a->input=0; return 0; }
        if(result!=WIDGET_ACCEPT) return 0;
        if(a->input) {
            if(strlen(app->widgets.text)>=sizeof(a->owner)) { a->input=0; app_notice(app,T("名前が長すぎます")); return 0; }
            if(a->input==1) { strcpy(a->owner,app->widgets.text); a->owner_changed=1; }
            else { strcpy(a->group,app->widgets.text); a->group_changed=1; }
            a->input=0; return 0;
        }
        a->choice=0;
        a->row=app->widgets.selected==0?0:1;
        return 0;
    }
    if(key->key==KESC) { filer_reload(a->filer,app); return 1; }
    if(key->key==KRET) return attr_finish(a,app);
    if(key->key==KUP || key->key==KDOWN || key->key==KTAB) {
        if(!a->recursive) a->row=(a->row+(key->key==KUP?4:1))%5;
        a->column=0; return 0;
    }
    if(key->key==KLEFT || key->key==KRIGHT) {
        if(a->row<3) attr_next(a,key->key==KLEFT?-1:1);
        return 0;
    }
    if(key->key!=KCHAR || key->ch>127 || !key->ch || (key->mods&(MOD_CTRL|MOD_ALT))) return 0;
    char ch=(char)key->ch;
    static const char shortcuts[]="adtog";
    const char *shortcut=strchr(shortcuts,ch);
    if(shortcut && (!a->recursive || ch=='a')) { a->row=(int)(shortcut-shortcuts); a->column=0; return 0; }
    if(a->row>=3) {
        if(ch==' ' && geteuid()==0) {
            a->input=a->row==3?1:2;
            widgets_input(&app->widgets,a->row==3?T("持ち主（名前または番号）"):T("グループ（名前または番号）"),a->row==3?a->owner:a->group,a->directory);
        }
        return 0;
    }
    if(a->row==0) {
        if(ch=='m') a->mode[a->column]='*';
        else if(ch==' ') {
            char *p=&a->mode[a->column];
            if(a->column%3==2) {
                const char *cycle=a->recursive?"xX!-*":(a->column==8?"xt-":"xs-");
                const char *current=strchr(cycle,*p);
                *p=current && current[1]?current[1]:cycle[0];
            } else *p=*p=='-'||*p=='*'?(a->column%3==0?'r':'w'):'-';
        } else return 0;
        a->mode_changed=1;
    } else if(ch=='m' || (ch>='0'&&ch<='9')) {
        char *value=a->row==1?a->date:a->clock;
        value[a->column]=ch=='m'?'*':ch; a->date_changed=1;
        attr_next(a,1);
    }
    return 0;
}
static int attr_start(App *app,const FileSelection *selection,int recursive)
{
    if(!selection || !selection->directory || !selection->current ||
       (recursive && !strcmp(selection->current,".."))) { app_notice(app,T("対象がありません")); return -1; }
    size_t count=recursive?1:selection->count;
    if(!count) { app_notice(app,T("対象がありません")); return -1; }
    Attributes *a=calloc(1,sizeof(*a));
    if(!a) return -1;
    a->screen=(Screen){.self=a,.key=attr_key,.draw=attr_draw,.tick=attr_tick,.destroy=attr_destroy};
    a->filer=app_top(app); a->recursive=recursive;
    a->directory=strdup(selection->directory); a->names=calloc(count,sizeof(*a->names));
    if(!a->directory || !a->names) goto fail;
    for(size_t i=0;i<count;i++) {
        const char *name=recursive?selection->current:selection->names[i];
        if(!name || !strcmp(name,"..") || !(a->names[i]=strdup(name))) goto fail;
        a->count++;
    }
    char *path=attr_path(a->directory,a->names[0]); struct stat st;
    if(!path) goto fail;
    int result=lstat(path,&st); free(path);
    if(result<0) goto fail;
    if(recursive && !S_ISDIR(st.st_mode)) { app_notice(app,T("ディレクトリを選んでください")); attr_destroy(&a->screen); return -1; }
    for(int i=0;i<9;i++) a->mode[i]=(st.st_mode&bits[i])?"rwx"[i%3]:'-';
    if(st.st_mode&S_ISUID) a->mode[2]=(st.st_mode&S_IXUSR)?'s':'S';
    if(st.st_mode&S_ISGID) a->mode[5]=(st.st_mode&S_IXGRP)?'s':'S';
    if(st.st_mode&S_ISVTX) a->mode[8]=(st.st_mode&S_IXOTH)?'t':'T';
    attr_stamp(st.st_mtime,a->date,a->clock);
    snprintf(a->owner,sizeof(a->owner),"%lu",(unsigned long)st.st_uid);
    snprintf(a->group,sizeof(a->group),"%lu",(unsigned long)st.st_gid);
    a->marked=!recursive && filer_marked(a->filer);
    if(a->marked) a->choice=1;
    if(recursive || a->marked) memset(a->mode,'*',9);
    if(a->marked) { strcpy(a->date,"**-**-**"); strcpy(a->clock,"**:**"); }
    if(app_push(app,&a->screen)<0) goto fail;
    if(a->choice) {
        static const char *const labels[]={N_("モード"),N_("日時")};
        const char *options[]={T(labels[0]),T(labels[1])};
        widgets_choice(&app->widgets,T("属性変更"),options,2);
    }
    return 0;
fail:
    attr_destroy(&a->screen); app_notice(app,T("属性を読み込めませんでした")); return -1;
}
int fileops_attributes(App *app,const FileSelection *selection) { return attr_start(app,selection,0); }
int fileops_attributes_directory(App *app,const FileSelection *selection) { return attr_start(app,selection,1); }
