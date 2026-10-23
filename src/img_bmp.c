// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "img_decode.h"
#include "i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Accumulate directly into the display image: even RLE's unpainted areas
 * need no full-resolution pixel buffer. Coordinates use an integer area grid. */
typedef struct {
    int w, h, dw, dh, topdown;
    double *sum;
    unsigned char pal[256][3];
    unsigned npal;
    int base[3];
} Bmp;

static uint16_t u16(const unsigned char *p)
{ return (uint16_t)(p[0] | (unsigned)p[1] << 8); }
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int32_t s32(const unsigned char *p)
{ uint32_t v=u32(p); return v<=INT32_MAX ? (int32_t)v : (int32_t)(-1-(int64_t)(UINT32_MAX-v)); }

static void add(Bmp *b, int x, int y, int n, const unsigned char rgb[3])
{
    int64_t l=(int64_t)x*b->dw, r=(int64_t)(x+n)*b->dw;
    int sy=b->topdown ? y : b->h-1-y;
    int64_t t=(int64_t)sy*b->dh, bot=(int64_t)(sy+1)*b->dh;
    int dx0=(int)(l/b->w), dx1=(int)((r-1)/b->w);
    int dy0=(int)(t/b->h), dy1=(int)((bot-1)/b->h);
    for (int dy=dy0;dy<=dy1;dy++) {
        int64_t a=t>(int64_t)dy*b->h ? t : (int64_t)dy*b->h;
        int64_t z=bot<(int64_t)(dy+1)*b->h ? bot : (int64_t)(dy+1)*b->h;
        for (int dx=dx0;dx<=dx1;dx++) {
            int64_t c=l>(int64_t)dx*b->w ? l : (int64_t)dx*b->w;
            int64_t d=r<(int64_t)(dx+1)*b->w ? r : (int64_t)(dx+1)*b->w;
            double weight=(double)(z-a)*(double)(d-c);
            double *v=b->sum+((size_t)dy*b->dw+dx)*3;
            for(int k=0;k<3;k++) v[k]+=(rgb[k]-b->base[k])*weight;
        }
    }
}

static int mask_ok(uint32_t m)
{
    if (!m) return 0;
    while (!(m&1)) m>>=1;
    return (m & (m+1))==0;
}
static unsigned channel(uint32_t v,uint32_t mask)
{
    if (!mask) return 255;
    while (!(mask&1)) { mask>>=1; v>>=1; }
    return (unsigned)(((uint64_t)(v&mask)*255+mask/2)/mask);
}

int img_bmp_load(const char *path,int maxw,int maxh,Img *out,char *err,size_t errlen)
{
    int fd=-1, rc=-1;
    size_t len=0;
    unsigned char *data=MAP_FAILED;
    const char *why=T("壊れた BMP です");
    struct stat st;
    Bmp b;
    memset(&b,0,sizeof b);
    fd=open(path,O_RDONLY);
    if(fd<0 || fstat(fd,&st)<0) { why=T("BMP を読めません"); goto done; }
    if(st.st_size<26 || (uintmax_t)st.st_size>SIZE_MAX) goto done;
    len=(size_t)st.st_size;
    data=mmap(NULL,len,PROT_READ,MAP_PRIVATE,fd,0);
    if(data==MAP_FAILED) { why=T("BMP を読めません"); goto done; }
    if(data[0]!='B' || data[1]!='M') goto done;
    uint32_t declared=u32(data+2), off=u32(data+10), dib=u32(data+14);
    if(declared) { if(declared>len || declared<26) goto done; len=declared; }
    if(off>len || dib>len-14) goto done;
    unsigned depth,compression=0,used=0,palsize;
    size_t paloff;
    uint32_t masks[4]={0x00ff0000,0x0000ff00,0x000000ff,0};
    if(dib==12) {
        b.w=u16(data+18); b.h=u16(data+20);
        if(u16(data+22)!=1) goto done;
        depth=u16(data+24); palsize=3; paloff=26;
    } else if(dib>=40) {
        b.w=s32(data+18);
        int32_t sh=s32(data+22);
        if(sh==INT32_MIN || sh==0 || b.w<=0 || u16(data+26)!=1) goto done;
        b.topdown=sh<0; b.h=sh<0 ? -sh : sh;
        depth=u16(data+28); compression=u32(data+30); used=u32(data+46);
        palsize=4; paloff=14+(size_t)dib;
        if(compression==3 || compression==6) {
            if(depth!=32) { why=T("未対応の BMP ビットフィールドです"); goto done; }
            size_t mp=54;
            unsigned count=compression==6 ? 4 : 3;
            if(dib>=52) { if(compression==6 && dib<56) goto done; }
            else if(dib==40) { paloff+=count*4; }
            else goto done;
            if(mp>len || count*4>len-mp) goto done;
            for(unsigned k=0;k<count;k++) masks[k]=u32(data+mp+k*4);
            if(dib>=56) masks[3]=u32(data+66);
        } else if(depth==32 && dib>=56 && u32(data+66)) {
            for(unsigned k=0;k<4;k++) masks[k]=u32(data+54+k*4);
        }
    } else { why=T("未対応の BMP ヘッダーです"); goto done; }
    if(b.w<=0 || b.h<=0) goto done;
    if(depth!=1 && depth!=4 && depth!=8 && depth!=24 && depth!=32) {
        why=T("未対応の BMP 色深度です"); goto done;
    }
    if(!((compression==0) || (compression==1 && depth==8) ||
         (compression==2 && depth==4) || ((compression==3 || compression==6) && depth==32))) {
        why=T("未対応の BMP 圧縮です"); goto done;
    }
    if(b.topdown && (compression==1 || compression==2)) goto done;
    if(depth==32) {
        uint32_t all=0;
        for(int k=0;k<4;k++) {
            if(k==3 && !masks[k]) continue;
            if(!mask_ok(masks[k]) || (all&masks[k])) goto done;
            all|=masks[k];
        }
    }
    if(depth<=8) {
        b.npal=used ? used : 1u<<depth;
        if(b.npal>(1u<<depth) || paloff>off || (size_t)b.npal*palsize>off-paloff) goto done;
        for(unsigned i=0;i<b.npal;i++) {
            const unsigned char *p=data+paloff+(size_t)i*palsize;
            b.pal[i][0]=p[2]; b.pal[i][1]=p[1]; b.pal[i][2]=p[0];
        }
    } else if(paloff>off) goto done;
    img_size(b.w,b.h,maxw,maxh,&b.dw,&b.dh);
    if(b.dw<=0 || b.dh<=0 || (size_t)b.dw>SIZE_MAX/(size_t)b.dh/3/sizeof(double) ||
       (uint64_t)b.dw*b.dh>8u*1024u*1024u) {
        why=T("BMP が大きすぎます"); goto done;
    }
    size_t count=(size_t)b.dw*b.dh*3;
    b.sum=calloc(count,sizeof(double));
    if(!b.sum) { why=T("BMP 用のメモリが足りません"); goto done; }
    if(compression==1 || compression==2) {
        for(int k=0;k<3;k++) b.base[k]=b.pal[0][k];
        size_t pos=off;
        int x=0,y=0,ended=0;
        while(pos<len) {
            if(len-pos<2) goto done;
            unsigned n=data[pos++], v=data[pos++];
            if(n) {
                if(y>=b.h || n>(unsigned)(b.w-x)) goto done;
                if(compression==1) {
                    if(v>=b.npal) goto done;
                    add(&b,x,y,(int)n,b.pal[v]); x+=(int)n;
                } else for(unsigned j=0;j<n;j++) {
                    unsigned ix=(j&1) ? v&15 : v>>4;
                    if(ix>=b.npal) goto done;
                    add(&b,x++,y,1,b.pal[ix]);
                }
            } else if(v==0) {
                if(y>=b.h) goto done;
                x=0; y++;
            } else if(v==1) { ended=1; break; }
            else if(v==2) {
                if(len-pos<2 || y>=b.h) goto done;
                unsigned dx=data[pos++],dy=data[pos++];
                if(dx>(unsigned)(b.w-x) || dy>=(unsigned)(b.h-y)) goto done;
                x+=(int)dx; y+=(int)dy;
            } else {
                size_t bytes=compression==1 ? v : (v+1)/2;
                size_t padded=(bytes+1)&~(size_t)1;
                if(padded>len-pos || y>=b.h || v>(unsigned)(b.w-x)) goto done;
                for(unsigned j=0;j<v;j++) {
                    unsigned ix=compression==1 ? data[pos+j] :
                        ((j&1) ? data[pos+j/2]&15 : data[pos+j/2]>>4);
                    if(ix>=b.npal) goto done;
                    add(&b,x++,y,1,b.pal[ix]);
                }
                pos+=padded;
            }
        }
        if(!ended) goto done;
    } else {
        uint64_t stride=(((uint64_t)b.w*depth+31)/32)*4;
        if(stride>(uint64_t)(len-off)/(unsigned)b.h) goto done;
        for(int y=0;y<b.h;y++) {
            const unsigned char *row=data+off+(size_t)y*(size_t)stride;
            for(int x=0;x<b.w;x++) {
                unsigned char rgb[3];
                if(depth<=8) {
                    unsigned ix=depth==8 ? row[x] : depth==4 ?
                        ((x&1) ? row[x/2]&15 : row[x/2]>>4) :
                        ((row[x/8]>>(7-(x&7)))&1);
                    if(ix>=b.npal) goto done;
                    memcpy(rgb,b.pal[ix],3);
                } else if(depth==24) {
                    const unsigned char *p=row+(size_t)x*3;
                    rgb[0]=p[2]; rgb[1]=p[1]; rgb[2]=p[0];
                } else {
                    uint32_t p=u32(row+(size_t)x*4);
                    unsigned a=channel(p,masks[3]);
                    for(int k=0;k<3;k++) {
                        unsigned c=channel(p,masks[k]);
                        rgb[k]=(unsigned char)((c*a+(k==2 ? 85 : 0)*(255-a)+127)/255);
                    }
                }
                add(&b,x,y,1,rgb);
            }
        }
    }
    if(img_alloc(out,b.dw,b.dh)<0) { why=T("BMP 用のメモリが足りません"); goto done; }
    double area=(double)b.w*b.h;
    for(size_t i=0;i<count;i++) {
        double v=b.sum[i]/area+b.base[i%3];
        out->pixels[i]=(unsigned char)(v<0 ? 0 : v>255 ? 255 : v+0.5);
    }
    out->original_width=b.w; out->original_height=b.h;
    out->colors=depth<=8 ? b.npal : 16777216u; out->format="BMP";
    rc=0;
done:
    free(b.sum);
    if(data!=MAP_FAILED) munmap(data,(size_t)st.st_size);
    if(fd>=0) close(fd);
    if(rc<0 && err && errlen) snprintf(err,errlen,"%s",why);
    return rc;
}
