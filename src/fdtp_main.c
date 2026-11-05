// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "filer.h"
#include "version.h"
#include "fileops.h"
#include "config.h"
#include "pomera_notice.h"
#include "app.h"
#include "headless.h"
#include "raster.h"
#include "rmp_platform.h"
#include "rmp_screen.h"
#include "rmp_gfx.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include "icon_data.inc"   /* fdtp_icon: tools/gen_icon.py (build/) */

typedef struct { GLuint texture, program; GLint pos, uv; int w, h; } Texture;
static GLuint shader(GLenum kind, const char *source)
{
    GLuint s=glCreateShader(kind);
    glShaderSource(s,1,&source,NULL); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s,sizeof(log),NULL,log);
        fprintf(stderr,"FDTP shader: %s\n",log); glDeleteShader(s); return 0;
    }
    return s;
}
static void texture_destroy(Texture *t)
{
    if (!t->texture && !t->program) return;
    rmp_gfx_flush();
    if (t->texture) glDeleteTextures(1,&t->texture);
    if (t->program) glDeleteProgram(t->program);
    memset(t,0,sizeof(*t));
    rmp_gfx_invalidate_state();
}
static int texture_create(Texture *t)
{
    const char *vs="attribute vec2 p; attribute vec2 uv; varying vec2 v; void main(){gl_Position=vec4(p,0.,1.);v=uv;}";
    const char *fs="precision mediump float; varying vec2 v; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,v);}";
    rmp_gfx_flush();
    GLuint v=shader(GL_VERTEX_SHADER,vs), f=shader(GL_FRAGMENT_SHADER,fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        rmp_gfx_invalidate_state(); return -1;
    }
    t->program=glCreateProgram(); glAttachShader(t->program,v); glAttachShader(t->program,f);
    glLinkProgram(t->program); glDeleteShader(v); glDeleteShader(f);
    GLint ok=0; glGetProgramiv(t->program,GL_LINK_STATUS,&ok);
    if (!ok) { texture_destroy(t); return -1; }
    t->pos=glGetAttribLocation(t->program,"p"); t->uv=glGetAttribLocation(t->program,"uv");
    glUseProgram(t->program); glUniform1i(glGetUniformLocation(t->program,"image"),0);
    glActiveTexture(GL_TEXTURE0); glGenTextures(1,&t->texture); glBindTexture(GL_TEXTURE_2D,t->texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,CANVAS_W,CANVAS_H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    t->w=CANVAS_W; t->h=CANVAS_H;
    GLenum error=glGetError(); rmp_gfx_invalidate_state();
    if (error!=GL_NO_ERROR) { texture_destroy(t); return -1; }
    return 0;
}
static void texture_update(const Texture *t, const Raster *r, const FDRect *rects, int count)
{
    if (!count) return;
    rmp_gfx_flush();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,t->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    for (int i=0;i<count;++i) {
        const FDRect *d=&rects[i];
        /* raster emits whole rows: no GLES2 unpack-row-length extension needed. */
        if(r->w!=t->w || d->y+d->h>t->h) continue;   /* a canvas of another size: never sent */
        glTexSubImage2D(GL_TEXTURE_2D,0,0,d->y,r->w,d->h,GL_RGBA,GL_UNSIGNED_BYTE,CANVAS_ROW(r,d->y));
        rmp_rect damage={0,d->y,d->w,d->h}; rmp_frame_damage(&damage);
    }
    rmp_gfx_invalidate_state();
}
static void texture_draw(const Texture *t, const rmp_rect *rects, int count)
{
    const GLfloat vertices[]={-1.0f,1,0,0, -1.0f,-1,0,1, 1.0f,1,1,0, 1.0f,-1,1,1};
    for (int i=0;i<count;++i) {
        rmp_frame_clip(&rects[i]);
        rmp_gfx_flush();
        glUseProgram(t->program); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,t->texture);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glBindBuffer(GL_ARRAY_BUFFER,0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
        glEnableVertexAttribArray((GLuint)t->pos); glEnableVertexAttribArray((GLuint)t->uv);
        glVertexAttribPointer((GLuint)t->pos,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices);
        glVertexAttribPointer((GLuint)t->uv,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices+2);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        glDisableVertexAttribArray((GLuint)t->pos); glDisableVertexAttribArray((GLuint)t->uv);
        rmp_gfx_invalidate_state();
    }
}
static int init_platform(rmp_platform_options *options)
{
    /* The size in use (WIDE_NOTES.md): an X window opens at it, and coming
     * back from an external command keeps it. The Pomera's 1024 x 600 is
     * the platform's own default. The window may take any size: fit_screen
     * follows it. */
    int pomera=CANVAS_W==1024 && CANVAS_H==600;
    options->display_w=pomera ? 0 : CANVAS_W; options->display_h=pomera ? 0 : CANVAS_H;
    options->resizable=1;
    /* The X window's name and icon (revision 1 item 23; KMS ignores them). */
    options->title="FDTP"; options->wm_instance="fdtp"; options->wm_class="FDTP";
    /* Another FDTP window (Ctrl+N) opens apart from the others: drop targets stay in view (item 15). */
    options->place_apart=1;
    options->icon=fdtp_icon; options->icon_words=sizeof fdtp_icon/sizeof fdtp_icon[0];
    if(options->output==RMP_OUTPUT_AUTO) {
        const char *display=getenv("DISPLAY");
        if(display && *display) {
            options->output=RMP_OUTPUT_X11;
            if(rmp_platform_init(options)==0) return 0;
            rmp_platform_shutdown();
        }
        options->output=RMP_OUTPUT_KMS;
    }
    return rmp_platform_init(options);
}
static int executable_on_path(const char *name)
{
    const char *path=getenv("PATH"); if(!path) path="/bin:/usr/bin";
    for(;;) {
        const char *end=strchr(path,':'); size_t n=end ? (size_t)(end-path) : strlen(path);
        char *candidate=malloc(n+strlen(name)+2); if(!candidate) return 0;
        if(n) { memcpy(candidate,path,n); candidate[n]='/'; strcpy(candidate+n+1,name); } else strcpy(candidate,name);
        struct stat st; int yes=stat(candidate,&st)==0 && S_ISREG(st.st_mode) && access(candidate,X_OK)==0; free(candidate);
        if(yes) return 1;
        if(!end) return 0;
        path=end+1;
    }
}
static void press_any_key(void)
{
    fputs(T("\n何かキーを押してください"),stdout); fflush(stdout);
    struct termios saved,raw; int tty=tcgetattr(STDIN_FILENO,&saved)==0;
    if(tty) { raw=saved; raw.c_lflag&=(tcflag_t)~(ICANON|ECHO|ISIG); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0; tcsetattr(STDIN_FILENO,TCSANOW,&raw); }
    unsigned char ch; ssize_t n;
    do { n=read(STDIN_FILENO,&ch,1); } while(n<0 && errno==EINTR);
    if(tty) tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    fputc('\n',stdout);
}
/* Only this platform adapter executes an App request. KMS releases GL and the
 * terminal before fork; X terminals are tracked/reaped without blocking UI. */
static int run_external(App *a,Texture *texture,Cells *cells,Canvas *canvas,
                        rmp_platform_options *options,pid_t *async_child)
{
    ExternalRequest *r=&a->external;
    int is_x=options->output==RMP_OUTPUT_X11;
    /* A request without APP_EXTERNAL_TERMINAL (rmp-jukebox) draws on the
     * display itself: no terminal around it, under X it is started directly. */
    int wrap=(r->flags & APP_EXTERNAL_TERMINAL)!=0;
    const char *term=getenv("TERM"); int fb=wrap && !is_x && term && !strcmp(term,"linux");
    char *args[APP_EXTERNAL_ARGS+24]; int n=0;
    if(is_x && wrap) { args[n++]="x-terminal-emulator"; args[n++]="-e"; }
    else if(fb) { args[n++]="fbterm"; args[n++]="--"; if(executable_on_path("uim-fep")) { args[n++]="uim-fep"; args[n++]="-e"; } }
    if(((is_x && wrap) || fb) && (r->flags & APP_EXTERNAL_PAUSE)) {
        args[n++]="/bin/sh"; args[n++]="-c";
        /* Fixed shell program; all user-supplied argv stay separate arguments. */
        args[n++]=i18n_lang()==I18N_EN ? "\"$@\"; result=$?; printf '\\nPress any key'; state=$(stty -g); stty -icanon -echo -isig min 1 time 0; dd bs=1 count=1 >/dev/null 2>&1; stty \"$state\"; exit \"$result\"" : "\"$@\"; result=$?; printf '\\n何かキーを押してください'; state=$(stty -g); stty -icanon -echo -isig min 1 time 0; dd bs=1 count=1 >/dev/null 2>&1; stty \"$state\"; exit \"$result\""; /* i18n:keep */
        args[n++]="fdtp-external";
    }
    for(int i=0;r->argv[i];++i) args[n++]=r->argv[i];
    args[n]=NULL;
    const int signals[]={SIGINT,SIGQUIT,SIGTERM,SIGHUP,SIGPIPE};
    struct sigaction saved[5],ignore;
    memset(&ignore,0,sizeof(ignore)); sigemptyset(&ignore.sa_mask); ignore.sa_handler=SIG_IGN;
    if(!is_x) {
        texture_destroy(texture); rmp_platform_shutdown();
        for(int i=0;i<5;++i) sigaction(signals[i],&ignore,&saved[i]);
    }
    pid_t child=fork();
    if(child==0) {
        struct sigaction def; memset(&def,0,sizeof(def)); sigemptyset(&def.sa_mask); def.sa_handler=SIG_DFL;
        for(int i=0;i<5;++i) sigaction(signals[i],&def,NULL);
        if(fb) setenv("LANG","ja_JP.UTF-8",1);
        if(r->cwd && chdir(r->cwd)<0) { perror(r->cwd); _exit(126); }
        execvp(args[0],args); perror(args[0]); _exit(127);
    }
    int status=0;
    if(child<0) { perror("fork external"); status=-1; }
    if(is_x) {
        if(child>0) *async_child=child;
        app_external_clear(a);
        if(status) app_notice(a,T("外部コマンドを起動できません"));
    } else {
        if(child>0) {
            pid_t waited; do { waited=waitpid(child,&status,0); } while(waited<0 && errno==EINTR);
            if(waited<0) status=-1;
        }
        if(child>0 && !fb && (r->flags & APP_EXTERNAL_PAUSE)) press_any_key();
        for(int i=0;i<5;++i) sigaction(signals[i],&saved[i],NULL);
        if(init_platform(options)<0 || texture_create(texture)<0) return -1;
        rmp_set_frame_interval(2); rmp_frame_set_clear_color(0x000055);
        cells_invalidate(cells); canvas->full_dirty=1; rmp_frame_invalidate_all(); app_external_done(a,status);
    }
    return 0;
}
/* WIDE_NOTES.md: the size SCREENSIZE / --size asks for on this surface, again
 * whenever the X window was resized or SCREENSIZE changed (the customizer).
 * Nothing happens while it stays the same (the Pomera: always 80 x 25 on
 * 1024 x 600). Outside a frame. 0, or -1 on a GL error. */
static int fit_screen(App *a,int cols,int rows,Texture *texture,Canvas *canvas,Cells *cells)
{
    int ow,oh; rmp_output_size(&ow,&oh);
    ScreenSize want=app_screen_choice(a,cols,rows,ow,oh);
    if(!app_screen_differs(want) && texture->texture && texture->w==CANVAS_W && texture->h==CANVAS_H) return 0;
    screen_apply(want);
    if(rmp_set_display_size(CANVAS_W,CANVAS_H)<0) return -1;
    texture_destroy(texture);
    if(texture_create(texture)<0) return -1;
    canvas_init(canvas); cells_invalidate(cells); a->changed=1; rmp_frame_invalidate_all();
    return 0;
}
/* ---- The X window's mouse, drag and drop and Ctrl+N (revision 1 item 15;
 * as FDTM, docs/FDTP_MACOS.md, through the same filer functions). The Mac's
 * Cmd is Ctrl here. Copy and move follow the X convention, as Thunar: Ctrl
 * copies, Shift moves, otherwise the same file system moves and another
 * copies. KMS has no pointer and no drag and drop: none of this happens. */
#define DOUBLE_CLICK 0.4    /* seconds, GTK's default */
#define DRAG_START 4        /* display pixels before a press becomes a drag */
static App *mouse_app;      /* the one App, for the drag and drop callbacks */
static Screen *filer_screen(void) { return mouse_app && mouse_app->depth ? mouse_app->screens[0] : NULL; }
static void reload_filers(App *a) { for(int i=0;i<a->depth;++i) filer_reload(a->screens[i],a); a->changed=1; }
/* The display pixel (the canvas, rmp_poll_pointer) to the cell under it. */
static int cell_at(int x,int y,int *col,int *row)
{
    int gx=x-CANVAS_GRID_X;
    if(gx<0 || y<0) return 0;
    *col=gx/12; *row=y/24;
    return *col<FD_COLS && *row<FD_ROWS;
}
static struct {
    int pressed, dragging, deselect, col, row, x, y;   /* the press on an entry */
    double last_click; int last_col, last_row;          /* for a double click */
} mouse;
/* A drop waits for the loop (it comes inside the platform's event handling). */
static struct { char *directory, *destination, **names; size_t count; int move; } drop;
static char *parent_of(const char *path)
{
    size_t n=strlen(path);
    while(n>1 && path[n-1]=='/') --n;   /* a directory may come with a slash */
    size_t cut=n; while(cut>0 && path[cut-1]!='/') --cut;
    if(cut==0 || cut==n) return NULL;   /* relative, or the root */
    char *p=malloc(cut+1); if(!p) return NULL;
    memcpy(p,path,cut); p[cut]=0;
    if(cut>1) p[cut-1]=0;               /* no slash at the end, but for "/" */
    return p;
}
static char *name_of(const char *path)
{
    size_t n=strlen(path); while(n>1 && path[n-1]=='/') --n;
    size_t cut=n; while(cut>0 && path[cut-1]!='/') --cut;
    char *p=malloc(n-cut+1); if(!p) return NULL;
    memcpy(p,path+cut,n-cut); p[n-cut]=0; return p;
}
/* What a drop of paths at display (x,y) does (RMP_DND_*), and where it goes
 * (*dest, malloc'd, when asked and not NONE). As FDTM: into the directory
 * under the cell, else the pane's; all from one directory, none into
 * itself or into the directory it is in. want: the action already chosen
 * (the drop), or NONE to choose by the keys and the file systems. paths
 * NULL: the list is not here yet, the keys alone decide. *mixed (may be
 * NULL) is set when the only fault is that the files come from more than
 * one directory (a Thunar search, the desktop and a folder together). */
static int drop_decide(App *a,int x,int y,const char *const *paths,size_t count,unsigned mods,unsigned allowed,int want,char **dest_out,int *mixed)
{
    if(dest_out) *dest_out=NULL;
    if(mixed) *mixed=0;
    int c,r; Screen *s=filer_screen();
    if(drop.destination || !s || app_top(a)!=s || !cell_at(x,y,&c,&r)) return RMP_DND_NONE;
    char *dest=filer_drop_directory(s,a,c,r);
    if(!dest) return RMP_DND_NONE;
    int op=want ? want : (mods & RMP_MOD_CTRL) ? RMP_DND_COPY : (mods & RMP_MOD_SHIFT) ? RMP_DND_MOVE : RMP_DND_COPY;
    if(paths) {
        char target[PATH_MAX],from[PATH_MAX];
        char *parent=count ? parent_of(paths[0]) : NULL;
        int ok=parent && realpath(dest,target) && realpath(parent,from) && strcmp(from,target);
        free(parent);
        for(size_t i=0;ok && i<count;++i) {
            char *p=parent_of(paths[i]),*n=name_of(paths[i]),here[PATH_MAX],self[PATH_MAX*2];
            ok=p && n && *n && realpath(p,here);
            if(ok && strcmp(here,from)) { ok=0; if(mixed) *mixed=1; }   /* one directory at a time */
            if(ok) {   /* not into itself: the target is not it, nor below it */
                snprintf(self,sizeof self,"%s/%s",strcmp(from,"/") ? from : "",n);
                size_t sl=strlen(self);
                ok=strncmp(target,self,sl) || (target[sl]!=0 && target[sl]!='/');
            }
            free(p); free(n);
        }
        if(!ok) { free(dest); return RMP_DND_NONE; }
        struct stat sa,sb;
        if(!want && !(mods & (RMP_MOD_CTRL | RMP_MOD_SHIFT)))
            op=stat(from,&sa)==0 && stat(target,&sb)==0 && sa.st_dev==sb.st_dev ? RMP_DND_MOVE : RMP_DND_COPY;
    }
    if(!(allowed & (unsigned)op)) op=(allowed & RMP_DND_COPY) ? RMP_DND_COPY : (allowed & RMP_DND_MOVE) ? RMP_DND_MOVE : RMP_DND_NONE;
    if(dest_out && op!=RMP_DND_NONE) *dest_out=dest; else free(dest);
    return op;
}
/* Files from several directories are taken as far as the drop, which then
 * says why nothing was done (rather than a silent refusal or a part done). */
static int dnd_over(void *ctx,int x,int y,const char *const *paths,size_t count,unsigned mods,unsigned allowed)
{
    int mixed, op=drop_decide(ctx,x,y,paths,count,mods,allowed,RMP_DND_NONE,NULL,&mixed);
    if(mixed) op=(allowed & RMP_DND_COPY) ? RMP_DND_COPY : RMP_DND_MOVE;
    return op;
}
static int dnd_drop(void *ctx,int x,int y,const char *const *paths,size_t count,int action)
{
    char *dest; int mixed;
    int op=drop_decide(ctx,x,y,paths,count,0,(unsigned)action,action,&dest,&mixed);
    if(mixed) app_notice(ctx,T("別々のディレクトリの物は一度に落とせません"));
    if(op==RMP_DND_NONE) return op;
    drop.names=calloc(count,sizeof *drop.names); drop.directory=parent_of(paths[0]);
    int ok=drop.names && drop.directory;
    for(size_t i=0;ok && i<count;++i) ok=(drop.names[i]=name_of(paths[i]))!=NULL;
    drop.count=count;
    if(!ok) {
        for(size_t i=0;drop.names && i<count;++i) free(drop.names[i]);
        free(drop.names); free(drop.directory); free(dest); memset(&drop,0,sizeof drop);
        return RMP_DND_NONE;
    }
    drop.destination=dest; drop.move=op==RMP_DND_MOVE;
    return op;
}
/* Drags the press's entry (or every marked one with it) out. */
static void start_drag(App *a)
{
    FileSelection sel;
    if(filer_drag_selection(filer_screen(),a,mouse.col,mouse.row,&sel)<0) { mouse.pressed=0; return; }
    char **paths=calloc(sel.count,sizeof *paths); size_t n=0;
    for(;paths && n<sel.count;++n) {
        size_t dl=strlen(sel.directory),nl=strlen(sel.names[n]);
        if(!(paths[n]=malloc(dl+nl+2))) break;
        memcpy(paths[n],sel.directory,dl); paths[n][dl]='/'; memcpy(paths[n]+dl+(dl && sel.directory[dl-1]=='/' ? 0 : 1),sel.names[n],nl+1);
    }
    if(paths && n==sel.count && rmp_dnd_drag((const char *const *)paths,n)==0) mouse.dragging=1;
    else mouse.pressed=0;   /* the button came up already: its release is still to come */
    for(size_t i=0;paths && i<n;++i) free(paths[i]);
    free(paths);
}
/* Clicks as FDTM (the Finder's): a click selects the entry alone, Ctrl+click
 * marks or unmarks it, Shift+click marks the range, a double click opens
 * (Return), the wheel moves the cursor. A plain click on a marked entry keeps
 * the marks until the button comes up without a drag, so that they can be
 * dragged together. */
static void pointer_events(App *a)
{
    rmp_pointer_event ev[32]; int n=rmp_poll_pointer(ev,32);
    for(int i=0;i<n;++i) {
        const rmp_pointer_event *e=&ev[i]; Screen *s=filer_screen(); int col=0,row=0;
        int in=cell_at(e->x,e->y,&col,&row);
        if(e->type==RMP_POINTER_PRESS && e->button==1) {
            mouse.pressed=mouse.dragging=mouse.deselect=0;
            if(!in || !s || app_top(a)!=s) { mouse.last_click=0; continue; }
            int marked=filer_mouse_marked(s,a,col,row);
            int action=(e->mods & RMP_MOD_CTRL) ? FILER_TOGGLE : (e->mods & RMP_MOD_SHIFT) ? FILER_RANGE : marked ? FILER_POINT : FILER_SELECT;
            if(!filer_mouse(s,a,col,row,action)) { mouse.last_click=0; continue; }
            mouse.pressed=1; mouse.deselect=action==FILER_POINT; mouse.col=col; mouse.row=row; mouse.x=e->x; mouse.y=e->y;
            int plain=action!=FILER_TOGGLE && action!=FILER_RANGE;
            if(plain && mouse.last_click>0 && e->time-mouse.last_click<DOUBLE_CLICK && col==mouse.last_col && row==mouse.last_row) {
                mouse.pressed=0; mouse.last_click=0;
                Key k={KRET,0,0}; app_key(a,&k);
            } else { mouse.last_click=plain ? e->time : 0; mouse.last_col=col; mouse.last_row=row; }
        } else if(e->type==RMP_POINTER_RELEASE && e->button==1) {
            if(mouse.pressed && mouse.deselect && !mouse.dragging) filer_mouse(s,a,mouse.col,mouse.row,FILER_SELECT);
            if(!mouse.dragging) mouse.pressed=0;
        } else if(e->type==RMP_POINTER_MOTION) {
            if(mouse.pressed && !mouse.dragging && (abs(e->x-mouse.x)>=DRAG_START || abs(e->y-mouse.y)>=DRAG_START)) {
                mouse.last_click=0; start_drag(a);
            }
        } else if(e->type==RMP_POINTER_WHEEL) {
            if(!s || app_top(a)!=s || (a->widgets.kind!=WIDGET_NONE && a->widgets.kind!=WIDGET_NOTICE)) continue;
            Key k={e->wheel<0 ? KUP : KDOWN,0,0}; app_key(a,&k);
        }
        if(a->external.pending || a->quit) break;
    }
    int result=rmp_dnd_take_result();
    if(result>=0) {
        mouse.pressed=mouse.dragging=0;
        /* Taken: the marks are done with. The other side may still be moving
         * them one by one, which the filer's look at the directory catches. */
        if(result!=RMP_DND_NONE) { filer_unmark(filer_screen(),a); reload_filers(a); }
    }
}
/* Ctrl+N: another FDTP at this pane's directory. Another process, as FDTM's
 * Cmd+N: the screen size is one per process. The platform places its
 * window apart from the others (place_apart). */
static void new_window(App *a,const char *directory,int cols,int rows)
{
    char exe[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);
    pid_t child=n>0 ? fork() : -1;
    if(child==0) {
        setsid();
        if(fork()==0) {
            exe[n]=0;
            /* The tests' and the trace's variables are this window's. */
            unsetenv("FDTP_TEST_KEYS"); unsetenv("FDTP_SNAPSHOT_PREFIX"); unsetenv("FDTP_TRACE");
            char size[32],dir[PATH_MAX+2]; char *args[8]; int k=0;
            args[k++]="fdtp"; args[k++]="--x11";
            if(cols>0 && rows>0) { snprintf(size,sizeof size,"%dx%d",cols,rows); args[k++]="--size"; args[k++]=size; }
            snprintf(dir,sizeof dir,"%s%s",directory[0]=='-' ? "./" : "",directory);
            args[k++]=dir; args[k]=NULL;
            execv(exe,args); _exit(127);
        }
        _exit(0);
    }
    if(child>0) waitpid(child,NULL,0);
    else app_notice(a,T("外部コマンドを起動できません"));
}
static void take_drop(App *a)
{
    if(!drop.destination) return;
    if(app_top(a)==filer_screen() && (a->widgets.kind==WIDGET_NONE || a->widgets.kind==WIDGET_NOTICE)) {
        if(a->widgets.kind==WIDGET_NOTICE) a->widgets.kind=WIDGET_NONE;
        FileSelection sel={drop.directory,drop.names[0],(const char *const *)drop.names,drop.count};
        fileops_transfer_to(a,&sel,drop.destination,drop.move);
    }
    for(size_t i=0;i<drop.count;++i) free(drop.names[i]);
    free(drop.names); free(drop.directory); free(drop.destination); memset(&drop,0,sizeof drop);
}
int main(int argc,char **argv)
{
    /* --headless must work on machines with no DRM device, tty or GL context. */
    for(int i=1;i<argc;++i) if(!strcmp(argv[i],"--headless")) return headless_main(argc,argv);
    rmp_platform_options options; rmp_platform_default_options(&options);
    options.close_key=16; /* KEY_Q: the X11 close button asks to quit (q); F10 is Unpack here */
    const char *directory="."; int have_directory=0,size_cols=-1,size_rows=-1;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"-v")) { puts("FDTP Version " FDTP_VERSION); return 0; }
        else if(!strcmp(argv[i],"--kms")) options.output=RMP_OUTPUT_KMS;
        else if(!strcmp(argv[i],"--x11")) options.output=RMP_OUTPUT_X11;
        else if(!strcmp(argv[i],"--fullscreen")) options.fullscreen=1;
        else if(!strcmp(argv[i],"--size") && i+1<argc && !screen_parse(argv[i+1],&size_cols,&size_rows)) ++i;
        else if(argv[i][0]=='-' || have_directory) { fprintf(stderr,"usage: fdtp [--kms|--x11] [--fullscreen] [--size auto|COLSxROWS] [DIRECTORY]\n"); return 2; }
        else { directory=argv[i]; have_directory=1; }
    }
    App a; app_init(&a); fileops_tree_install(&a);
    /* An asked size is known before the display opens (the X window's first
     * size); auto waits for the surface. */
    screen_apply(app_screen_choice(&a,size_cols,size_rows,0,0));
    Screen *screen=filer_create(directory);
    if(!screen) { perror(directory); return 1; }
    app_push(&a,screen);
    config_first_run(&a, 0);
    pomera_notice_start(&a, 0);   /* revision 1 §6 */
    Canvas *canvas=malloc(sizeof(*canvas)); Cells cells; cells_init(&cells);
    Texture texture={0}; int result=0,mods=0;
    if(!canvas) { app_destroy(&a); return 1; }
    canvas_init(canvas);
    if(init_platform(&options)<0) { free(canvas); app_destroy(&a); return 1; }
    a.output=options.output==RMP_OUTPUT_X11 ? "X" : "KMS";   /* the system information (item 31) */
    mouse_app=&a; rmp_dnd_accept(dnd_over,dnd_drop,&a);   /* -1 on KMS: nothing to take */
    a.is_fullscreen=rmp_platform_is_fullscreen;
    a.set_fullscreen=rmp_platform_set_fullscreen;
    if(fit_screen(&a,size_cols,size_rows,&texture,canvas,&cells)<0) { result=1; goto done; }
    rmp_set_frame_interval(2); rmp_frame_set_clear_color(0x000055); rmp_frame_invalidate_all();
    const char *script=getenv("FDTP_TEST_KEYS"),*prefix=getenv("FDTP_SNAPSHOT_PREFIX");
    ScriptRunner runner; headless_script_init(&runner,script ? script : "",prefix ? prefix : "fdtp-snap",rmp_now_seconds());
    pid_t children[32]={0};
    /* FDTP_TRACE=FILE (revision 1 item 26): every key code, and every time
     * the loop or a key's handling and drawing took long (a stall of the
     * Pomera's SD card shows as a gap), with the time of day. */
    FILE *trace=NULL; { const char *t=getenv("FDTP_TRACE"); if(t && *t) trace=fopen(t,"a"); }
    double loop_at=rmp_now_seconds(),key_at=0;
    /* AWAKE_MINUTES (revision 1 item 28): until that long after the last key
     * (or the start), one cell of the unchanged texture is drawn again every
     * APP_AWAKE_PERIOD, so that the Pomera's GPU (asleep after 0.2 s) and
     * vblank interrupt (after a few seconds) stay awake and a key is shown
     * at once. Nothing on the screen changes. Not in an X window. */
    double awake_key=rmp_now_seconds(),awake_poke=0;
    while(!a.quit && !rmp_quit_requested()) {
        double awake_wait=-1,timeout=0.1;
        if(options.output!=RMP_OUTPUT_X11) {
            if(app_awake_due(a.settings.awake_minutes,awake_key,awake_poke,rmp_now_seconds(),&awake_wait)) timeout=0;
            else if(awake_wait>=0 && awake_wait<timeout) timeout=awake_wait;
        }
        if(rmp_wait(timeout)<0) { result=1; break; }
        if(trace) {
            double now=rmp_now_seconds();
            if(now-loop_at>0.3) fprintf(trace,"%.3f gap %.0f ms\n",now,(now-loop_at)*1000);
            loop_at=now;
        }
        for(int i=0;i<32;++i) if(children[i]) {
            int status; pid_t p=waitpid(children[i],&status,WNOHANG);
            if(p>0) { children[i]=0; app_external_done(&a,status); }
        }
        app_tick(&a,(double)time(NULL));
        rmp_key_event events[32]; int count=rmp_poll_keys(events,32);
        /* Track modifier events in order, including press/release in one poll. */
        static const int modifier_codes[]={KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_LEFTCTRL,KEY_RIGHTCTRL,KEY_LEFTALT,KEY_RIGHTALT};
        static const int modifier_bits[]={MOD_SHIFT,MOD_SHIFT,MOD_CTRL,MOD_CTRL,MOD_ALT,MOD_ALT};
        static int held[6];
        for(int i=0;i<count;++i) {
            for(int j=0;j<6;++j) if(events[i].code==modifier_codes[j]) held[j]=events[i].value!=0;
            mods=0; for(int j=0;j<6;++j) if(held[j]) mods|=modifier_bits[j];
            app_modifiers(&a,mods);   /* the filer's Shift band (revision 1 §2) */
            if(trace) { fprintf(trace,"%.3f key %d %d mods %d\n",rmp_now_seconds(),events[i].code,events[i].value,mods); if(events[i].value) key_at=rmp_now_seconds(); }
            if(events[i].value) { awake_key=rmp_now_seconds(); a.last_key=(double)time(NULL); }
            Key key; if(!events[i].value || !keys_evdev(events[i].code,mods,&key)) continue;
            if(options.output==RMP_OUTPUT_X11 && key.key==KCHAR && key.ch=='n' && key.mods==MOD_CTRL) {
                /* Ctrl+N in the filer (item 15); elsewhere, and on KMS, Ctrl+n is what it was. */
                Screen *s=filer_screen(); char *here=s && app_top(&a)==s ? filer_drop_directory(s,&a,-1,-1) : NULL;
                if(here) { new_window(&a,here,size_cols,size_rows); free(here); continue; }
            }
            app_key(&a,&key);
            if(a.external.pending || a.quit) break;
        }
        if(!a.external.pending && !a.quit) { pointer_events(&a); take_drop(&a); }
        if(script && !runner.ended && !a.external.pending && !a.quit) {
            unsigned snapshots=runner.snapshots;
            int step=headless_script_step(&runner,&a,canvas,&cells,rmp_now_seconds());
            if(step<0) { result=1; break; }
            if(snapshots!=runner.snapshots) cells_invalidate(&cells);
        }
        if(a.external.pending) {
            int slot=0; while(slot<32 && children[slot]) ++slot;
            if(slot==32) { app_external_clear(&a); app_notice(&a,T("外部コマンドが多すぎます")); }
            else if(run_external(&a,&texture,&cells,canvas,&options,&children[slot])<0) { result=1; break; }
            memset(held,0,sizeof(held)); mods=0; app_modifiers(&a,0);
            awake_key=rmp_now_seconds(); a.last_key=(double)time(NULL);   /* back from the command: as a key */
        }
        if(a.quit || rmp_quit_requested()) break;
        if(options.output!=RMP_OUTPUT_X11 && app_awake_due(a.settings.awake_minutes,awake_key,awake_poke,rmp_now_seconds(),NULL)) {
            rmp_rect cell={0,0,12,24}; rmp_frame_damage(&cell); awake_poke=rmp_now_seconds();
        }
        if(!rmp_frame_ready()) continue;
        /* rmp_frame_ready has read the X window's new size, if any. */
        if(fit_screen(&a,size_cols,size_rows,&texture,canvas,&cells)<0) { result=1; break; }
        int changed=a.changed; Screen *top=app_top(&a);
        if(changed) app_draw(&a,canvas,&cells);
        FDRect damage[FD_ROWS_MAX]; int dirty=0;
        if(top && top->pixel) {
            if(changed || canvas->full_dirty) { damage[0]=(FDRect){0,0,CANVAS_W,CANVAS_H}; dirty=1; canvas->full_dirty=0; }
        } else {
            /* The rows cover the canvas but for the navy under the grid of a
             * screen whose height is not a whole number of rows. */
            int whole=canvas->full_dirty && CANVAS_H>FD_H;
            dirty=raster_update(canvas,&cells,damage);
            if(whole) { damage[0]=(FDRect){0,0,CANVAS_W,CANVAS_H}; dirty=1; }
        }
        double t0=trace ? rmp_now_seconds() : 0;
        texture_update(&texture,canvas,damage,dirty);
        double t1=trace ? rmp_now_seconds() : 0;
        rmp_rect repaint[RMP_REPAINT_MAX]; count=rmp_frame_begin(repaint,RMP_REPAINT_MAX);
        if(count<0) { result=1; break; }
        double t2=trace ? rmp_now_seconds() : 0;
        if(count) texture_draw(&texture,repaint,count);
        double t3=trace ? rmp_now_seconds() : 0;
        if(rmp_frame_end()<0) { result=1; break; }
        if(trace && key_at) {
            double t4=rmp_now_seconds();
            fprintf(trace,"%.3f drawn %.0f ms after the key (upload %.0f, begin %.0f, draw %.0f, end %.0f)\n",
                    t4,(t4-key_at)*1000,(t1-t0)*1000,(t2-t1)*1000,(t3-t2)*1000,(t4-t3)*1000);
            fflush(trace); key_at=0;
        }
    }
    if(trace) fclose(trace);
done:
    texture_destroy(&texture); rmp_platform_shutdown(); free(canvas); app_destroy(&a); return result;
}
