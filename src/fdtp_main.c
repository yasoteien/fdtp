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
int main(int argc,char **argv)
{
    /* --headless must work on machines with no DRM device, tty or GL context. */
    for(int i=1;i<argc;++i) if(!strcmp(argv[i],"--headless")) return headless_main(argc,argv);
    rmp_platform_options options; rmp_platform_default_options(&options);
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
            Key key; if(events[i].value && keys_evdev(events[i].code,mods,&key)) app_key(&a,&key);
            if(a.external.pending || a.quit) break;
        }
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
