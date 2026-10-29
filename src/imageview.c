// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "imageview.h"
#include "img_decode.h"
#include "i18n.h"
#include "app.h"
#include "raster.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
    Screen screen;
    Img image;
    char *path;
    double zoom, started;
    int panx,pany;
    Canvas *bands;
} ImageView;
static double fit(const Img *im)
{
    double z=fmin(1.,fmin((double)VIEW_W/im->original_width,(double)VIEW_H/im->original_height));
    return z;
}
static int reload(ImageView *v,App *a,double z)
{
    int mw=(int)fmax(1.,ceil(v->image.original_width*fmin(z,1.)));
    int mh=(int)fmax(1.,ceil(v->image.original_height*fmin(z,1.)));
    /* A display cache stays below 24 MiB even when zooming into huge files. */
    double limit=sqrt(8388608./((double)mw*mh));
    if(limit<1) { mw=(int)(mw*limit); mh=(int)(mh*limit); }
    Img im={0}; char err[192]={0};
    if(img_load(v->path,mw,mh,&im,err,sizeof(err))) { app_notice(a,err); return -1; }
    img_free(&v->image); v->image=im; v->zoom=z; return 0;
}
static int is_image(const char *name)
{
    const char *e=strrchr(name,'.');
    if(!e) return 0;
    const char *ext[]={".jpg",".jpeg",".png",".gif",".bmp",".webp",".heic",".heif",".mag",".pi"};
    for(size_t i=0;i<sizeof(ext)/sizeof(*ext);i++) if(!strcasecmp(e,ext[i])) return 1;
    return 0;
}
static int adjacent(ImageView *v,App *a,int direction)
{
    char *dir=strdup(v->path); if(!dir) return 0;
    char *slash=strrchr(dir,'/'); const char *name=strrchr(v->path,'/');
    name=name?name+1:v->path;
    if(slash) { if(slash==dir) slash[1]=0; else *slash=0; } else strcpy(dir,".");
    DIR *d=opendir(dir); if(!d) { free(dir); return 0; }
    char *best=NULL; struct dirent *e;
    while((e=readdir(d))) {
        if(!is_image(e->d_name)) continue;
        int c=strcmp(e->d_name,name);
        if((direction>0?c<=0:c>=0) || (best && (direction>0?strcmp(e->d_name,best)>=0:strcmp(e->d_name,best)<=0))) continue;
        size_t n=strlen(dir)+strlen(e->d_name)+2;
        char *p=malloc(n); if(!p) continue;
        snprintf(p,n,"%s/%s",dir,e->d_name); struct stat st;
        int regular=!stat(p,&st) && S_ISREG(st.st_mode); free(p);
        if(regular) { free(best); best=strdup(e->d_name); }
    }
    closedir(d);
    if(best) {
        size_t n=strlen(dir)+strlen(best)+2; char *p=malloc(n);
        if(p) {
            snprintf(p,n,"%s/%s",dir,best); Img im={0}; char err[192]={0};
            if(img_load(p,VIEW_W,VIEW_H,&im,err,sizeof(err))) { app_notice(a,err); free(p); free(best); free(dir); return 1; }
            free(v->path); v->path=p; img_free(&v->image); v->image=im;
            v->zoom=fit(&im); v->panx=v->pany=0; v->started=a->now;
        }
    } else app_notice(a,direction>0?T("次の画像はありません"):T("前の画像はありません"));
    free(best); free(dir); return 0;
}
static int image_key(Screen *s,App *a,const Key *k)
{
    ImageView *v=s->self;
    if(k->key==KESC || (k->key==KCHAR && k->ch=='q')) return 1;
    if(k->key==KBS || (k->key==KCHAR && k->ch==' ')) return adjacent(v,a,k->key==KBS?-1:1);
    if(k->key==KLEFT) v->panx-=64;
    if(k->key==KRIGHT) v->panx+=64;
    if(k->key==KUP) v->pany-=64;
    if(k->key==KDOWN) v->pany+=64;
    if(k->key==KCHAR) {
        double z=v->zoom;
        if(k->ch=='+') z=fmin(8.,fmax(.125,z*2));
        if(k->ch=='-') z=fmax(.125,z/2);
        if(k->ch=='0') z=fit(&v->image);
        if(k->ch=='1') z=1;
        if(z!=v->zoom) { if(reload(v,a,z)) return 1; v->panx=v->pany=0; }
        if(k->ch=='i') {
            char info[192]; char colors[32];
            if(v->image.colors>=16777216u) snprintf(colors,sizeof(colors),"%s", T("フルカラー"));
            else if(i18n_lang()==I18N_EN && v->image.colors==1) snprintf(colors,sizeof(colors),"%u color",v->image.colors);
            else snprintf(colors,sizeof(colors),T("%u 色"),v->image.colors);
            snprintf(info,sizeof(info),"%d x %d  %s  %s%s",v->image.original_width,v->image.original_height,v->image.format,colors,v->image.frames>1?T("  動画"):"");
            app_notice(a,info);
        }
    }
    return 0;
}
static void image_draw(Screen *s,App *a,Canvas *c,Cells *cells)
{
    ImageView *v=s->self; Img *im=&v->image;
    int dw=(int)fmax(1.,round(im->original_width*v->zoom));
    int dh=(int)fmax(1.,round(im->original_height*v->zoom));
    int px=dw>VIEW_W?(dw-VIEW_W)/2:0, py=dh>VIEW_H?(dh-VIEW_H)/2:0;
    if(v->panx>px) v->panx=px;
    if(v->panx< -px) v->panx=-px;
    if(v->pany>py) v->pany=py;
    if(v->pany< -py) v->pany=-py;
    int left=(VIEW_W-dw)/2-v->panx,top=VIEW_TOP+(VIEW_H-dh)/2-v->pany;
    canvas_clear(c,0x000055);
    for(int y=VIEW_TOP;y<VIEW_TOP+VIEW_H;y++) {
        int sy=y-top; if(sy<0 || sy>=dh) continue;
        int iy=(int)((long long)sy*im->height/dh);
        for(int x=0;x<VIEW_W;x++) {
            int sx=x-left; if(sx<0 || sx>=dw) continue;
            int ix=(int)((long long)sx*im->width/dw);
            memcpy(CANVAS_PX(c,VIEW_LEFT+x,y),im->pixels+((size_t)iy*im->width+ix)*3,3);
        }
    }
    /* Revision 1 item 29: the picture in the screen's frame (cells.h), the
     * name in its top line, the zoom in its bottom line. */
    cells_init(cells); frame_open(cells);
    const char *base=strrchr(v->path,'/'); base=base?base+1:v->path;
    char zoom[32]; snprintf(zoom,sizeof(zoom),"%.1f%%",v->zoom*100);
    frame_title(cells,FRAME_TITLE_COL,base,FRAME_STATUS_END-FRAME_TITLE_COL-12,NULL,0);
    frame_status(cells,FRAME_STATUS_END,zoom,20);
    static const FrameKey keys[]={{"+",N_("拡大")},{"-",N_("縮小")},{"0",N_("全体")},{"1",N_("等倍")},{"SPC",N_("次")},{"BS",N_("前")},{"i",N_("情報")},{"q",N_("戻る")}};
    FrameKey translated[8];
    for(int i=0;i<8;i++) translated[i]=(FrameKey){keys[i].key,T(keys[i].name)};
    frame_keys(cells,2,translated,8);
    widgets_draw(&a->widgets,cells);
    raster_bands(c,v->bands,cells,widgets_row23(&a->widgets));
    raster_boxes(c,cells);
    c->full_dirty=1;
}
/* Only an animated GIF changes with time: the frame is decoded again (from
 * the file, no frame array is kept) when the shown one has run its delay. */
static void image_tick(Screen *s,App *a,double now)
{
    ImageView *v=s->self;
    if(v->image.frames<2 || !isfinite(v->image.frame_end) || now-v->started<v->image.frame_end) return;
    Img im={0}; char err[192]={0};
    if(img_gif_load(v->path,v->image.width,v->image.height,now-v->started,&im,err,sizeof(err))) {
        app_notice(a,err); app_pop(a); return;
    }
    img_free(&v->image); v->image=im; a->changed=1;
}
static void image_destroy(Screen *s)
{
    ImageView *v=s->self; img_free(&v->image); free(v->path); free(v->bands); free(v);
}
Screen *imageview_create(App *a,const char *path)
{
    ImageView *v=calloc(1,sizeof(*v)); if(!v) { app_notice(a,T("メモリが足りません")); return NULL; }
    char err[192]={0};
    if(!path || !(v->path=strdup(path)) || !(v->bands=malloc(sizeof(Canvas))) || img_load(path,VIEW_W,VIEW_H,&v->image,err,sizeof(err))) {
        app_notice(a,err[0]?err:T("画像を開けません")); img_free(&v->image); free(v->path); free(v->bands); free(v); return NULL;
    }
    v->zoom=fit(&v->image); v->started=a->now;
    v->screen=(Screen){v,image_key,image_draw,1,image_tick,image_destroy};
    return &v->screen;
}
