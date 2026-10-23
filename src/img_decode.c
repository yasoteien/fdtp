// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "img_decode.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void img_size(int w,int h,int mw,int mh,int *ow,int *oh)
{
    double s=1;
    if(w>mw) s=(double)mw/w;
    if(h*s>mh) s=(double)mh/h;
    *ow=(int)(w*s); *oh=(int)(h*s);
    if(*ow<1) *ow=1;
    if(*oh<1) *oh=1;
}
int img_alloc(Img *im,int w,int h)
{
    if(w<1 || h<1 || (uint64_t)w*h>8388608) return -1;
    im->pixels=calloc((size_t)w*h,3);
    if(!im->pixels) return -1;
    im->width=w; im->height=h; return 0;
}
void img_free(Img *im) { free(im->pixels); memset(im,0,sizeof(*im)); }
int img_load(const char *p,int mw,int mh,Img *im,char *err,size_t n)
{
    unsigned char b[12]={0}; FILE *f=fopen(p,"rb");
    memset(im,0,sizeof(*im));
    if(!f) { snprintf(err,n,"%s", T("ファイルを開けません")); return -1; }
    size_t got=fread(b,1,sizeof(b),f); fclose(f);
    if(mw<1 || mh<1) return -1;
    int rc;
    if(got>=2 && b[0]=='B' && b[1]=='M') rc=img_bmp_load(p,mw,mh,im,err,n);
    else if((got>=8 && !memcmp(b,"MAKI02  ",8)) || (got>=2 && !memcmp(b,"Pi",2)))
        rc=img_legacy_load(p,mw,mh,im,err,n);
    else rc=img_modern_load(p,mw,mh,im,err,n);
    if(rc) { img_free(im); if(n && !err[0]) snprintf(err,n,"%s", T("壊れた画像、または未知の形式です")); }
    return rc;
}
