// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Part of archive.o (included from archive.c): child processes, command
 * lookup through ~/.fdtp/bin, and the extraction cache. */

#define RUN_OUTPUT_CAP (64u*1024u*1024u)
#define RUN_ERROR_CAP (16u*1024u)

/* The children's locale: "C" keeps listings in one parseable form; zip is
 * run in C.UTF-8 so that Info-ZIP built with Unicode support marks names. */
static const char *run_locale="C";

typedef struct { char *out; size_t out_len; char *err; size_t err_len; int status; } RunResult;

static void run_free(RunResult *r) { free(r->out); free(r->err); memset(r,0,sizeof(*r)); }

static char *join2(const char *a,const char *b)
{
    size_t n=strlen(a)+strlen(b)+2; char *p=malloc(n);
    if(p) snprintf(p,n,"%s%s%s",a,(*a && a[strlen(a)-1]=='/')?"":"/",b);
    return p;
}

/* "$HOME/.fdtp/bin:$PATH", owned by the caller. */
static char *search_path(void)
{
    const char *home=getenv("HOME"),*path=getenv("PATH");
    if(!path || !*path) path="/usr/local/bin:/usr/bin:/bin";
    size_t n=(home?strlen(home):0)+strlen(path)+16; char *p=malloc(n);
    if(!p) return NULL;
    if(home && *home) snprintf(p,n,"%s/.fdtp/bin:%s",home,path); else snprintf(p,n,"%s",path);
    return p;
}

/* Absolute path of an executable found on search_path(), or NULL. */
static char *find_command(const char *name)
{
    char *path=search_path(); if(!path) return NULL;
    char *found=NULL,*save=NULL;
    for(char *dir=strtok_r(path,":",&save);dir && !found;dir=strtok_r(NULL,":",&save)) {
        if(!*dir) continue;
        char *candidate=join2(dir,name); struct stat st;
        if(candidate && !stat(candidate,&st) && S_ISREG(st.st_mode) && !access(candidate,X_OK)) found=candidate;
        else free(candidate);
    }
    free(path); return found;
}

static int append_bytes(char **buf,size_t *len,size_t *cap,const char *data,size_t n,size_t limit)
{
    if(*len+n>limit) n=limit>*len ? limit-*len : 0;
    if(*len+n+1>*cap) {
        size_t next=*cap ? *cap : 4096;
        while(next<*len+n+1) next*=2;
        char *p=realloc(*buf,next); if(!p) return -1;
        *buf=p; *cap=next;
    }
    memcpy(*buf+*len,data,n); *len+=n; (*buf)[*len]=0; return 0;
}

/* Run argv[0] (looked up on search_path()) in cwd with stdin from /dev/null.
 * Output is read here, not through the screen hand-over: these commands
 * never interact. LC_ALL is run_locale.
 * Returns 0 when the command ran (see r->status), -1 when it could not start. */
static int run_command(const char *const *argv,const char *cwd,RunResult *r)
{
    memset(r,0,sizeof(*r)); r->status=-1;
    char *program=strchr(argv[0],'/') ? strdup(argv[0]) : find_command(argv[0]);
    if(!program) { errno=ENOENT; return -1; }
    char *path=search_path();
    int out[2]={-1,-1},err[2]={-1,-1};
    if(!path || pipe(out)<0 || pipe(err)<0) goto fail;
    pid_t child=fork();
    if(child<0) goto fail;
    if(!child) {
        int null=open("/dev/null",O_RDONLY);
        if(null>=0) { dup2(null,0); close(null); }
        dup2(out[1],1); dup2(err[1],2);
        close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        if(cwd && chdir(cwd)<0) _exit(126);
        setenv("PATH",path,1); setenv("LC_ALL",run_locale,1);
#ifdef __APPLE__
        setenv("COPYFILE_DISABLE","1",1); /* bsdtar: no ._ AppleDouble members */
#endif
        execv(program,(char *const *)argv);
        _exit(127);
    }
    close(out[1]); close(err[1]); out[1]=err[1]=-1;
    size_t out_cap=0,err_cap=0; int open_fds=2,failed=0;
    struct pollfd fds[2]={{out[0],POLLIN,0},{err[0],POLLIN,0}};
    while(open_fds) {
        if(poll(fds,2,-1)<0) { if(errno==EINTR) continue; failed=1; break; }
        for(int i=0;i<2;++i) {
            if(fds[i].fd<0 || !(fds[i].revents&(POLLIN|POLLHUP|POLLERR))) continue;
            char buf[65536]; ssize_t n=read(fds[i].fd,buf,sizeof(buf));
            if(n<0 && errno==EINTR) continue;
            if(n<=0) { close(fds[i].fd); fds[i].fd=-1; --open_fds; continue; }
            if(i==0) { if(append_bytes(&r->out,&r->out_len,&out_cap,buf,(size_t)n,RUN_OUTPUT_CAP)<0) failed=1; }
            else if(append_bytes(&r->err,&r->err_len,&err_cap,buf,(size_t)n,RUN_ERROR_CAP)<0) failed=1;
        }
    }
    for(int i=0;i<2;++i) if(fds[i].fd>=0) close(fds[i].fd);
    int status; pid_t waited;
    do { waited=waitpid(child,&status,0); } while(waited<0 && errno==EINTR);
    r->status=waited<0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
    if(!r->out) r->out=calloc(1,1);
    if(!r->err) r->err=calloc(1,1);
    free(program); free(path);
    if(failed || !r->out || !r->err) { run_free(r); errno=ENOMEM; return -1; }
    return 0;
fail:
    { int saved=errno;
      for(int i=0;i<2;++i) { if(out[i]>=0) close(out[i]); if(err[i]>=0) close(err[i]); }
      free(program); free(path); errno=saved; }
    return -1;
}

/* lhasa, or an lha that names itself Lhasa: Homebrew installs lhasa as lha.
 * Another lha (lha for UNIX, jlha) takes other options and is not used.
 * Absolute path, or NULL. */
static char *find_lhasa(void)
{
    char *p=find_command("lhasa"); if(p) return p;
    if(!(p=find_command("lha"))) return NULL;
    const char *argv[]={p,NULL}; RunResult r; int lhasa=0;
    if(!run_command(argv,NULL,&r)) lhasa=(r.out && strstr(r.out,"Lhasa")) || (r.err && strstr(r.err,"Lhasa"));
    run_free(&r);
    if(!lhasa) { free(p); return NULL; }
    return p;
}

/* `a | b > out_fd` in cwd (tar into a compressor, with its level). Both
 * standard errors are collected in r->err. r->status is a's status when it
 * failed, otherwise b's. Returns 0 when both ran, -1 when one could not start. */
static int run_pipeline(const char *const *a,const char *const *b,const char *cwd,int out_fd,RunResult *r)
{
    memset(r,0,sizeof(*r)); r->status=-1;
    char *pa=find_command(a[0]),*pb=find_command(b[0]),*path=search_path();
    int link[2]={-1,-1},err[2]={-1,-1}; pid_t child[2]={-1,-1};
    if(!pa || !pb) { errno=ENOENT; goto fail; }
    if(!path || pipe(link)<0 || pipe(err)<0) goto fail;
    for(int i=0;i<2;++i) {
        if((child[i]=fork())<0) goto fail;
        if(child[i]) continue;
        int in=i ? link[0] : open("/dev/null",O_RDONLY),out=i ? out_fd : link[1];
        if(in>=0) dup2(in,0);
        dup2(out,1); dup2(err[1],2);
        close(link[0]); close(link[1]); close(err[0]); close(err[1]);
        if(cwd && chdir(cwd)<0) _exit(126);
        setenv("PATH",path,1); setenv("LC_ALL",run_locale,1);
#ifdef __APPLE__
        setenv("COPYFILE_DISABLE","1",1);
#endif
        execv(i ? pb : pa,(char *const *)(i ? b : a));
        _exit(127);
    }
    close(link[0]); close(link[1]); close(err[1]); link[0]=link[1]=err[1]=-1;
    size_t cap=0; int failed=0;
    for(;;) {
        char buf[4096]; ssize_t n=read(err[0],buf,sizeof(buf));
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        if(append_bytes(&r->err,&r->err_len,&cap,buf,(size_t)n,RUN_ERROR_CAP)<0) failed=1;
    }
    close(err[0]); err[0]=-1;
    int status[2];
    for(int i=0;i<2;++i) {
        int s; pid_t w;
        do { w=waitpid(child[i],&s,0); } while(w<0 && errno==EINTR);
        status[i]=w<0 ? -1 : WIFEXITED(s) ? WEXITSTATUS(s) : 128+WTERMSIG(s);
    }
    r->status=status[0] ? status[0] : status[1];
    r->out=calloc(1,1); if(!r->err) r->err=calloc(1,1);
    free(pa); free(pb); free(path);
    if(failed || !r->out || !r->err) { run_free(r); errno=ENOMEM; return -1; }
    return 0;
fail:
    { int saved=errno;
      for(int i=0;i<2;++i) {
          if(link[i]>=0) close(link[i]);
          if(err[i]>=0) close(err[i]);
          if(child[i]>0) { int s; while(waitpid(child[i],&s,0)<0 && errno==EINTR) {} }
      }
      free(pa); free(pb); free(path); errno=saved; }
    return -1;
}

/* First line of a command's error output, for row 23. */
static void first_line(const RunResult *r,char *out,size_t n)
{
    const char *s=r->err && *r->err ? r->err : r->out ? r->out : "";
    while(*s=='\n') ++s;
    size_t len=strcspn(s,"\n"); if(len>=n) len=n-1;
    memcpy(out,s,len); out[len]=0;
}

static int make_dirs(const char *path,mode_t mode)
{
    char *p=strdup(path); if(!p) return -1;
    for(char *s=p+1;;++s) if(*s=='/' || !*s) {
        char c=*s; *s=0; struct stat st;
        if(mkdir(p,mode)<0 && (errno!=EEXIST || stat(p,&st)<0 || !S_ISDIR(st.st_mode))) { free(p); return -1; }
        *s=c; if(!c) break;
    }
    free(p); return 0;
}

/* $FDTP_CACHE_DIR (tests) or ~/.cache/fdtp, created when missing. */
static char *cache_root(void)
{
    const char *env=getenv("FDTP_CACHE_DIR"),*home=getenv("HOME");
    char *root=env && *env ? strdup(env) : home && *home ? join2(home,".cache/fdtp") : NULL;
    if(!root) { errno=ENOENT; return NULL; }
    if(make_dirs(root,0700)<0) { free(root); return NULL; }
    return root;
}

static int remove_entry(const char *path,const struct stat *st,int flag,struct FTW *ftw)
{ (void)st; (void)flag; (void)ftw; remove(path); return 0; }
static void remove_tree(const char *path) { if(path && *path) nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
