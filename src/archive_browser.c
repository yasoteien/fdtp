// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Part of archive.o (included from archive.c): the archive browser. The
 * archive's directories are shown like the filer's list (fdscreen). */

#define NO_MEMBER ((size_t)-1)

typedef enum { BR_LOAD, BR_READY, BR_OPEN } BrowserState;
typedef struct {
    Screen screen, *below;
    BrowserState state;
    char *source;          /* the path given to archive_browse */
    Archive arc;
    char *dir;             /* "" or "a/b": the directory shown */
    DirList view;          /* entries[0] is ".." */
    size_t *member;        /* per entry: index into arc.members, or NO_MEMBER */
    size_t cursor; int columns;
    char **temps; size_t temp_count; /* extraction directories in the cache */
    size_t opening;        /* member being opened (BR_OPEN) */
} Browser;

typedef struct { char *name; size_t member; int is_dir; } Child;
static int child_cmp(const void *x,const void *y)
{
    const Child *a=x,*b=y;
    if(a->is_dir!=b->is_dir) return b->is_dir-a->is_dir;
    int c=strcmp(a->name,b->name);
    if(c) return c;
    /* The stored entry (with its size and date) before an implied one. */
    return (a->member==NO_MEMBER)-(b->member==NO_MEMBER);
}

static void view_free(Browser *b)
{
    for(size_t i=0;i<b->view.count;++i) free(b->view.entries[i].name);
    free(b->view.entries); free(b->view.path); free(b->member);
    memset(&b->view,0,sizeof(b->view)); b->member=NULL;
}

/* Rebuild the list for b->dir; the cursor goes to select (or the top). */
static int view_build(Browser *b,const char *select)
{
    size_t prefix=strlen(b->dir),n=0;
    Child *kids=malloc((b->arc.count+1)*sizeof(*kids)); if(!kids) return -1;
    for(size_t i=0;i<b->arc.count;++i) {
        const Member *m=&b->arc.members[i]; const char *rest;
        if(m->unsafe) { if(prefix) continue; kids[n++]=(Child){strdup(m->raw),i,0}; if(!kids[n-1].name) goto fail; continue; }
        if(!prefix) rest=m->path;
        else if(!strncmp(m->path,b->dir,prefix) && m->path[prefix]=='/') rest=m->path+prefix+1;
        else continue;
        if(!*rest) continue;
        const char *slash=strchr(rest,'/');
        size_t len=slash ? (size_t)(slash-rest) : strlen(rest);
        char *name=malloc(len+1); if(!name) goto fail;
        memcpy(name,rest,len); name[len]=0;
        kids[n++]=(Child){name,slash ? NO_MEMBER : i,slash ? 1 : m->is_dir};
    }
    qsort(kids,n,sizeof(*kids),child_cmp);
    size_t unique=0;
    for(size_t i=0;i<n;++i) {
        if(unique && kids[unique-1].is_dir==kids[i].is_dir && !strcmp(kids[unique-1].name,kids[i].name)) { free(kids[i].name); continue; }
        kids[unique++]=kids[i];
    }
    view_free(b);
    b->view.entries=calloc(unique+1,sizeof(*b->view.entries)); b->member=calloc(unique+1,sizeof(*b->member));
    size_t path_n=strlen(b->arc.path)+prefix+4; b->view.path=malloc(path_n);
    if(!b->view.entries || !b->member || !b->view.path) { for(size_t i=0;i<unique;++i) free(kids[i].name); free(kids); view_free(b); return -1; }
    snprintf(b->view.path,path_n,"%s:/%s",b->arc.path,b->dir);
    DirEntry *up=&b->view.entries[0]; up->name=strdup(".."); up->is_dir=1; dirlist_display_name("..",up->display); b->member[0]=NO_MEMBER;
    struct stat st; if(!stat(b->arc.path,&st)) up->mtime=st.st_mtime;
    up->mode=S_IFDIR|0755;
    b->view.count=1;
    for(size_t i=0;i<unique;++i) {
        DirEntry *e=&b->view.entries[b->view.count]; const Member *m=kids[i].member==NO_MEMBER ? NULL : &b->arc.members[kids[i].member];
        e->name=kids[i].name; e->is_dir=kids[i].is_dir; dirlist_display_name(e->name,e->display);
        e->ordinal=b->view.count;
        if(m) { e->size=m->is_dir ? 0 : m->size; e->mtime=m->mtime; e->mode=m->mode|(m->is_dir ? S_IFDIR : m->is_link ? S_IFLNK : S_IFREG); e->is_link=m->is_link; e->link_target=m->link; e->readonly=!(m->mode&0222); }
        else e->mode=S_IFDIR|0755;
        e->hidden=e->name[0]=='.';
        if(!e->is_dir) { ++b->view.file_count; b->view.bytes+=e->size; }
        b->member[b->view.count++]=kids[i].member;
    }
    free(kids);
    if(!up->name) { view_free(b); return -1; }
    b->cursor=0;
    if(select) for(size_t i=1;i<b->view.count;++i) if(!strcmp(b->view.entries[i].name,select)) { b->cursor=i; break; }
    return 0;
fail:
    for(size_t i=0;i<n;++i) free(kids[i].name);
    free(kids); return -1;
}

static void browser_destroy(Screen *s)
{
    Browser *b=s->self;
    for(size_t i=0;i<b->temp_count;++i) { remove_tree(b->temps[i]); free(b->temps[i]); }
    free(b->temps); view_free(b); archive_free(&b->arc); free(b->dir); free(b->source); free(b);
}

static void browser_draw(Screen *s,App *a,Canvas *canvas,Cells *cells)
{
    Browser *b=s->self;
    if(b->state==BR_LOAD || !b->view.entries) { if(b->below && b->below->draw) b->below->draw(b->below,a,canvas,cells); return; }
    fdscreen_build(cells,&b->view,b->cursor,b->columns,(time_t)a->now);
    size_t count=0; uint64_t bytes=0;
    size_t per=(size_t)FDSCREEN_ROWS*(size_t)fdscreen_slots(b->columns),first=b->cursor/per*per;
    for(size_t i=0;i<b->view.count;++i) if(b->view.entries[i].marked) { ++count; bytes+=b->view.entries[i].size; }
    for(size_t i=first;i<b->view.count && i<first+per;++i) if(b->view.entries[i].marked) {
        int slot=(int)((i-first)/FDSCREEN_ROWS),row=6+(int)((i-first)%FDSCREEN_ROWS);
        cells_put(cells,fdscreen_slot_x(b->columns,slot)+1,row,'*',YELLOW,BG);
    }
    const int X=FD_XCOLS;
    for(int x=52+X;x<58+X;++x) cells_put(cells,x,3,' ',WHITE,BG);
    for(int x=64+X;x<78+X;++x) cells_put(cells,x,3,' ',WHITE,BG);
    fdscreen_number(cells,57+X,3,count,52+X); fdscreen_number(cells,77+X,3,bytes,64+X);
}

/* Every member at or below the marked entries (or below entry when no mark). */
static size_t *selected_members(Browser *b,size_t *out_n,int marked_only)
{
    size_t n=0,*which=malloc((b->arc.count+1)*sizeof(*which)); if(!which) return NULL;
    for(size_t e=1;e<b->view.count;++e) {
        if(marked_only && !b->view.entries[e].marked) continue;
        const char *name=b->view.entries[e].name;
        if(b->member[e]!=NO_MEMBER && b->arc.members[b->member[e]].unsafe) { which[n++]=b->member[e]; continue; }
        size_t pre=strlen(b->dir),len=strlen(name);
        for(size_t i=0;i<b->arc.count;++i) {
            const Member *m=&b->arc.members[i]; const char *p=m->path;
            if(m->unsafe) continue;
            if(pre) { if(strncmp(p,b->dir,pre) || p[pre]!='/') continue; p+=pre+1; }
            if(!strncmp(p,name,len) && (!p[len] || (p[len]=='/' && b->view.entries[e].is_dir))) which[n++]=i;
        }
    }
    *out_n=n; return which;
}

static const Member *link_target(const Archive *a,const Member *m)
{
    for(int depth=0;m && m->is_link && depth<8;++depth) {
        if(!m->link || m->link[0]=='/') return NULL;
        const char *slash=strrchr(m->path,'/');
        size_t n=(slash ? (size_t)(slash-m->path) : 0)+strlen(m->link)+2; char *joined=malloc(n); int unsafe=0;
        if(!joined) return NULL;
        snprintf(joined,n,"%.*s%s%s",slash ? (int)(slash-m->path) : 0,m->path,slash ? "/" : "",m->link);
        /* Resolve ".." textually inside the archive. */
        char *out=malloc(strlen(joined)+1); size_t o=0; char *save=NULL;
        if(!out) { free(joined); return NULL; }
        for(char *part=strtok_r(joined,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
            if(!strcmp(part,".")) continue;
            if(!strcmp(part,"..")) { if(!o) { unsafe=1; break; } while(o && out[o-1]!='/') --o; if(o) --o; continue; }
            if(o) out[o++]='/';
            size_t len=strlen(part); memcpy(out+o,part,len); o+=len;
        }
        out[o]=0; free(joined);
        const Member *next=NULL;
        if(!unsafe) for(size_t i=0;i<a->count;++i) if(!a->members[i].unsafe && !strcmp(a->members[i].path,out)) { next=&a->members[i]; break; }
        free(out); m=next;
    }
    return m && !m->is_link && !m->is_dir ? m : NULL;
}

static char *found_file;
static int find_regular(const char *path,const struct stat *st,int flag,struct FTW *ftw)
{
    (void)ftw;
    if(flag==FTW_F && S_ISREG(st->st_mode)) { free(found_file); found_file=strdup(path); return 1; }
    return 0;
}

/* Extract one member into a new cache directory and open it with the launcher. */
static void browser_open(Browser *b,App *a,size_t index)
{
    const Member *m=link_target(&b->arc,&b->arc.members[index]);
    if(!m) { app_notice(a,T("リンク先がアーカイブの中にありません")); return; }
    size_t which=(size_t)(m-b->arc.members);
    char msg[256];
    char *root=cache_root(); if(!root) { app_notice(a,T("作業用のディレクトリを作れません")); return; }
    char *dir=join2(root,"arc-XXXXXX"); free(root);
    char **temps=dir ? realloc(b->temps,(b->temp_count+1)*sizeof(*temps)) : NULL;
    if(temps) b->temps=temps;
    if(!dir || !temps || !mkdtemp(dir)) { free(dir); app_notice(a,T("作業用のディレクトリを作れません")); return; }
    b->temps[b->temp_count++]=dir;
    if(extract_members(&b->arc,&which,1,dir,msg,sizeof(msg))<0) { app_notice(a,msg); return; }
    free(found_file); found_file=NULL;
    nftw(dir,find_regular,16,FTW_PHYS);
    char *file=found_file; found_file=NULL;
    if(!file) { app_notice(a,T("取り出せませんでした")); return; }
    launcher_open(a,file);
    free(file);
}

static void browser_tick(Screen *s,App *a,double now)
{
    (void)now; Browser *b=s->self;
    if(b->state==BR_LOAD) {
        char err[256];
        if(archive_read(&b->arc,b->source,err,sizeof(err))<0) { app_pop(a); app_notice(a,err); return; }
        if(view_build(b,NULL)<0) { app_pop(a); app_notice(a,T("メモリが足りません")); return; }
        b->state=BR_READY; a->changed=1;
        if(a->widgets.kind==WIDGET_NOTICE) a->widgets.kind=WIDGET_NONE;
        size_t unsafe=0; for(size_t i=0;i<b->arc.count;++i) unsafe+=b->arc.members[i].unsafe;
        if(unsafe) { char msg[128]; snprintf(msg,sizeof(msg),T("外に出る名前が %zu 個あります（取り出しません）"),unsafe); app_notice(a,msg); }
    } else if(b->state==BR_OPEN) {
        b->state=BR_READY; if(a->widgets.kind==WIDGET_NOTICE) a->widgets.kind=WIDGET_NONE;
        browser_open(b,a,b->opening); a->changed=1;
    }
}

static int browser_up(Browser *b,App *a)
{
    if(!*b->dir) return 1;
    char *slash=strrchr(b->dir,'/'); char *came=strdup(slash ? slash+1 : b->dir);
    if(slash) *slash=0; else b->dir[0]=0;
    if(view_build(b,came)<0) app_notice(a,T("メモリが足りません"));
    free(came); return 0;
}

static void browser_unpack(Browser *b,App *a,int tree)
{
    int marked=0; for(size_t i=1;i<b->view.count;++i) marked|=b->view.entries[i].marked;
    size_t n=0,*which=NULL;
    if(marked && !(which=selected_members(b,&n,1))) { app_notice(a,T("メモリが足りません")); return; }
    FileSelection sel; const char *dir=filer_directory(a,&sel);
    char *start=dir ? strdup(dir) : NULL;
    if(!start) { free(which); app_notice(a,T("行先が分かりません")); return; }
    ArcOp *o=op_create(a,ARC_UNPACK,start,NULL,0); free(start);
    if(!o) { free(which); return; }
    o->archive=&b->arc; o->which=which; o->which_count=n;
    if(marked && !n) { app_pop(a); app_notice(a,T("対象がありません")); return; }
    op_ask_destination(a,o,tree);
}

static int browser_key(Screen *s,App *a,const Key *k)
{
    Browser *b=s->self;
    if(b->state==BR_LOAD) { browser_tick(s,a,a->now); if(app_top(a)!=s) return 0; }
    if(b->state==BR_OPEN) { browser_tick(s,a,a->now); return 0; }
    const char *id=commands_lookup(&a->commands,k);
    if(!id) return 0;
    size_t count=b->view.count,rows=FDSCREEN_ROWS,per=rows*(size_t)fdscreen_slots(b->columns);
    DirEntry *e=b->cursor<count ? &b->view.entries[b->cursor] : NULL;
#define IS(x) (!strcmp(id,#x))
    if(IS(CUR_UP)) { if(b->cursor) --b->cursor; }
    else if(IS(CUR_DOWN)) { if(b->cursor+1<count) ++b->cursor; }
    else if(IS(CUR_LEFT) || IS(ROLL_DOWN)) { size_t n=IS(CUR_LEFT) ? rows : per; b->cursor=b->cursor>=n ? b->cursor-n : 0; }
    else if(IS(CUR_RIGHT) || IS(ROLL_UP)) { size_t n=IS(CUR_RIGHT) ? rows : per; b->cursor=b->cursor+n<count ? b->cursor+n : count-1; }
    else if(IS(CUR_TOP)) b->cursor=0;
    else if(IS(CUR_BOTTOM)) b->cursor=count-1;
    else if(IS(ONE_COLUMNS)) b->columns=1;
    else if(IS(TWO_COLUMNS)) b->columns=2;
    else if(IS(THREE_COLUMNS)) b->columns=3;
    else if(IS(FIVE_COLUMNS)) b->columns=5;
    else if(IS(MARK_FILE) || IS(MARK_FILE2) || IS(MARK_FILE3)) {
        if(e && b->cursor) e->marked=!e->marked;
        if(!IS(MARK_FILE) && b->cursor+1<count) ++b->cursor;
    }
    else if(IS(MARK_ALL) || IS(MARK_REVERSE)) { for(size_t i=1;i<count;++i) b->view.entries[i].marked=IS(MARK_ALL) ? 1 : !b->view.entries[i].marked; }
    else if(IS(OUT_DIR)) return browser_up(b,a);
    else if(IS(IN_DIR) || IS(VIEW_FILE)) {
        if(!e) return 0;
        if(!b->cursor) return browser_up(b,a);
        if(b->member[b->cursor]!=NO_MEMBER && b->arc.members[b->member[b->cursor]].unsafe) {
            char msg[256]; snprintf(msg,sizeof(msg),T("危険な名前があるので取り出しません: %.60s"),e->name); app_notice(a,msg);
        } else if(e->is_dir) {
            size_t n=strlen(b->dir)+strlen(e->name)+2; char *next=malloc(n);
            if(!next) { app_notice(a,T("メモリが足りません")); return 0; }
            snprintf(next,n,"%s%s%s",b->dir,*b->dir ? "/" : "",e->name);
            char *old=b->dir; b->dir=next;
            if(view_build(b,NULL)<0) { free(b->dir); b->dir=old; view_build(b,NULL); app_notice(a,T("メモリが足りません")); }
            else free(old);
        } else {
            char msg[256]; snprintf(msg,sizeof(msg),T("取り出し中 %.100s"),e->name);
            b->opening=b->member[b->cursor]; b->state=BR_OPEN; app_notice(a,msg);
        }
    }
    else if(IS(UNPACK_FILE)) browser_unpack(b,a,0);
    else if(IS(UNPACK_TREE)) browser_unpack(b,a,1);
    else if(IS(QUIT_SYSTEM)) return 1;
    else if(IS(HELP_MESSAGE) || IS(REREAD_DIR) || IS(NO_OPERATION)) {}
    else app_notice(a,T("アーカイブの中では使えません"));
#undef IS
    a->changed=1; return 0;
}

static int browse(App *a,const char *path)
{
    ArchiveFormat f=archive_format(path);
    if(f==FMT_NONE) { app_notice(a,T("アーカイブではありません")); return -1; }
    if(f==FMT_7Z) { app_notice(a,T("この環境では使えません")); return -1; }
    if(f==FMT_LHA) { char *l=find_command("lhasa"); if(!l) { app_notice(a,T("lhasa がありません")); return -1; } free(l); }
    Browser *b=calloc(1,sizeof(*b)); if(!b) { app_notice(a,T("メモリが足りません")); return -1; }
    b->screen=(Screen){.self=b,.key=browser_key,.draw=browser_draw,.tick=browser_tick,.destroy=browser_destroy};
    b->below=app_top(a); b->columns=2; b->state=BR_LOAD;
    b->source=strdup(path); b->dir=strdup("");
    if(!b->source || !b->dir || app_push(a,&b->screen)<0) { browser_destroy(&b->screen); app_notice(a,T("画面を開けません")); return -1; }
    const char *base=strrchr(path,'/'); char msg[256]; snprintf(msg,sizeof(msg),T("読み込み中 %.100s"),base ? base+1 : path);
    app_notice(a,msg); return 0;
}
