// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifdef __linux__
#define _GNU_SOURCE   /* statx (the creation time) */
#endif
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "fileops.h"
#include "i18n.h"
#include "app.h"
#include "launcher.h"
#include <grp.h>
#include <pwd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#endif

typedef enum { COPY, MOVE, DELETE, RMDIR, RENAME, MKDIR, EXECUTE, SHELL } Operation;
typedef enum { INPUT, CONFIRM, READONLY, CONFLICT, NEWNAME, RELOCATE } Phase;
typedef struct Job Job;
struct Job { char *src,*dst; int finish,move; struct stat original; Job *next; };
typedef struct {
    Screen screen, *filer;
    Operation op; Phase phase;
    char *directory,*current,**names; size_t count,index,done;
    char *destination; Job *jobs;
    int overwrite,errors,running;
    char last_error[256], label[160];
} Ops;
static char *last_destination;
static char *path_join(const char *d,const char *n)
{
    if(!n) { errno=EINVAL; return NULL; }
    if(*n=='/') return strdup(n);
    size_t len=strlen(d)+strlen(n)+2; char *p=malloc(len);
    if(p) snprintf(p,len,"%s/%s",d,n);
    return p;
}
static void error(App *a,Ops *o,const char *path)
{
    char msg[256]; snprintf(msg,sizeof(msg),T("操作できません: %.100s (%s)"),path,strerror(errno));
    ++o->errors; snprintf(o->last_error,sizeof(o->last_error),"%s",msg); app_notice(a,msg);
}
static int safe_name(const char *p) { return p && *p && strcmp(p,".") && strcmp(p,".."); }
static void job_free(Job *j) { if(j) { free(j->src); free(j->dst); free(j); } }
static int job_push(Ops *o,const char *src,const char *dst,int finish)
{
    Job *j=calloc(1,sizeof(*j));
    if(!j) return -1;
    j->src=strdup(src); j->dst=dst?strdup(dst):NULL; j->finish=finish; j->move=o->op==MOVE;
    if(!j->src || (dst&&!j->dst)) { job_free(j); return -1; }
    j->next=o->jobs; o->jobs=j; return 0;
}
static void destroy(Screen *s)
{
    Ops *o=s->self;
    free(o->directory); free(o->current); free(o->destination);
    for(size_t i=0;i<o->count;++i) free(o->names[i]);
    free(o->names);
    while(o->jobs) { Job *j=o->jobs; o->jobs=j->next; job_free(j); }
    free(o);
}
static void draw(Screen *s,App *a,Canvas *c,Cells *cells)
{ Ops *o=s->self; o->filer->draw(o->filer,a,c,cells); }
static void finish(App *a,Ops *o)
{
    filer_reload(o->filer,a);
    if(o->errors) app_notice(a,o->last_error);
    if(!o->errors) { char msg[128]; snprintf(msg,sizeof(msg),T("完了 %zu 件"),o->done); app_notice(a,msg); }
    app_pop(a);
}
static int mkdirs(const char *path)
{
    char *p=strdup(path); if(!p) return -1;
    for(char *s=p+1;;++s) if(*s=='/' || !*s) {
        char c=*s; *s=0;
        if(mkdir(p,0777)<0) { struct stat st; if(errno!=EEXIST || stat(p,&st)<0 || !S_ISDIR(st.st_mode)) { free(p); return -1; } }
        *s=c; if(!c) break;
    }
    free(p); return 0;
}
static int metadata(const char *dst,const struct stat *st)
{
    struct timespec ts[2]={st->st_atim,st->st_mtim};
    if(chmod(dst,st->st_mode&07777)<0) return -1;
    return utimensat(AT_FDCWD,dst,ts,0);
}
/* Never follow a link at either end. Existing regular files are replaced only
 * after a complete sibling temporary file has been written successfully. */
static int copy_leaf(const char *src,const char *dst,const struct stat *st)
{
    size_t n=strlen(dst)+24; char *tmp=malloc(n); if(!tmp) return -1;
    snprintf(tmp,n,"%s.fdtp-XXXXXX",dst); int out=mkstemp(tmp),result=-1;
    if(out<0) { free(tmp); return -1; }
    if(S_ISLNK(st->st_mode)) {
        char *target=malloc((size_t)st->st_size+4097);
        if(target) {
            ssize_t len=readlink(src,target,(size_t)st->st_size+4096);
            if(len>=0) { target[len]=0; close(out); out=-1; unlink(tmp); if(!symlink(target,tmp)) result=0; }
            free(target);
        }
    } else if(S_ISREG(st->st_mode)) {
        int in=open(src,O_RDONLY|O_NOFOLLOW);
        if(in>=0) {
            char buf[65536]; ssize_t got; result=0;
            while((got=read(in,buf,sizeof(buf)))>0) {
                ssize_t offset=0;
                while(offset<got) { ssize_t put=write(out,buf+offset,(size_t)(got-offset)); if(put<0&&errno==EINTR) continue; if(put<=0) { result=-1; break; } offset+=put; }
                if(result<0) break;
            }
            if(got<0) result=-1;
            close(in);
            if(!result && metadata(tmp,st)<0) result=-1;
        }
    } else errno=ENOTSUP;
    int saved=errno; if(out>=0 && close(out)<0) { result=-1; saved=errno; }
    if(!result && rename(tmp,dst)<0) { result=-1; saved=errno; }
    unlink(tmp); free(tmp); errno=saved; return result;
}
static int descendant(const char *src,const char *dst)
{
    char real_src[4096],parent[4096],real_parent[4096];
    if(!realpath(src,real_src)) return 0;
    if(strlen(dst)>=sizeof(parent)) return 1;
    strcpy(parent,dst); char *slash=strrchr(parent,'/'); if(!slash) return 1;
    if(slash==parent) slash[1]=0; else *slash=0;
    if(!realpath(parent,real_parent)) return 1;
    size_t n=strlen(real_src);
    return !strncmp(real_src,real_parent,n) && (!real_parent[n] || real_parent[n]=='/' || (n==1&&real_src[0]=='/'));
}
static int children(Ops *o,Job *j,int deleting)
{
    DIR *dir=opendir(j->src); if(!dir) return -1;
    int result=job_push(o,j->src,j->dst,1);
    if(!result) { o->jobs->move=j->move; if(lstat(j->src,&o->jobs->original)<0) result=-1; }
    struct dirent *e;
    while(!result && (e=readdir(dir))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char *src=path_join(j->src,e->d_name),*dst=deleting?NULL:path_join(j->dst,e->d_name);
        if(!src || (!deleting&&!dst) || job_push(o,src,dst,0)<0) result=-1;
        else o->jobs->move=j->move;
        free(src); free(dst);
    }
    closedir(dir); return result;
}
/* Row 23 is narrow: show paths below the operation directory relatively. */
static const char *shown(const Ops *o,const char *path)
{
    size_t n=strlen(o->directory);
    if(!strncmp(path,o->directory,n) && path[n]=='/') return path+n+1;
    return path;
}
static const char *const conflicts[]={N_("新しければ上書き"),N_("名前を変える"),N_("全部上書き"),N_("コピーしない"),N_("別の場所へ")};
static void advance(App *a,Ops *o)
{
    o->running=1;
    while(o->jobs || o->index<o->count) {
        if(!o->jobs) {
            const char *name=o->names[o->index++];
            char *src=path_join(o->directory,name),*dst=o->destination?path_join(o->destination,name):NULL;
            if(!src || (o->destination&&!dst) || job_push(o,src,dst,0)<0) { error(a,o,name); free(src); free(dst); continue; }
            free(src); free(dst);
        }
        Job *j=o->jobs; struct stat st,target; int deleting=o->op==DELETE||o->op==RMDIR;
        if(lstat(j->src,&st)<0) goto failed;
        if(j->finish) {
            if(deleting || j->move) { if(rmdir(j->src)<0) goto failed; }
            if(!deleting && metadata(j->dst,&j->original)<0) goto failed;
            goto next;
        }
        char progress[256]; snprintf(progress,sizeof(progress),T("%s中 %zu/%zu %.150s"),deleting?T("削除"):j->move?T("移動"):T("コピー"),o->index,o->count,shown(o,j->src)); app_notice(a,progress);
        if(deleting) {
            if(!(st.st_mode&0222) && o->phase!=READONLY) {
                o->running=0; o->phase=READONLY; char ask[256]; snprintf(ask,sizeof(ask),T("%.120s は書き込み禁止です。削除しますか？"),shown(o,j->src));
                widgets_confirm(&a->widgets,ask); return;
            }
            o->phase=CONFIRM;
            if(S_ISDIR(st.st_mode)) { o->jobs=j->next; if(children(o,j,1)<0) error(a,o,j->src); job_free(j); continue; }
            if(unlink(j->src)<0) goto failed;
            ++o->done; goto next;
        }
        if(!lstat(j->dst,&target)) {
            if(st.st_dev==target.st_dev && st.st_ino==target.st_ino) { errno=EINVAL; goto failed; }
            if(!o->overwrite && o->phase!=NEWNAME) {
                o->running=0; o->phase=CONFLICT; snprintf(o->label,sizeof(o->label),T("%.100s は行先にあります"),shown(o,j->src));
                const char *choices[5]; for(int i=0;i<5;i++) choices[i]=T(conflicts[i]);
                widgets_choice(&a->widgets,o->label,choices,5); return;
            }
            if(S_ISDIR(st.st_mode)!=S_ISDIR(target.st_mode)) { errno=EISDIR; goto failed; }
        } else if(errno!=ENOENT) goto failed;
        o->phase=INPUT;
        if(S_ISDIR(st.st_mode) && descendant(j->src,j->dst)) { errno=EINVAL; goto failed; }
        if(j->move) {
            if(!rename(j->src,j->dst)) { ++o->done; goto next; }
            /* Copy and remove only across file systems or to merge into an existing directory. */
            if(errno!=EXDEV && errno!=ENOTEMPTY && errno!=EEXIST) goto failed;
        }
        if(S_ISDIR(st.st_mode)) {
            if(mkdir(j->dst,0700)<0 && errno!=EEXIST) goto failed;
            o->jobs=j->next; if(children(o,j,0)<0) error(a,o,j->src); job_free(j); continue;
        }
        if(copy_leaf(j->src,j->dst,&st)<0) goto failed;
        if(j->move && unlink(j->src)<0) goto failed;
        ++o->done; goto next;
failed: error(a,o,j->src);
next: o->jobs=j->next; job_free(j); return;
    }
    finish(a,o);
}
static size_t count_files(const char *path)
{
    struct stat st; if(lstat(path,&st)<0) return 0;
    if(!S_ISDIR(st.st_mode)) return 1;
    DIR *d=opendir(path); if(!d) return 0;
    size_t n=0; struct dirent *e;
    while((e=readdir(d))) if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")) {
        char *p=path_join(path,e->d_name); if(p) { n+=count_files(p); free(p); }
    }
    closedir(d); return n;
}
static char *shell_quote(const char *name)
{
    int plain=*name!=0;
    for(const char *p=name;*p;++p) if(!(*p=='/'||*p=='.'||*p=='_'||*p=='-'||*p=='+'||*p==','||(*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z'))) plain=0;
    if(plain) return strdup(name);
    size_t n=3; for(const char *p=name;*p;++p) n+=*p=='\''?4:1;
    char *q=malloc(n),*out=q; if(!q) return NULL;
    *out++='\''; for(const char *p=name;*p;++p) { if(*p=='\'') { memcpy(out,"'\\''",4); out+=4; } else *out++=*p; } *out++='\''; *out=0; return q;
}
/* 1.01: rmp-jukebox draws on the display itself, so x starts it without a
 * terminal around it, as launcher_jukebox does (in fbterm the two fight over
 * the screen). The command's first word, its directory and quotes aside. */
static int runs_jukebox(const char *command)
{
    while(*command==' '||*command=='\t') ++command;
    size_t n=strcspn(command," \t;&|<>");
    if(n>=2 && command[0]=='\'' && command[n-1]=='\'') { ++command; n-=2; }
    const char *base=command;
    for(size_t i=0;i<n;++i) if(command[i]=='/') base=command+i+1;
    return (size_t)(command+n-base)==11 && !memcmp(base,"rmp-jukebox",11);
}
static int key(Screen *s,App *a,const Key *k)
{
    Ops *o=s->self;
    if(o->running) { if(k->key==KESC) { filer_reload(o->filer,a); app_notice(a,T("操作を中止しました")); return 1; } return 0; }
    WidgetResult r=widgets_key(&a->widgets,k);
    if(r==WIDGET_CANCEL) {
        if(o->phase==READONLY) { Job *j=o->jobs; o->jobs=j->next; job_free(j); o->phase=CONFIRM; advance(a,o); return 0; }
        if(o->phase==CONFLICT || o->phase==NEWNAME || o->phase==RELOCATE) { filer_reload(o->filer,a); app_notice(a,T("操作を中止しました")); }
        return 1;
    }
    if(r!=WIDGET_ACCEPT) return 0;
    if(o->phase==CONFLICT) {
        int choice=a->widgets.selected, skipped=0;
        if(choice==1 || choice==4) { o->phase=choice==1?NEWNAME:RELOCATE; widgets_input_history(&a->widgets,choice==1?T("新しい名前"):T("古い方の移動先"),"",o->directory,choice==1?WIDGET_HISTORY_NAME:WIDGET_HISTORY_DESTINATION); return 0; }
        if(choice==3) { Job *j=o->jobs; o->jobs=j->next; job_free(j); skipped=1; }
        else if(choice==2) o->overwrite=1;
        else { struct stat x,y; Job *j=o->jobs;
            if(lstat(j->src,&x)<0 || lstat(j->dst,&y)<0 || x.st_mtim.tv_sec<y.st_mtim.tv_sec || (x.st_mtim.tv_sec==y.st_mtim.tv_sec&&x.st_mtim.tv_nsec<=y.st_mtim.tv_nsec)) { o->jobs=j->next; job_free(j); skipped=1; }
        }
        o->phase=skipped?INPUT:NEWNAME; advance(a,o); return 0;
    }
    if(o->phase==NEWNAME || o->phase==RELOCATE) {
        Job *j=o->jobs; char *p=path_join(o->phase==NEWNAME?o->destination:o->directory,a->widgets.text); struct stat st;
        if(!p || !*a->widgets.text) { free(p); app_notice(a,T("名前が空です")); return 1; }
        if(o->phase==RELOCATE) {
            char *base=strrchr(j->dst,'/'); char *target=path_join(p,base?base+1:j->dst); free(p); p=target;
            if(!p || !lstat(p,&st) || errno!=ENOENT) { errno=EEXIST; error(a,o,p?p:""); free(p); return 1; }
            if(job_push(o,j->dst,p,0)<0) { error(a,o,p); free(p); return 1; }
            o->jobs->move=1; free(p);
        } else {
            if(!lstat(p,&st) || errno!=ENOENT) { errno=EEXIST; error(a,o,p); free(p); return 1; }
            free(j->dst); j->dst=p;
        }
        o->phase=NEWNAME; advance(a,o); return 0;
    }
    if(o->op==COPY || o->op==MOVE) {
        o->destination=path_join(o->directory,a->widgets.text); struct stat st;
        if(!o->destination || stat(o->destination,&st)<0 || !S_ISDIR(st.st_mode)) { errno=ENOTDIR; error(a,o,a->widgets.text); return 1; }
        free(last_destination); last_destination=strdup(o->destination); advance(a,o); return 0;
    }
    if(o->op==DELETE || o->op==RMDIR) { advance(a,o); return 0; }
    if(o->op==EXECUTE || o->op==SHELL) {
        const char *shell=getenv("SHELL"); if(!shell||!*shell) shell="/bin/sh";
        const char *argv[]={"/bin/sh","-c",a->widgets.text,NULL}; const char *interactive[]={shell,NULL};
        int interactive_shell=o->op==SHELL && !*a->widgets.text;
        /* A command waits for a key afterwards; an interactive shell returns at exit. */
        unsigned flags=APP_EXTERNAL_TERMINAL|(interactive_shell?0u:APP_EXTERNAL_PAUSE);
        if(o->op==EXECUTE && runs_jukebox(a->widgets.text)) flags=0;
        if(app_request_external_at(a,interactive_shell?interactive:argv,flags,o->directory)<0) error(a,o,T("外部コマンド"));
        return 1;
    }
    char *p=path_join(o->directory,a->widgets.text);
    if(!p || !*a->widgets.text) { free(p); app_notice(a,T("名前が空です")); return 1; }
    if(o->op==MKDIR) { if(mkdirs(p)<0) error(a,o,p); else ++o->done; }
    else {
        char *src=path_join(o->directory,o->current); struct stat st;
        if(!src) error(a,o,p);
        else if(!lstat(p,&st) || errno!=ENOENT) { errno=EEXIST; error(a,o,p); }
        else if(rename(src,p)<0) error(a,o,p); else ++o->done;
        free(src);
    }
    free(p);
    if(o->op==RENAME && ++o->index<o->count) {
        filer_reload(o->filer,a);
        free(o->current); o->current=strdup(o->names[o->index]);
        if(!o->current) { errno=ENOMEM; error(a,o,""); finish(a,o); return 0; }
        widgets_input_history(&a->widgets,T("新しい名前"),o->current,o->directory,WIDGET_HISTORY_NAME); return 0;
    }
    finish(a,o); return 0;
}
static void operation_tick(Screen *s,App *a,double now)
{
    (void)now; Ops *o=s->self; if(o->running) advance(a,o); a->changed=1;
}
static Ops *create(App *a,const FileSelection *sel,Operation op)
{
    Ops *o=calloc(1,sizeof(*o)); if(!o) return NULL;
    o->screen=(Screen){o,key,draw,0,operation_tick,destroy}; o->filer=app_top(a); o->op=op;
    o->directory=strdup(sel->directory); o->current=strdup(sel->current?sel->current:"");
    o->count=sel->count; o->names=calloc(o->count?o->count:1,sizeof(char *));
    if(!o->directory || !o->current || !o->names) goto fail;
    for(size_t i=0;i<o->count;++i) if(!(o->names[i]=strdup(sel->names[i]))) goto fail;
    if(app_push(a,&o->screen)<0) goto fail;
    return o;
fail: destroy(&o->screen); app_notice(a,T("メモリが足りません")); return NULL;
}
static int transfer(App *a,const FileSelection *s,int move,const char *dest)
{
    if(!s->count) { app_notice(a,T("対象がありません")); return -1; }
    Ops *o=create(a,s,move?MOVE:COPY); if(!o) return -1;
    if(dest) { o->destination=path_join(o->directory,dest); if(!o->destination) { app_pop(a); return -1; } advance(a,o); }
    else { const char *other=filer_destination(o->filer); widgets_input_history(&a->widgets,move?T("移動先"):T("コピー先"),other?other:last_destination?last_destination:"",o->directory,WIDGET_HISTORY_DESTINATION); }
    return 0;
}
int fileops_transfer_to(App *a,const FileSelection *s,const char *d,int move) { return transfer(a,s,move,d); }
int fileops_copy(App *a,const FileSelection *s) { return transfer(a,s,0,NULL); }
int fileops_move(App *a,const FileSelection *s) { return transfer(a,s,1,NULL); }
int fileops_delete(App *a,const FileSelection *s)
{
    if(!s->count) { app_notice(a,T("対象がありません")); return -1; }
    Ops *o=create(a,s,DELETE); if(!o) return -1;
    /* Item 36: d removes directories too, with what they hold. */
    o->phase=CONFIRM; size_t files=0,dirs=0,inside=0;
    for(size_t i=0;i<o->count;++i) {
        char *p=path_join(o->directory,o->names[i]); struct stat st;
        if(p&&!lstat(p,&st)) { if(S_ISDIR(st.st_mode)) { ++dirs; inside+=count_files(p); } else ++files; }
        free(p);
    }
    char msg[160];
    if(!dirs) snprintf(msg,sizeof(msg),T("%zu 個のファイルを削除しますか？"),files);
    else if(!files) snprintf(msg,sizeof(msg),T("ディレクトリ %zu 個（中のファイル %zu 個）を削除しますか？"),dirs,inside);
    else snprintf(msg,sizeof(msg),T("ファイル %zu 個とディレクトリ %zu 個（中のファイル %zu 個）を削除しますか？"),files,dirs,inside);
    widgets_confirm(&a->widgets,msg); return 0;
}
int fileops_rmdir(App *a,const FileSelection *s)
{
    if(!safe_name(s->current)) { app_notice(a,T("対象がありません")); return -1; }
    const char *names[]={s->current}; FileSelection one={s->directory,s->current,names,1};
    Ops *o=create(a,&one,RMDIR); if(!o) return -1;
    char *p=path_join(o->directory,o->current); struct stat st;
    if(!p || lstat(p,&st)<0 || (!S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode))) { free(p); app_pop(a); app_notice(a,T("ディレクトリではありません")); return -1; }
    char msg[256];
    if(S_ISLNK(st.st_mode)) snprintf(msg,sizeof(msg),T("リンク %.100s を削除しますか？（指し先は残します）"),o->current);
    else snprintf(msg,sizeof(msg),T("ディレクトリ %.100s（%zu 個のファイル）を削除しますか？"),o->current,count_files(p));
    free(p);
    o->phase=CONFIRM; widgets_confirm(&a->widgets,msg); return 0;
}
int fileops_rename(App *a,const FileSelection *s)
{
    if(!s->count) { app_notice(a,T("対象がありません")); return -1; }
    Ops *o=create(a,s,RENAME); if(!o) return -1;
    free(o->current); o->current=strdup(o->names[0]);
    if(!o->current) { app_pop(a); return -1; }
    widgets_input_history(&a->widgets,T("新しい名前"),o->current,o->directory,WIDGET_HISTORY_NAME); return 0;
}
int fileops_mkdir(App *a,const FileSelection *s)
{ Ops *o=create(a,s,MKDIR); if(!o) return -1; widgets_input_history(&a->widgets,T("ディレクトリ名"),"",o->directory,WIDGET_HISTORY_NAME); return 0; }
int fileops_execute(App *a,const FileSelection *s)
{
    if(!safe_name(s->current)) { app_notice(a,T("対象がありません")); return -1; }
    Ops *o=create(a,s,EXECUTE); if(!o) return -1;
    /* Executable: "./name" with the cursor after it (arguments follow).
     * Otherwise " name" with the cursor before it (the command goes first). */
    char *p=path_join(o->directory,o->current),*quoted=shell_quote(o->current); struct stat st;
    int runnable=p && !stat(p,&st) && S_ISREG(st.st_mode) && (st.st_mode&0111);
    size_t n=quoted?strlen(quoted)+4:1; char *initial=malloc(n);
    if(initial) snprintf(initial,n,"%s%s",runnable?"./":" ",quoted?quoted:"");
    widgets_input(&a->widgets,T("実行"),initial?initial:"",o->directory);
    if(!runnable) a->widgets.cursor=0;
    free(p); free(quoted); free(initial); return 0;
}
int fileops_shell(App *a,const FileSelection *s)
{ Ops *o=create(a,s,SHELL); if(!o) return -1; widgets_input(&a->widgets,T("シェル"),"",o->directory); return 0; }
/* ---- i: the cursor's file (revision 1 §1 8) ----------------------------- */
static const char *kind_name(LauncherKind k)
{
    switch(k) {
    case LAUNCHER_IMAGE: return T("画像"); case LAUNCHER_PDF: return "PDF"; case LAUNCHER_SHEET: return T("表計算");
    case LAUNCHER_DOC: return T("文書（Word）"); case LAUNCHER_SLIDE: return T("スライド（PowerPoint）");
    case LAUNCHER_ARCHIVE: return T("書庫"); case LAUNCHER_EXTERNAL: return T("外部コマンドで開く形式");
    case LAUNCHER_NOVEL: return T("小説"); case LAUNCHER_MIDI: return T("MIDI（Retro MIDI Player）");
    case LAUNCHER_TEXT: default: return T("テキスト");
    }
}
/* What the first bytes say, or NULL when they say nothing in particular. */
static const char *magic_name(const char *path)
{
    unsigned char b[16]={0}; FILE *f=fopen(path,"rb"); if(!f) return NULL;
    size_t n=fread(b,1,sizeof(b),f); fclose(f);
    static const struct { const char *magic; size_t length; const char *name; } table[]={
        {"%PDF",4,"PDF"},{"PK\3\4",4,N_("ZIP（Office の文書を含む）")},{"\x89PNG",4,N_("PNG 画像")},{"\xff\xd8\xff",3,N_("JPEG 画像")},
        {"GIF8",4,N_("GIF 画像")},{"BM",2,N_("BMP 画像")},{"RIFF",4,N_("RIFF（WebP・WAV など）")},{"MThd",4,N_("標準 MIDI")},
        {"\x1f\x8b",2,"gzip"},{"BZh",3,"bzip2"},{"\xfd" "7zXZ",6,"xz"},{"7z\xbc\xaf",4,"7z"},
        {"\x7f" "ELF",4,N_("ELF 実行形式")},{"#!",2,N_("スクリプト")},
    };
    for(size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if(n>=table[i].length && !memcmp(b,table[i].magic,table[i].length)) return T(table[i].name);
    if(n>=7 && b[2]=='-' && b[3]=='l' && b[4]=='h' && b[6]=='-') return T("LHa 書庫");
    if(n>=4 && !memcmp(b,"RCM-",4)) return T("RCP（レコンポーザ）");
    return NULL;
}
static void when(char *out,size_t size,int known,time_t t)
{
    struct tm tm;
    if(!known || !localtime_r(&t,&tm) || !strftime(out,size,"%Y-%m-%d %H:%M:%S",&tm)) snprintf(out,size,"%s",T("不明"));
}
/* The creation (birth) time: statx's btime on Linux, st_birthtime on macOS. */
static int birth(const char *path,time_t *t)
{
#if defined(__linux__) && defined(STATX_BTIME)
    struct statx x;
    if(!statx(AT_FDCWD,path,AT_SYMLINK_NOFOLLOW,STATX_BTIME,&x) && (x.stx_mask & STATX_BTIME)) { *t=(time_t)x.stx_btime.tv_sec; return 1; }
    return 0;
#elif defined(__APPLE__)
    struct stat st; if(lstat(path,&st)) return 0;
    *t=st.st_birthtimespec.tv_sec; return 1;
#else
    (void)path; (void)t; return 0;
#endif
}
int fileops_file_info(App *a,const FileSelection *s)
{
    if(!s || !s->current || !*s->current) { app_notice(a,T("対象がありません")); return -1; }
    char *path=path_join(s->directory,s->current); struct stat ls,st;
    if(!path || lstat(path,&ls)) { free(path); app_notice(a,T("ファイルの情報を読めません")); return -1; }
    int link=S_ISLNK(ls.st_mode), followed=link ? !stat(path,&st) : (st=ls,1);
    const struct stat *t=followed ? &st : &ls;
    char kind[256],size[128],created[64],modified[64],accessed[64],attributes[160],owner[160],target[1024];
    const char *base=strrchr(s->current,'/'); base=base ? base+1 : s->current;
    const char *dot=strrchr(base,'.'); if(dot==base) dot=NULL;
    if(S_ISDIR(t->st_mode)) snprintf(kind,sizeof(kind),"%s",T("ディレクトリ"));
    else if(!S_ISREG(t->st_mode)) snprintf(kind,sizeof(kind),"%s",S_ISFIFO(t->st_mode)?T("名前付きパイプ"):S_ISSOCK(t->st_mode)?T("ソケット"):S_ISCHR(t->st_mode)?T("キャラクタ装置"):S_ISBLK(t->st_mode)?T("ブロック装置"):link?T("リンク切れ"):T("その他"));
    else {
        const char *content=magic_name(path); int binary=launcher_binary(path);
        if(!content) content=!t->st_size?T("空"):binary==1?T("バイナリ"):binary==0?T("テキスト"):T("読めません");
        snprintf(kind,sizeof(kind),T("%s（拡張子 %s／中身 %s）"),kind_name(launcher_kind(path)),dot?dot:T("なし"),content);
    }
    /* Keep the entire description plus the link prefix without truncation. */
    char linked_kind[sizeof(kind) + 64];
    if(link) snprintf(linked_kind,sizeof(linked_kind),T("シンボリックリンク → %s"),kind);
    unsigned long long bytes=(unsigned long long)t->st_size;
    snprintf(size,sizeof(size),T("%llu バイト（%.1f KB）"),bytes,bytes/1024.0);
    time_t born=0; when(created,sizeof(created),birth(path,&born),born);
    when(modified,sizeof(modified),1,t->st_mtime); when(accessed,sizeof(accessed),1,t->st_atime);
    char mode[11]="---------"; static const char bits[]="rwxrwxrwx";
    for(int i=0;i<9;++i) if(t->st_mode & (0400>>i)) mode[i]=bits[i];
    char flags[128]="";
    if(!(t->st_mode & S_IWUSR)) strcat(flags,T("・書き込み禁止"));
    if(base[0]=='.') strcat(flags,T("・隠し"));
    if(!S_ISDIR(t->st_mode) && (t->st_mode & 0111)) strcat(flags,T("・実行"));
    if(link) strcat(flags,T("・リンク"));
    snprintf(attributes,sizeof(attributes),T("%s（%s）"),mode,flags[0]?flags+3:T("なし"));
    struct passwd *pw=getpwuid(ls.st_uid); struct group *gr=getgrgid(ls.st_gid);
    snprintf(owner,sizeof(owner),"%s (%lu) / %s (%lu)",pw?pw->pw_name:"?",(unsigned long)ls.st_uid,gr?gr->gr_name:"?",(unsigned long)ls.st_gid);
    if(link) { ssize_t n=readlink(path,target,sizeof(target)-1); if(n<0) n=0; target[n]=0; }
    else snprintf(target,sizeof(target),"%s",T("（リンクではありません）"));
    const char *names[]={T("名前"),T("場所"),T("種類"),T("サイズ"),T("作成日時"),T("更新日時"),T("アクセス日時"),T("属性"),T("持ち主とグループ"),T("リンク先")};
    const char *values[]={base,s->directory,link?linked_kind:kind,size,created,modified,accessed,attributes,owner,target};
    widgets_info(&a->widgets,T("ファイルの情報"),names,values,10);
    free(path); return 0;
}
int fileops_write_directory(App *a,const FileSelection *s)
{ (void)s; app_notice(a,T("Linux では使えません")); return 0; }

/* Keep T2 in fileops.o: T1 public-API checks link that object directly. */
#include "fileops_attributes.c"
#include "fileops_tree.c"
