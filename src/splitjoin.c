// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "i18n.h"
#include "splitjoin.h"
#include "pdftools.h"
#include "order_box.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { SIZE, NAME, CONFIRM, MIXED, ORDER, CHECK, OVERWRITE, RUN, SCRIPT, SCRIPT_CHECK, SCRIPT_OVERWRITE, SCRIPT_RUN } Phase;
typedef struct {
    Screen screen, *filer;
    char *directory, **names, *base, *output;
    size_t count, pieces, check, index;
    uint64_t chunk, total, written, piece_written;
    int split, width, in, out, partial;
    Phase phase;
    FILE *script;
} Join;
static char *path_of(const Join *j,const char *name)
{
    size_t n=strlen(j->directory)+strlen(name)+2;
    char *p=malloc(n); if(p) snprintf(p,n,"%s/%s",j->directory,name);
    return p;
}
static char *part_name(const Join *j,size_t index)
{
    size_t n=strlen(j->base)+32;
    char *p=malloc(n); if(p) snprintf(p,n,"%s.%0*zu",j->base,j->width,index+1);
    return p;
}
static void destroy(Screen *s)
{
    Join *j=s->self;
    if(j->in>=0) close(j->in);
    if(j->out>=0) close(j->out);
    if(j->script) fclose(j->script);
    if(j->partial && j->output) unlink(j->output);
    if(j->names) for(size_t i=0;i<j->count;++i) free(j->names[i]);
    free(j->names); free(j->directory); free(j->base); free(j->output); free(j);
}
static void draw(Screen *s,App *a,Canvas *canvas,Cells *cells)
{
    Join *j=s->self; j->filer->draw(j->filer,a,canvas,cells);
}
static void finish(App *a,Join *j,const char *message)
{
    Screen *filer=j->filer;
    app_pop(a); filer_reload(filer,a); app_notice(a,message);
}
static void failed(App *a,Join *j)
{
    char msg[256]; snprintf(msg,sizeof(msg),T("書き出しに失敗しました（%s）"),strerror(errno));
    finish(a,j,msg);
}
static int valid_name(const char *p)
{ return *p && strcmp(p,".") && strcmp(p,"..") && !strchr(p,'/'); }
static void ask_name(App *a,Join *j)
{
    j->phase=NAME;
    widgets_input_history(&a->widgets,j->split?T("分割後の名前の頭"):T("結合先の名前"),j->base,j->directory,WIDGET_HISTORY_NAME);
}
/* Protect inputs even when the destination is a hard link or an alias. */
static int source_at(const Join *j,const char *name,const struct stat *target)
{
    for(size_t i=0;i<j->count;++i) {
        if(!strcmp(name,j->names[i])) return 1;
        char *p=path_of(j,j->names[i]); struct stat st;
        int same=p && !stat(p,&st) && st.st_dev==target->st_dev && st.st_ino==target->st_ino;
        free(p); if(same) return 1;
    }
    return 0;
}
/* 0 absent, 1 existing regular output, -1 unsafe/error. */
static int target(Join *j,const char *name)
{
    char *p=path_of(j,name); struct stat st;
    if(!p) { errno=ENOMEM; return -1; }
    int rc=lstat(p,&st); free(p);
    if(rc<0) return errno==ENOENT?0:-1;
    if(source_at(j,name,&st)) { errno=EINVAL; return -2; }
    if(!S_ISREG(st.st_mode)) { errno=EEXIST; return -1; }
    return 1;
}
static int set_output(Join *j,const char *name)
{
    free(j->output); j->output=path_of(j,name);
    if(!j->output) { errno=ENOMEM; return -1; }
    /* Preflight has already asked about existing files; do not follow links. */
    struct stat before;
    int exists=!lstat(j->output,&before);
    int fd=open(j->output,O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK|(exists?0:O_EXCL),0666);
    if(fd<0) return -1;
    j->partial=!exists;
    struct stat st;
    if(fstat(fd,&st)<0) { close(fd); return -1; }
    if(!S_ISREG(st.st_mode) || source_at(j,name,&st)) { close(fd); j->partial=0; errno=EINVAL; return -1; }
    if(ftruncate(fd,0)<0) { close(fd); return -1; }
    j->out=fd; j->partial=1; return 0;
}
static int open_source(Join *j,size_t index)
{
    char *p=path_of(j,j->names[index]);
    if(!p) { errno=ENOMEM; return -1; }
    j->in=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK); free(p);
    struct stat st;
    if(j->in<0) return -1;
    if(fstat(j->in,&st)<0 || !S_ISREG(st.st_mode)) { close(j->in); j->in=-1; errno=EINVAL; return -1; }
    return 0;
}
static int close_output(Join *j)
{
    int fd=j->out; j->out=-1;
    if(close(fd)<0) return -1;
    j->partial=0; return 0;
}
static void start(App *a,Join *j)
{
    j->index=0; j->phase=RUN;
    if(open_source(j,0)<0) { failed(a,j); return; }
    if(!j->split && set_output(j,j->base)<0) { failed(a,j); return; }
    app_notice(a,j->split?T("分割中 0%"):T("結合中 0%"));
}
static void ask_script(App *a,Join *j)
{
    j->phase=SCRIPT;
    widgets_confirm(&a->widgets,T("結合用のシェルスクリプトを作りますか？"));
}
static void completed(App *a,Join *j)
{
    char msg[160];
    if(j->split) snprintf(msg,sizeof(msg),T("%zu 個に分割しました"),j->pieces);
    else snprintf(msg,sizeof(msg),T("%zu 個を結合しました（%llu バイト）"),j->count,(unsigned long long)j->written);
    finish(a,j,msg);
}
static char *script_name(const Join *j)
{
    size_t n=strlen(j->base)+9; char *p=malloc(n);
    if(p) snprintf(p,n,"%s.join.sh",j->base);
    return p;
}
/* Quote every name, with ./ so a leading '-' cannot be an option to cat. */
static int quoted(FILE *f,const char *name)
{
    if(fputs("'./",f)==EOF) return -1;
    for(const char *p=name;*p;++p) {
        if(*p=='\'') { if(fputs("'\\''",f)==EOF) return -1; }
        else if(fputc(*p,f)==EOF) return -1;
    }
    return fputc('\'',f)==EOF?-1:0;
}
static void start_script(App *a,Join *j)
{
    char *name=script_name(j);
    if(!name) { errno=ENOMEM; failed(a,j); return; }
    int rc=set_output(j,name); free(name);
    if(rc<0) { failed(a,j); return; }
    j->script=fdopen(j->out,"w");
    if(!j->script) { failed(a,j); return; }
    j->out=-1; j->index=0; j->phase=SCRIPT_RUN;
    if(fputs("#!/bin/sh\ncat",j->script)==EOF) failed(a,j);
}
static void ordered(App *a,void *context,const char *const *names,size_t count,int accepted)
{
    Join *j=context;
    if(!accepted) { finish(a,j,T("操作を中止しました")); return; }
    for(size_t i=0;i<count;++i) {
        char *p=strdup(names[i]);
        if(!p) { errno=ENOMEM; failed(a,j); return; }
        free(j->names[i]); j->names[i]=p;
    }
    ask_name(a,j);
}
static void order(App *a,Join *j)
{
    j->phase=ORDER;
    if(order_box_open(a,T("連結するファイルと連結の順番"),(const char *const *)j->names,j->count,ordered,j)<0)
        finish(a,j,T("メモリが足りません"));
}
static int size_value(const char *s,uint64_t *bytes)
{
    if(*s<'0' || *s>'9') return -1;
    errno=0; char *end; unsigned long n=strtoul(s,&end,10);
    if(errno || !n || !*end || end[1] || (*end!='K' && *end!='M')) return -1;
    if(n>(unsigned long)APP_SPLIT_MAX_KB/(*end=='M'?1024:1)) return -1;
    *bytes=(uint64_t)n*(*end=='M'?1024*1024:1024); return 0;
}
static int key(Screen *s,App *a,const Key *k)
{
    Join *j=s->self;
    if(j->phase==RUN || j->phase==CHECK || j->phase==SCRIPT_RUN) {
        if(k->key==KESC) finish(a,j,T("操作を中止しました"));
        return 0;
    }
    WidgetResult r=widgets_key(&a->widgets,k);
    if(r==WIDGET_CANCEL) {
        if(j->phase==SCRIPT || j->phase==SCRIPT_OVERWRITE) completed(a,j);
        else finish(a,j,T("操作を中止しました"));
        return 0;
    }
    if(r!=WIDGET_ACCEPT) return 0;
    switch(j->phase) {
    case SIZE:
        if(size_value(a->widgets.text,&j->chunk)<0) { finish(a,j,T("値が正しくありません")); break; }
        if(j->total<=j->chunk) { finish(a,j,T("分ける必要がありません")); break; }
        j->pieces=(size_t)((j->total-1)/j->chunk+1); j->width=j->pieces>1000?4:3;
        ask_name(a,j); break;
    case NAME: {
        if(!valid_name(a->widgets.text)) { finish(a,j,T("値が正しくありません")); break; }
        char *p=strdup(a->widgets.text);
        if(!p) { errno=ENOMEM; failed(a,j); break; }
        free(j->base); j->base=p;
        if(j->split) {
            char msg[256]; snprintf(msg,sizeof(msg),T("%zu 個に分けます（%.70s.%0*d〜%.70s.%0*zu）。よろしいですか？"),j->pieces,j->base,j->width,1,j->base,j->width,j->pieces);
            j->phase=CONFIRM; widgets_confirm(&a->widgets,msg);
        } else { j->check=0; j->phase=CHECK; }
        break;
    }
    case CONFIRM: j->check=0; j->phase=CHECK; break;
    case OVERWRITE: ++j->check; j->phase=CHECK; break;
    case MIXED: order(a,j); break;
    case SCRIPT: j->check=0; j->phase=SCRIPT_CHECK; break;
    case SCRIPT_OVERWRITE: start_script(a,j); break;
    default: break;
    }
    return 0;
}
static void tick(Screen *s,App *a,double now)
{
    (void)now; Join *j=s->self;
    if(j->phase==CHECK || j->phase==SCRIPT_CHECK) {
        int script=j->phase==SCRIPT_CHECK;
        size_t count=script?1:j->split?j->pieces:1;
        for(int step=0;step<128 && j->check<count;++step) {
            char *name=script?script_name(j):j->split?part_name(j,j->check):strdup(j->base);
            if(!name) { errno=ENOMEM; failed(a,j); return; }
            int rc=target(j,name);
            if(rc<0) { free(name); if(rc==-2) finish(a,j,j->split?T("分割元と同じ名前にはできません"):T("結合元と同じ名前にはできません")); else failed(a,j); return; }
            if(rc) {
                char msg[256]; snprintf(msg,sizeof(msg),T("%.180s は既にあります。上書きしますか？"),name); free(name);
                j->phase=script?SCRIPT_OVERWRITE:OVERWRITE; widgets_confirm(&a->widgets,msg); a->changed=1; return;
            }
            free(name); ++j->check;
        }
        if(j->check==count) { if(script) start_script(a,j); else start(a,j); }
        a->changed=1; return;
    }
    if(j->phase==SCRIPT_RUN) {
        for(int step=0;step<128 && j->index<j->pieces;++step,++j->index) {
            char *p=part_name(j,j->index);
            if(!p) { errno=ENOMEM; failed(a,j); return; }
            int rc=fputc(' ',j->script)==EOF?-1:quoted(j->script,p); free(p);
            if(rc<0) { failed(a,j); return; }
        }
        if(j->index==j->pieces) {
            if(fputs(" > ",j->script)==EOF || quoted(j->script,j->base)<0 || fputc('\n',j->script)==EOF || fflush(j->script)<0 || fchmod(fileno(j->script),0755)<0) { failed(a,j); return; }
            FILE *f=j->script; j->script=NULL;
            if(fclose(f)<0) { failed(a,j); return; }
            j->partial=0; completed(a,j); return;
        }
        a->changed=1; return;
    }
    if(j->phase!=RUN) return;
    /* At most 1 MB per tick, including transitions between empty sources. */
    char buffer[65536]; size_t budget=1024*1024; int steps=0;
    while(budget && steps++<128) {
        if(j->split && j->out<0) {
            char *name=part_name(j,j->index);
            if(!name) { errno=ENOMEM; failed(a,j); return; }
            int rc=set_output(j,name); free(name);
            if(rc<0) { failed(a,j); return; }
        }
        size_t want=sizeof(buffer); if(want>budget) want=budget;
        if(j->split && want>j->chunk-j->piece_written) want=(size_t)(j->chunk-j->piece_written);
        if(j->split && want>j->total-j->written) want=(size_t)(j->total-j->written);
        ssize_t got=read(j->in,buffer,want);
        if(got<0) { if(errno==EINTR) continue; failed(a,j); return; }
        if(!got) {
            if(j->split) { errno=EIO; failed(a,j); return; }
            close(j->in); j->in=-1;
            if(++j->index==j->count) {
                if(close_output(j)<0) { failed(a,j); return; }
                completed(a,j); return;
            }
            if(open_source(j,j->index)<0) { failed(a,j); return; }
            continue;
        }
        size_t offset=0;
        while(offset<(size_t)got) {
            ssize_t put=write(j->out,buffer+offset,(size_t)got-offset);
            if(put<0 && errno==EINTR) continue;
            if(put<=0) { if(!put) errno=EIO; failed(a,j); return; }
            offset+=(size_t)put;
        }
        budget-=(size_t)got; j->written+=(uint64_t)got; j->piece_written+=(uint64_t)got;
        if(j->split && (j->piece_written==j->chunk || j->written==j->total)) {
            if(close_output(j)<0) { failed(a,j); return; }
            ++j->index; j->piece_written=0;
            if(j->written==j->total) {
                close(j->in); j->in=-1; filer_reload(j->filer,a); ask_script(a,j); a->changed=1; return;
            }
        }
    }
    char msg[160]; snprintf(msg,sizeof(msg),T("%s中 %llu/%llu バイト"),j->split?T("分割"):T("結合"),(unsigned long long)j->written,(unsigned long long)j->total);
    app_notice(a,msg); a->changed=1;
}
static int compare(const void *a,const void *b) { return strcmp(*(const char *const *)a,*(const char *const *)b); }
static Join *create(App *a,const FileSelection *s,int split)
{
    Join *j=calloc(1,sizeof(*j)); if(!j) goto failed;
    j->in=j->out=-1; j->split=split; j->filer=app_top(a);
    j->directory=strdup(s->directory); j->count=split?1:s->count;
    j->names=calloc(j->count,sizeof(*j->names));
    if(!j->directory || !j->names) goto cleanup;
    for(size_t i=0;i<j->count;++i) { j->names[i]=strdup(split?s->current:s->names[i]); if(!j->names[i]) goto cleanup; }
    qsort(j->names,j->count,sizeof(*j->names),compare);
    j->base=strdup(j->names[0]); if(!j->base) goto cleanup;
    j->screen=(Screen){j,key,draw,0,tick,destroy};
    if(!app_push(a,&j->screen)) return j;
cleanup: destroy(&(Screen){.self=j});
failed: app_notice(a,T("メモリが足りません")); return NULL;
}
int splitjoin_split(App *a,const FileSelection *s)
{
    if(!s->current || !valid_name(s->current)) { app_notice(a,T("ファイルを選んでください")); return -1; }
    Join *j=create(a,s,1); if(!j) return -1;
    char *p=path_of(j,s->current); struct stat st;
    int ok=p && !lstat(p,&st) && S_ISREG(st.st_mode); free(p);
    if(!ok) { finish(a,j,T("ファイルを選んでください")); return -1; }
    if(pdftools_is_pdf(s->current)) { app_pop(a); return pdftools_open(a,s,PDFTOOLS_SPLIT,-1); }
    j->total=(uint64_t)st.st_size; j->phase=SIZE;
    int kb=a->settings.split_kb;
    if(kb<1 || kb>APP_SPLIT_MAX_KB) kb=APP_SPLIT_DEFAULT_KB;
    char initial[32]; snprintf(initial,sizeof(initial),"%d%s",kb%1024?kb:kb/1024,kb%1024?"K":"M");
    widgets_input(&a->widgets,T("分割の大きさ（1K〜1023M）"),initial,j->directory); return 0;
}
int splitjoin_join(App *a,const FileSelection *s)
{
    if(s->count<2) { app_notice(a,T("結合するファイルをマークしてください")); return -1; }
    Join *j=create(a,s,0); if(!j) return -1;
    size_t pdf=0;
    for(size_t i=0;i<j->count;++i) {
        char *p=path_of(j,j->names[i]); struct stat st;
        int rc=p?lstat(p,&st):-1; free(p);
        if(rc<0) { failed(a,j); return -1; }
        if(S_ISDIR(st.st_mode)) { finish(a,j,T("ディレクトリは結合できません")); return -1; }
        if(!S_ISREG(st.st_mode)) { finish(a,j,T("ファイルを選んでください")); return -1; }
        j->total+=(uint64_t)st.st_size; pdf+=pdftools_is_pdf(j->names[i])!=0;
    }
    if(pdf==j->count) { app_pop(a); return pdftools_open(a,s,PDFTOOLS_MERGE,-1); }
    /* Recognise numbered pieces independently of their lexical display order. */
    char *dot=strrchr(j->base,'.'); int numbered=dot && (strlen(dot+1)==3 || strlen(dot+1)==4);
    size_t stem=dot?(size_t)(dot-j->base):0;
    for(size_t i=0;numbered && i<j->count;++i) {
        const char *n=j->names[i]; char *end; size_t len=strlen(n);
        if(len<stem+1 || (len-stem-1!=3 && len-stem-1!=4) || strncmp(n,j->base,stem+1)) { numbered=0; break; }
        for(const char *q=n+stem+1;*q;++q) if(*q<'0' || *q>'9') numbered=0;
        unsigned long index=strtoul(n+stem+1,&end,10);
        if(*end || !index) numbered=0;
    }
    if(numbered && stem) j->base[stem]=0;
    if(pdf) { j->phase=MIXED; widgets_confirm(&a->widgets,T("PDF とそれ以外が混ざっています。バイトのままつなぎますか？")); }
    else order(a,j);
    return 0;
}
