// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "i18n.h"
#include "app.h"
#include "filer.h"
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void app_init(App *a)
{
    memset(a,0,sizeof(*a)); widgets_init(&a->widgets); commands_init(&a->commands); a->changed=1;
    config_startup(a);
}
ScreenSize app_screen_choice(const App *a,int cols,int rows,int out_w,int out_h)
{
    if(cols<0 || rows<0) { cols=a->settings.screen_cols; rows=a->settings.screen_rows; }
    return screen_choose(cols,rows,out_w,out_h);
}
int app_screen_differs(ScreenSize s)
{
    return s.cols!=FD_COLS || s.rows!=FD_ROWS || s.canvas_w!=CANVAS_W || s.canvas_h!=CANVAS_H;
}
Screen *app_top(App *a) { return a->depth ? a->screens[a->depth-1] : NULL; }
int app_push(App *a,Screen *s)
{
    if(!s || a->depth==APP_SCREENS) return -1;
    a->screens[a->depth++]=s; a->changed=1; filer_apply_settings(s,a); return 0;
}
void app_pop(App *a)
{
    if(!a->depth) return;
    Screen *s=a->screens[--a->depth];
    if(s->destroy) s->destroy(s);
    a->changed=1;
    if(!a->depth) a->quit=1;
}
void app_external_clear(App *a)
{
    for(int i=0;i<APP_EXTERNAL_ARGS;++i) { free(a->external.argv[i]); a->external.argv[i]=NULL; }
    free(a->external.cwd); memset(&a->external,0,sizeof(a->external));
}
void app_destroy(App *a) { while(a->depth) app_pop(a); app_external_clear(a); config_free(a); widgets_history_close(); }
static void show_deferred(App *a)
{
    if(!a->deferred_notice[0]) return;
    if(a->deferred_error) widgets_error(&a->widgets,a->deferred_notice); else widgets_notice(&a->widgets,a->deferred_notice);
    a->deferred_notice[0]=0; a->deferred_error=0;
}
void app_notice(App *a,const char *message)
{
    if(a->widgets.kind!=WIDGET_NONE && a->widgets.kind!=WIDGET_NOTICE)
        { snprintf(a->deferred_notice,sizeof(a->deferred_notice),"%s",message); a->deferred_error=0; }
    else widgets_notice(&a->widgets,message);
    a->changed=1;
}
void app_error(App *a,const char *message)
{
    if(a->widgets.kind!=WIDGET_NONE && a->widgets.kind!=WIDGET_NOTICE)
        { snprintf(a->deferred_notice,sizeof(a->deferred_notice),"%s",message); a->deferred_error=1; }
    else widgets_error(&a->widgets,message);
    a->changed=1;
}
void app_key(App *a,const Key *k)
{
    if(a->quit) return;
    if(a->first_run_key && a->first_run_key(a,k)) { a->changed=1; return; }
    /* An error, information or help box takes the key (one that closes it,
     * or scrolls it: revision 1 item 25). */
    if(a->widgets.kind==WIDGET_INFO || a->widgets.kind==WIDGET_ERROR || a->widgets.kind==WIDGET_HELP) {
        widgets_key(&a->widgets,k);
        if(a->widgets.kind==WIDGET_NONE) show_deferred(a);
        a->changed=1; return;
    }
    if(a->widgets.kind==WIDGET_NOTICE) widgets_key(&a->widgets,k);
    Screen *s=app_top(a);
    if(s && s->key && s->key(s,a,k)==1 && app_top(a)==s) app_pop(a);
    if(a->widgets.kind==WIDGET_NONE) show_deferred(a);
    a->changed=1;
}
void app_modifiers(App *a,int mods)
{
    if((a->mods ^ mods) & MOD_SHIFT) a->changed=1;
    a->mods=mods;
}
void app_tick(App *a,double now)
{
    if((long long)(a->now/60)!=(long long)(now/60)) a->changed=1;
    a->now=now;
    Screen *s=app_top(a); if(s && s->tick) s->tick(s,a,now);
}
int app_awake_due(int minutes,double last_key,double last_poke,double now,double *wait)
{
    double end=last_key+minutes*60.0, next=last_poke+APP_AWAKE_PERIOD;
    if(minutes<=0 || now>=end) { if(wait) *wait=-1; return 0; }
    if(now>=next) {
        if(wait) *wait=now+APP_AWAKE_PERIOD<end ? APP_AWAKE_PERIOD : -1;
        return 1;
    }
    if(wait) *wait=next<end ? next-now : -1;
    return 0;
}
void app_draw(App *a,Canvas *canvas,Cells *cells)
{
    Screen *s=app_top(a); if(!s) return;
    if(s->pixel) {
        if(!a->last_pixel) canvas_clear(canvas,0x000055);
        a->last_pixel=1;
        s->draw(s,a,canvas,cells); canvas->full_dirty=1; cells_invalidate(cells); a->changed=0; return;
    }
    if(a->last_pixel) { canvas_clear(canvas,0x000055); cells_invalidate(cells); a->last_pixel=0; }
    Cells next; cells_init(&next);
    s->draw(s,a,canvas,&next); widgets_draw(&a->widgets,&next); if(a->first_run_draw) a->first_run_draw(a,&next);
    for(int y=0;y<FD_ROWS;++y) for(int x=0;x<FD_COLS;++x) {
        Cell old=cells->data[y][x], v=next.data[y][x];
        next.dirty[y][x]=cells->dirty[y][x] || old.code!=v.code || old.fg!=v.fg || old.bg!=v.bg || old.part!=v.part;
    }
    next.lines_dirty=cells->lines_dirty || cells->frame!=next.frame || !cells_same_separators(cells,&next) ||
        cells->title_start!=next.title_start || cells->title_end!=next.title_end ||
        cells->date_start!=next.date_start || cells->date_end!=next.date_end || !cells_same_boxes(cells,&next);
    cells_copy(cells,&next); a->changed=0;
}
int app_request_external_at(App *a,const char *const *argv,unsigned flags,const char *cwd)
{
    if(a->external.pending || !argv || !argv[0] || !*argv[0]) return -1;
    ExternalRequest next={0}; int i;
    for(i=0;argv[i] && i<APP_EXTERNAL_ARGS-1;++i) {
        next.argv[i]=strdup(argv[i]); if(!next.argv[i]) goto fail;
    }
    if(argv[i]) goto fail;
    if(cwd && !(next.cwd=strdup(cwd))) goto fail;
    next.flags=flags; next.pending=1; a->external=next; return 0;
fail:
    for(i=0;i<APP_EXTERNAL_ARGS;++i) free(next.argv[i]);
    free(next.cwd); return -1;
}
int app_request_external(App *a,const char *const *argv,unsigned flags)
{ return app_request_external_at(a,argv,flags,NULL); }
void app_external_done(App *a,int status)
{
    app_external_clear(a);
    for(int i=0;i<a->depth;++i) filer_reload(a->screens[i],a);
    if(status) app_notice(a,T("外部コマンドが正常に終了しませんでした"));
    a->changed=1;
}
