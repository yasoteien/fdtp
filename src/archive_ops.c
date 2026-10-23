// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Part of archive.o (included from archive.c): extracting archives, and
 * the u/U operation screen (prompt, tree). p is archive_pack.c. */

/* Extract which[0..n) (all members when n==0) into dest, an existing
 * directory. Refuses the whole request when any of them would be written
 * outside dest. Returns 0, or -1 with a row-23 message in msg. */
static int extract_members(const Archive *a,const size_t *which,size_t n,const char *dest,char *msg,size_t msgn)
{
    size_t total=n ? n : a->count;
    for(size_t i=0;i<total;++i) {
        const char *bad=member_danger(a,&a->members[n ? which[i] : i]);
        if(bad) { snprintf(msg,msgn,T("危険な名前があるので取り出しません: %.60s"),bad); return -1; }
    }
    if(a->format==FMT_LHA) { char *l=find_command("lhasa"); if(!l) { snprintf(msg,msgn,"%s",T("lhasa がありません")); return -1; } free(l); }
    const char **argv=calloc(n+10,sizeof(*argv)); char **owned=calloc(n+1,sizeof(*owned));
    if(!argv || !owned) { free(argv); free(owned); snprintf(msg,msgn,"%s",T("メモリが足りません")); return -1; }
    size_t c=0; const char *cwd=NULL;
    if(a->format==FMT_TAR) { argv[c++]="tar"; argv[c++]="-xf"; argv[c++]=a->path; argv[c++]="-C"; argv[c++]=dest; if(n) argv[c++]="--"; }
    else if(a->format==FMT_ZIP) { argv[c++]="unzip"; argv[c++]="-o"; argv[c++]="-q"; argv[c++]="-d"; argv[c++]=dest; argv[c++]=a->path; }
    else { argv[c++]="lhasa"; argv[c++]="xf"; argv[c++]=a->path; cwd=dest; }
    int failed=0;
    for(size_t i=0;i<n;++i) {
        const Member *m=&a->members[which[i]];
        if(a->format==FMT_LHA) {
            /* lhasa matches '/'-separated paths; DOS level-0 names use '\'. */
            int ascii=1; for(const char *p=m->raw;*p;++p) if((unsigned char)*p>=0x80) ascii=0;
            if(!(owned[i]=strdup(ascii ? m->path : m->raw))) failed=1;
            argv[c++]=owned[i] ? owned[i] : m->raw;
        } else argv[c++]=m->raw;
    }
    RunResult r; int result=-1;
    if(failed) snprintf(msg,msgn,"%s",T("メモリが足りません"));
    else {
        run_locale=a->format==FMT_ZIP ? "C.UTF-8" : "C";
        int started=run_command(argv,cwd,&r);
        run_locale="C";
        if(started<0) { snprintf(msg,msgn,T("%s を起動できません"),argv[0]); goto out; }
        /* unzip: 1 is a warning with everything extracted. */
        if(r.status==0 || (a->format==FMT_ZIP && r.status==1)) result=0;
        else { char line[160]; first_line(&r,line,sizeof(line)); snprintf(msg,msgn,T("取り出せません: %.120s"),line); }
        run_free(&r);
    }
out:
    for(size_t i=0;i<n;++i) free(owned[i]);
    free(owned); free(argv); return result;
}

static void reload_filers(App *a) { for(int i=0;i<a->depth;++i) filer_reload(a->screens[i],a); }

/* The nearest filer below the top: its directory is "the current one". */
static const char *filer_directory(App *a,FileSelection *sel)
{
    for(int i=a->depth-1;i>=0;--i) if(!filer_selection(a->screens[i],sel)) return sel->directory;
    return NULL;
}

typedef enum { ARC_UNPACK } ArcKind;
typedef enum { PH_INPUT, PH_TREE, PH_RUN } ArcPhase;
typedef struct {
    Screen screen, *below;
    ArcKind kind; ArcPhase phase;
    char *directory, **names; size_t count; /* filer: the archives */
    const Archive *archive; size_t *which, which_count; /* browser u: borrowed archive */
    char *target;
    Tree tree;
} ArcOp;

static void op_destroy(Screen *s)
{
    ArcOp *o=s->self;
    for(size_t i=0;i<o->count;++i) free(o->names[i]);
    free(o->names); free(o->directory); free(o->which); free(o->target); tree_free(&o->tree); free(o);
}
static void op_draw(Screen *s,App *a,Canvas *c,Cells *cells)
{
    ArcOp *o=s->self;
    if(o->phase==PH_TREE) tree_draw(&o->tree,cells,T("<< 解凍先のツリー >>"));
    else if(o->below && o->below->draw) o->below->draw(o->below,a,c,cells);
}
static char *resolve(const char *dir,const char *name)
{ return name[0]=='/' ? strdup(name) : join2(dir,name); }

static void op_run(App *a,ArcOp *o)
{
    char msg[256]="";
    int ok=1;
    if(o->archive) {
        ok=!extract_members(o->archive,o->which,o->which_count,o->target,msg,sizeof(msg));
        if(ok) snprintf(msg,sizeof(msg),T("解凍しました %zu 件 → %.100s"),o->which_count ? o->which_count : o->archive->count,o->target);
    } else {
        size_t done=0;
        for(size_t i=0;i<o->count && ok;++i) {
            char *path=resolve(o->directory,o->names[i]); Archive arc;
            if(!path) { snprintf(msg,sizeof(msg),"%s",T("メモリが足りません")); ok=0; break; }
            if(archive_read(&arc,path,msg,sizeof(msg))<0) ok=0;
            else { ok=!extract_members(&arc,NULL,0,o->target,msg,sizeof(msg)); archive_free(&arc); if(ok) ++done; }
            free(path);
        }
        if(ok) snprintf(msg,sizeof(msg),T("解凍しました %zu 個 → %.100s"),done,o->target);
    }
    reload_filers(a);
    app_pop(a);
    app_notice(a,msg);
}
static void op_start(App *a,ArcOp *o)
{
    char msg[256];
    snprintf(msg,sizeof(msg),T("解凍中 → %.100s"),o->target);
    o->phase=PH_RUN; app_notice(a,msg);
}
/* The destination chosen by input or tree: must be an existing directory. */
static int op_destination(App *a,ArcOp *o,const char *text)
{
    char *p=resolve(o->directory,text); struct stat st;
    if(!p || !*text || stat(p,&st)<0 || !S_ISDIR(st.st_mode)) { free(p); app_notice(a,T("行先のディレクトリがありません")); return -1; }
    free(o->target); o->target=p; op_start(a,o); return 0;
}
static int op_key(Screen *s,App *a,const Key *k)
{
    ArcOp *o=s->self;
    if(o->phase==PH_RUN) { op_run(a,o); return 0; }
    if(o->phase==PH_TREE) {
        int r=tree_key(&o->tree,a,k);
        if(r<0) { app_notice(a,T("操作を中止しました")); return 1; }
        if(r>0) { char *chosen=strdup(o->tree.cursor->path); int fail=!chosen || op_destination(a,o,chosen)<0; free(chosen); if(fail) return 1; }
        return 0;
    }
    WidgetResult r=widgets_key(&a->widgets,k);
    if(r==WIDGET_CANCEL) { app_notice(a,T("操作を中止しました")); return 1; }
    if(r!=WIDGET_ACCEPT) return 0;
    return op_destination(a,o,a->widgets.text)<0 ? 1 : 0;
}
static void op_tick(Screen *s,App *a,double now)
{ (void)now; ArcOp *o=s->self; if(o->phase==PH_RUN) op_run(a,o); }

/* names: copied. archive/which: borrowed archive (browser), owned which. */
static ArcOp *op_create(App *a,ArcKind kind,const char *directory,const char *const *names,size_t count)
{
    ArcOp *o=calloc(1,sizeof(*o)); if(!o) { app_notice(a,T("メモリが足りません")); return NULL; }
    o->screen=(Screen){.self=o,.key=op_key,.draw=op_draw,.tick=op_tick,.destroy=op_destroy};
    o->below=app_top(a); o->kind=kind;
    o->directory=strdup(directory); o->names=calloc(count ? count : 1,sizeof(*o->names));
    if(!o->directory || !o->names) goto fail;
    for(size_t i=0;i<count;++i) { if(!(o->names[i]=strdup(names[i]))) goto fail; o->count=i+1; }
    if(app_push(a,&o->screen)<0) goto fail;
    return o;
fail: op_destroy(&o->screen); app_notice(a,T("メモリが足りません")); return NULL;
}
/* Ask for the destination: row 23 (tree==0) or the tree. */
static int op_ask_destination(App *a,ArcOp *o,int tree)
{
    if(tree) {
        if(tree_init(&o->tree,o->directory)<0) { app_pop(a); app_notice(a,T("メモリが足りません")); return -1; }
        o->phase=PH_TREE; return 0;
    }
    o->phase=PH_INPUT; widgets_input_history(&a->widgets,T("解凍先"),o->directory,o->directory,WIDGET_HISTORY_DESTINATION); return 0;
}
