// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Isolated viewer entrance; no launcher/fileops/textview linkage. */
#include "imageview.h"
#include "img_decode.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <webp/encode.h>
/* Supplemental fixtures use only the same linked decoder libraries. The
 * standard-library Python generator still skips sips when unavailable. */
static int make_modern(const char *dir)
{
    unsigned char rgb[16*16*3];
    static const unsigned char colors[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,255}};
    for(int y=0;y<16;y++) for(int x=0;x<16;x++) memcpy(rgb+(y*16+x)*3,colors[(y>=8)*2+(x>=8)],3);
    char path[4096];
    for(int o=0;o<=8;o++) {
        if(o) snprintf(path,sizeof(path),"%s/exif%d.jpg",dir,o);
        else snprintf(path,sizeof(path),"%s/quadrants.jpg",dir);
        FILE *f=fopen(path,"wb"); if(!f) return 2;
        struct jpeg_compress_struct c; struct jpeg_error_mgr e;
        c.err=jpeg_std_error(&e); jpeg_create_compress(&c); jpeg_stdio_dest(&c,f);
        c.image_width=c.image_height=16; c.input_components=3; c.in_color_space=JCS_RGB;
        jpeg_set_defaults(&c); c.comp_info[0].h_samp_factor=c.comp_info[0].v_samp_factor=1;
        jpeg_set_quality(&c,100,TRUE); jpeg_start_compress(&c,TRUE);
        if(o) {
            unsigned char ex[]={ 'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,0,0,0,0,0,0,0,0};
            ex[24]=(unsigned char)o; jpeg_write_marker(&c,JPEG_APP0+1,ex,sizeof(ex));
        }
        while(c.next_scanline<16) { JSAMPROW row=rgb+c.next_scanline*48; jpeg_write_scanlines(&c,&row,1); }
        jpeg_finish_compress(&c); jpeg_destroy_compress(&c); fclose(f);
    }
    /* 2048x1024 quadrants: decoding it small makes libjpeg use scale_denom. */
    snprintf(path,sizeof(path),"%s/large.jpg",dir);
    FILE *lf=fopen(path,"wb"); if(!lf) return 2;
    unsigned char *line=malloc(2048*3); if(!line) { fclose(lf); return 2; }
    struct jpeg_compress_struct c; struct jpeg_error_mgr e;
    c.err=jpeg_std_error(&e); jpeg_create_compress(&c); jpeg_stdio_dest(&c,lf);
    c.image_width=2048; c.image_height=1024; c.input_components=3; c.in_color_space=JCS_RGB;
    jpeg_set_defaults(&c); jpeg_set_quality(&c,90,TRUE); jpeg_start_compress(&c,TRUE);
    while(c.next_scanline<1024) {
        for(int x=0;x<2048;x++) memcpy(line+x*3,colors[(c.next_scanline>=512)*2+(x>=1024)],3);
        JSAMPROW row=line; jpeg_write_scanlines(&c,&row,1);
    }
    jpeg_finish_compress(&c); jpeg_destroy_compress(&c); fclose(lf); free(line);
    unsigned char *encoded=NULL; size_t n=WebPEncodeLosslessRGB(rgb,16,16,48,&encoded);
    if(!n) return 2;
    snprintf(path,sizeof(path),"%s/quadrants.webp",dir); FILE *f=fopen(path,"wb");
    if(!f) { WebPFree(encoded); return 2; }
    int rc=fwrite(encoded,1,n,f)==n?0:2; fclose(f); WebPFree(encoded); return rc;
}
int main(int argc,char **argv)
{
    if(argc<3) return 2;
    if(!strcmp(argv[1],"--make-modern")) return make_modern(argv[2]);
    if(!strcmp(argv[1],"--decode")) {
        if(argc!=6) return 2;
        Img im={0}; char err[256]={0};
        if(img_load(argv[2],atoi(argv[3]),atoi(argv[4]),&im,err,sizeof(err))) { fprintf(stderr,"%s\n",err); return 3; }
        FILE *f=fopen(argv[5],"wb"); if(!f) { img_free(&im); return 2; }
        fprintf(f,"P6\n%d %d\n255\n",im.width,im.height); fwrite(im.pixels,3,(size_t)im.width*im.height,f); fclose(f);
        printf("%s %d %d %u\n",im.format,im.original_width,im.original_height,im.colors); img_free(&im); return 0;
    }
    if(argc!=4) return 2;
    FILE *f=fopen(argv[2],"rb"); if(!f) return 2;
    char text[65536]; size_t n=fread(text,1,sizeof(text)-1,f); fclose(f); text[n]=0;
    App a; app_init(&a); Screen *s=imageview_create(&a,argv[1]);
    if(!s) { fprintf(stderr,"%s\n",a.widgets.label); app_destroy(&a); return 3; }
    app_push(&a,s); Canvas *c=malloc(sizeof(*c)); Cells cells; cells_init(&cells);
    if(!c) { app_destroy(&a); return 2; } canvas_init(c);
    KeyScript script; keys_script_init(&script,text); unsigned serial=0; int rc=0;
    for(;;) {
        ScriptEvent e=keys_script_next(&script);
        if(e.kind==SCRIPT_END) break;
        if(e.kind==SCRIPT_ERROR) { rc=2; break; }
        if(e.kind==SCRIPT_WAIT) { app_tick(&a,a.now+e.seconds); continue; }
        if(e.kind==SCRIPT_KEY) { app_key(&a,&e.key); continue; }
        if(e.kind==SCRIPT_SNAP) {
            /* Back in the (absent) list: record the notice, no canvas. */
            if(!app_top(&a)) { printf("SNAP %u 0 %s\n",++serial,widgets_message(&a.widgets)?a.widgets.label:""); continue; }
            app_draw(&a,c,&cells); char path[4096]; snprintf(path,sizeof(path),"%s-%03u.ppm",argv[3],++serial);
            if(canvas_write_ppm(c,path)) { rc=2; break; }
            printf("SNAP %u %d %s\n",serial,a.depth,widgets_message(&a.widgets)?a.widgets.label:"");
        }
    }
    app_destroy(&a); free(c); return rc;
}
