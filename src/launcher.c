// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "launcher.h"
#include "app.h"
#include "archive.h"
#include "docview.h"
#include "imageview.h"
#include "noveledit.h"
#include "pdfview.h"
#include "sheetview.h"
#include "slideview.h"
#include "textview.h"
#include "hexview.h"
#include "widgets.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define LAUNCHER_ENTRIES 128
typedef struct { char *extension, *command; LauncherKind kind; } Entry;
static Entry table[LAUNCHER_ENTRIES];
static size_t entries;
static int ready;

static const struct { const char *name; LauncherKind kind; } internal[]={
    {"textview",LAUNCHER_TEXT},{"imageview",LAUNCHER_IMAGE},{"pdfview",LAUNCHER_PDF},
    {"sheetview",LAUNCHER_SHEET},{"docview",LAUNCHER_DOC},{"slideview",LAUNCHER_SLIDE},
    {"noveledit",LAUNCHER_NOVEL},{"archive",LAUNCHER_ARCHIVE},{"jukebox",LAUNCHER_MIDI},
};
static const struct { const char *extension, *command; } defaults[]={
    {".tar","archive"},{".tar.gz","archive"},{".tgz","archive"},{".tar.bz2","archive"},{".tbz","archive"},
    {".tar.xz","archive"},{".txz","archive"},{".zip","archive"},{".lzh","archive"},{".lha","archive"},{".7z","archive"},
    {".jpg","imageview"},{".jpeg","imageview"},{".png","imageview"},{".gif","imageview"},{".bmp","imageview"},
    {".webp","imageview"},{".mag","imageview"},{".mki","imageview"},{".pi","imageview"},
    {".pdf","pdfview"},{".csv","sheetview"},{".tsv","sheetview"},{".xlsx","sheetview"},
    {".docx","docview"},{".pptx","slideview"},
    {".mid","jukebox"},{".rcp","jukebox"},{".r36","jukebox"},{".g36","jukebox"},
};

static void clear(void)
{
    for(size_t i=0;i<entries;++i) { free(table[i].extension); free(table[i].command); }
    memset(table,0,sizeof(table)); entries=0;
}
static int add(const char *extension,const char *command)
{
    while(*extension=='*') ++extension;
    if(!*extension || strchr(extension,'/') || !command || !*command) return -1;
    size_t n=strlen(extension)+2; char *ext=malloc(n),*cmd=strdup(command);
    if(!ext || !cmd) { free(ext); free(cmd); return -1; }
    snprintf(ext,n,"%s%s",*extension=='.' ? "" : ".",extension);
    if(!ext[1]) { free(ext); free(cmd); return -1; }
    LauncherKind kind=LAUNCHER_EXTERNAL;
    for(size_t i=0;i<sizeof(internal)/sizeof(*internal);++i) if(!strcmp(command,internal[i].name)) kind=internal[i].kind;
    for(size_t i=0;i<entries;++i) if(!strcasecmp(table[i].extension,ext)) {
        free(table[i].extension); free(table[i].command); table[i]=(Entry){ext,cmd,kind}; return 0;
    }
    if(entries==LAUNCHER_ENTRIES) { free(ext); free(cmd); return -1; }
    table[entries++]=(Entry){ext,cmd,kind}; return 0;
}
static void init(void)
{
    if(ready) return;
    ready=1;
    for(size_t i=0;i<sizeof(defaults)/sizeof(*defaults);++i) add(defaults[i].extension,defaults[i].command);
}
void launcher_reset(void) { clear(); ready=0; init(); }
size_t launcher_count(void) { init(); return entries; }
int launcher_entry(size_t i,const char **extension,const char **command)
{
    init();
    if(i>=entries) return -1;
    if(extension) *extension=table[i].extension;
    if(command) *command=table[i].command;
    return 0;
}
static const Entry *lookup(const char *path)
{
    init();
    const char *base=strrchr(path,'/'); base=base ? base+1 : path;
    size_t len=strlen(base),best=0; const Entry *found=NULL;
    for(size_t i=0;i<entries;++i) {
        size_t n=strlen(table[i].extension);
        /* The extension must leave a name before it (".zip" alone is not a zip). */
        if(n<len && n>best && !strcasecmp(base+len-n,table[i].extension)) { best=n; found=&table[i]; }
    }
    return found;
}
LauncherKind launcher_kind(const char *path)
{
    const Entry *e=path ? lookup(path) : NULL;
    return e ? e->kind : LAUNCHER_TEXT;
}
int launcher_register(App *app,const char *extension,const char *command)
{
    init();
    if(!extension || !command || add(extension,command)<0) { if(app) app_notice(app,T("ランチャに登録できません")); return -1; }
    return 0;
}

static char *quoted(const char *s)
{
    size_t n=3; for(const char *p=s;*p;++p) n+=*p=='\'' ? 4 : 1;
    char *q=malloc(n),*o=q; if(!q) return NULL;
    *o++='\'';
    for(const char *p=s;*p;++p) { if(*p=='\'') { memcpy(o,"'\\''",4); o+=4; } else *o++=*p; }
    *o++='\''; *o=0; return q;
}
/* %C: the file's path (quoted); %% a percent. Other macros belong to 1c. */
static char *expand(const char *command,const char *path)
{
    char *q=quoted(path); if(!q) return NULL;
    size_t n=strlen(command)+2,ql=strlen(q); int used=0;
    for(const char *p=command;*p;++p) if(p[0]=='%' && p[1]=='C') n+=ql;
    char *out=malloc(n+ql+2),*o=out;
    if(!out) { free(q); return NULL; }
    for(const char *p=command;*p;++p) {
        if(p[0]=='%' && p[1]=='C') { memcpy(o,q,ql); o+=ql; ++p; used=1; }
        else if(p[0]=='%' && p[1]=='%') { *o++='%'; ++p; }
        else *o++=*p;
    }
    if(!used) { *o++=' '; memcpy(o,q,ql); o+=ql; }
    *o=0; free(q); return out;
}

static int push(App *app,Screen *screen)
{
    if(!screen) return -1;
    if(app_push(app,screen)==0) return 0;
    if(screen->destroy) screen->destroy(screen);
    app_notice(app,T("画面を開けません"));
    return -1;
}
/* A byte that can be the high byte of a UTF-16 character in text: Latin,
 * Greek, Cyrillic, punctuation and symbols, kana, CJK, Hangul, surrogates
 * (emoji) and the half/full-width forms; not 0x05..0x1f 0x28..0x2f 0xa0..0xab
 * 0xe0..0xf8. */
static int text_high(unsigned c)
{ return c<=0x04 || (c>=0x20 && c<=0x27) || (c>=0x30 && c<=0x9f) || (c>=0xac && c<=0xdf) || c>=0xf9; }
static int printable(unsigned c) { return (c>=0x20 && c<0x7f) || c=='\t' || c=='\n' || c=='\r'; }
int launcher_binary(const char *path)
{
    FILE *f=path ? fopen(path,"rb") : NULL; if(!f) return -1;
    unsigned char b[8192]; size_t n=fread(b,1,sizeof(b),f); int failed=ferror(f); fclose(f);
    if(failed) return -1;
    if(n>=2 && ((b[0]==0xff && b[1]==0xfe) || (b[0]==0xfe && b[1]==0xff))) return 0;
    size_t control=0,nul=0;
    for(size_t i=0;i<n;++i) {
        unsigned char c=b[i];
        if(!c) ++nul;
        else if((c<0x20 && c!='\t' && c!='\n' && c!='\r' && c!='\f' && c!='\v' && c!='\b' && c!=0x1b && c!=0x1a) || c==0x7f) ++control;
    }
    if(!nul) return control*16>n;
    /* NULs: text only as UTF-16 without a byte order mark (LE, then BE):
     * at most one pair in 32 implausible, and at least one in 64 an ASCII
     * character (line ends, spaces, digits). */
    if(n%2) return 1;
    size_t pairs=n/2;
    for(int hi_at=1;hi_at>=0;--hi_at) {
        size_t bad=0,ascii=0;
        for(size_t k=0;k<pairs;++k) {
            unsigned hi=b[2*k+hi_at],lo=b[2*k+1-hi_at];
            if(!hi) { if(printable(lo)) ++ascii; else ++bad; }
            else if(!text_high(hi)) ++bad;
        }
        if(bad*32<=pairs && ascii*64>=pairs) return 0;
    }
    return 1;
}
static int on_path(const char *name)
{
    const char *path=getenv("PATH"); if(!path || !*path) path="/bin:/usr/bin";
    for(;;) {
        const char *end=strchr(path,':'); size_t n=end ? (size_t)(end-path) : strlen(path);
        size_t size=n+strlen(name)+3; char *candidate=malloc(size); if(!candidate) return 0;
        snprintf(candidate,size,"%.*s%s%s",(int)n,path,n ? "/" : "",name);
        struct stat st; int yes=!stat(candidate,&st) && S_ISREG(st.st_mode) && !access(candidate,X_OK); free(candidate);
        if(yes) return 1;
        if(!end) return 0;
        path=end+1;
    }
}
static char *parent_of(const char *path)
{
    char *dir=strdup(path); char *slash=dir ? strrchr(dir,'/') : NULL;
    if(!slash) { free(dir); return NULL; }
    if(slash==dir) slash[1]=0; else *slash=0;
    return dir;
}
int launcher_jukebox(App *app,const char *path)
{
    struct stat st;
    if(!path || stat(path,&st)<0) { app_notice(app,T("ファイルを開けません")); return -1; }
    if(S_ISDIR(st.st_mode)) { app_notice(app,T("ディレクトリです")); return -1; }
    if(!on_path("rmp-jukebox")) { app_error(app,T("rmp-jukebox が見つかりません（PATH にありません）")); return -1; }
    char *dir=parent_of(path);
    const char *argv[]={"rmp-jukebox",path,NULL};
    /* No APP_EXTERNAL_TERMINAL: rmp-jukebox draws on the display itself. */
    int r=app_request_external_at(app,argv,0,dir);
    free(dir);
    if(r<0) app_notice(app,T("外部コマンドを起動できません"));
    return r;
}
/* The question for an unknown binary file: a screen over the one below that
 * only waits for the choice (widgets_choice), then opens the viewer. */
typedef struct { Screen screen, *below; char *path; } Chooser;
static const char *const binary_options[]={N_("テキスト表示(T)"),N_("バイナリ表示(B)"),N_("キャンセル")};
static int chooser_key(Screen *s,App *a,const Key *k)
{
    Chooser *c=s->self;
    WidgetResult r=widgets_key(&a->widgets,k);
    if(r==WIDGET_CANCEL) return 1;
    if(r!=WIDGET_ACCEPT) return 0;
    int choice=a->widgets.selected; char *path=c->path; c->path=NULL;
    app_pop(a);   /* destroys c */
    if(choice==0) push(a,textview_create(a,path));
    else if(choice==1) push(a,hexview_create(a,path));
    free(path); return 0;
}
static void chooser_draw(Screen *s,App *a,Canvas *canvas,Cells *cells)
{ Chooser *c=s->self; if(c->below && c->below->draw) c->below->draw(c->below,a,canvas,cells); }
static void chooser_destroy(Screen *s) { Chooser *c=s->self; free(c->path); free(c); }
static int ask_binary(App *app,const char *path)
{
    Chooser *c=calloc(1,sizeof(*c)); Screen *below=app_top(app);
    if(!c || !(c->path=strdup(path))) { free(c); app_notice(app,T("メモリが足りません")); return -1; }
    c->below=below; c->screen=(Screen){c,chooser_key,chooser_draw,below ? below->pixel : 0,NULL,chooser_destroy};
    if(app_push(app,&c->screen)<0) { chooser_destroy(&c->screen); app_notice(app,T("画面を開けません")); return -1; }
    const char *options[3];
    for(int i=0;i<3;++i) options[i]=T(binary_options[i]);
    widgets_choice(&app->widgets,T("このファイルはバイナリ形式の可能性があります。\n参照方法を選んでください。"),options,3);
    return 0;
}
int launcher_open(App *app,const char *path)
{
    struct stat st;
    if(!path || stat(path,&st)<0) { app_notice(app,T("ファイルを開けません")); return -1; }
    if(S_ISDIR(st.st_mode)) { app_notice(app,T("ディレクトリです")); return -1; }
    const Entry *e=lookup(path);
    if(!e && launcher_binary(path)==1) return ask_binary(app,path);
    switch(e ? e->kind : LAUNCHER_TEXT) {
    case LAUNCHER_ARCHIVE: return archive_browse(app,path);
    case LAUNCHER_IMAGE: return push(app,imageview_create(app,path));
    case LAUNCHER_PDF: return push(app,pdfview_create(app,path));
    case LAUNCHER_SHEET: return push(app,sheetview_create(app,path));
    case LAUNCHER_DOC: return push(app,docview_create(app,path));
    case LAUNCHER_SLIDE: return push(app,slideview_create(app,path));
    case LAUNCHER_NOVEL: return push(app,noveledit_create(app,path));
    case LAUNCHER_MIDI: return launcher_jukebox(app,path);
    case LAUNCHER_EXTERNAL: {
        char *command=expand(e->command,path);
        char *dir=parent_of(path);
        const char *argv[]={"/bin/sh","-c",command,NULL};
        int r=command ? app_request_external_at(app,argv,APP_EXTERNAL_TERMINAL,dir) : -1;
        free(command); free(dir);
        if(r<0) app_notice(app,T("外部コマンドを起動できません"));
        return r;
    }
    case LAUNCHER_TEXT: default: return push(app,textview_create(app,path));
    }
}
