// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Native MAKI02 and Pi decoders. MAG:  The packed image is reconstructed in a 17-row
 * window; compressed streams and full-resolution RGB are never buffered.
 * All file offsets in a MAG header are relative to its zero marker. */
#include "img_decode.h"
#include "i18n.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct { FILE *f; uint64_t left; } LegacyStream;
typedef struct {
    Img *img;
    unsigned sw,sh,dw,dh,y,dy;
    uint64_t *horizontal,*vertical;
} LegacyScale;

static unsigned legacy_u16(const unsigned char *p)
{ return (unsigned)p[0] | (unsigned)p[1]<<8; }
static uint32_t legacy_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int legacy_byte(LegacyStream *s)
{
    int c;
    if(!s->left || (c=fgetc(s->f))==EOF) return -1;
    --s->left; return c;
}
static int legacy_stream(LegacyStream *s,const char *path,uint64_t start,uint64_t len)
{
    s->f=fopen(path,"rb"); s->left=len;
    if(!s->f) return -1;
    if(start>(uint64_t)LONG_MAX || fseek(s->f,(long)start,SEEK_SET)) {
        fclose(s->f); s->f=NULL; return -1;
    }
    return 0;
}
static int legacy_scale_init(LegacyScale *s,Img *im,unsigned w,unsigned h,int mw,int mh)
{
    int dw,dh;
    memset(s,0,sizeof(*s)); s->img=im;
    img_size((int)w,(int)h,mw,mh,&dw,&dh);
    if(img_alloc(im,dw,dh)) return -1;
    s->sw=w; s->sh=h; s->dw=(unsigned)dw; s->dh=(unsigned)dh;
    s->horizontal=calloc((size_t)dw*3,sizeof(uint64_t));
    s->vertical=calloc((size_t)dw*3,sizeof(uint64_t));
    if(!s->horizontal || !s->vertical) return -1;
    im->original_width=(int)w; im->original_height=(int)h;
    return 0;
}
/* Exact area weights in integer source/destination coordinate grids. */
static int legacy_scale_row(LegacyScale *s,const unsigned char *rgb)
{
    uint64_t top=(uint64_t)s->y*s->dh,bottom=top+s->dh;
    unsigned x;
    if(s->y>=s->sh) return -1;
    memset(s->horizontal,0,(size_t)s->dw*3*sizeof(uint64_t));
    for(x=0;x<s->sw;x++) {
        uint64_t a=(uint64_t)x*s->dw,b=a+s->dw;
        while(a<b) {
            unsigned dx=(unsigned)(a/s->sw);
            uint64_t end=(uint64_t)(dx+1)*s->sw,weight;
            if(end>b) end=b;
            weight=end-a;
            for(unsigned c=0;c<3;c++)
                s->horizontal[(size_t)dx*3+c]+=(uint64_t)rgb[(size_t)x*3+c]*weight;
            a=end;
        }
    }
    while(top<bottom) {
        uint64_t end=(uint64_t)(s->dy+1)*s->sh,weight;
        if(end>bottom) end=bottom;
        weight=end-top;
        for(size_t i=0;i<(size_t)s->dw*3;i++)
            s->vertical[i]+=s->horizontal[i]*weight;
        top=end;
        if(end==(uint64_t)(s->dy+1)*s->sh) {
            uint64_t divisor=(uint64_t)s->sw*s->sh;
            unsigned char *dest=s->img->pixels+(size_t)s->dy*s->dw*3;
            for(size_t i=0;i<(size_t)s->dw*3;i++) {
                dest[i]=(unsigned char)((s->vertical[i]+divisor/2)/divisor);
                s->vertical[i]=0;
            }
            ++s->dy;
        }
    }
    ++s->y; return 0;
}
static void legacy_scale_free(LegacyScale *s)
{ free(s->horizontal); free(s->vertical); }

static int legacy_mag(FILE *f,const char *path,int mw,int mh,Img *out)
{
    unsigned char h[32],palette[256][3];
    unsigned char *flags=NULL,*history=NULL,*rgb=NULL;
    LegacyStream fa={0},fb={0},px={0}; LegacyScale scale={0};
    long base,file_end;
    unsigned w,ht,left,padded,words,nflags,colors,pixels_per_word,repeat;
    uint32_t aoff,boff,blen,poff,plen;
    int rc=-1,c;
    /* A comment must terminate within 1 MiB; no unbounded malformed scan. */
    for(unsigned i=0;;i++) {
        if(i>=1048576 || (c=fgetc(f))==EOF) goto done;
        if(c==0x1a) break;
    }
    base=ftell(f);
    if(base<0 || fread(h,1,32,f)!=32 || h[0]!=0) goto done;
    if(fseek(f,0,SEEK_END) || (file_end=ftell(f))<0) goto done;
    if(legacy_u16(h+8)<legacy_u16(h+4) || legacy_u16(h+10)<legacy_u16(h+6)) goto done;
    w=legacy_u16(h+8)-legacy_u16(h+4)+1;
    ht=legacy_u16(h+10)-legacy_u16(h+6)+1;
    /* Palette size and vertical aspect are encoded in screen-mode bits. */
    colors=(h[3]&0x80)?256:16;
    /* Lines are whole flag bytes: 8 dots (16 colors) or 4 dots (256). */
    {
        unsigned align=colors==256?3u:7u;
        left=legacy_u16(h+4)&~align;
        padded=(legacy_u16(h+8)|align)+1-left;
    }
    pixels_per_word=colors==256?2:4;
    repeat=(h[3]&1)?2:1;
    if(w>16384 || ht>16384 || (uint64_t)w*ht>67108864) goto done;
    words=padded/pixels_per_word; nflags=words/2;
    aoff=legacy_u32(h+12); boff=legacy_u32(h+16);
    blen=legacy_u32(h+20); poff=legacy_u32(h+24); plen=legacy_u32(h+28);
    if(aoff<32+colors*3 || boff<aoff || (uint64_t)boff+blen>poff ||
       (uint64_t)poff+plen>(uint64_t)(file_end-base) ||
       (uint64_t)boff-aoff<((uint64_t)nflags*ht+7)/8) goto done;
    if(fseek(f,base+32,SEEK_SET)) goto done;
    for(unsigned i=0;i<colors;i++) {
        unsigned char grb[3];
        if(fread(grb,1,3,f)!=3) goto done;
        palette[i][0]=grb[1]; palette[i][1]=grb[0]; palette[i][2]=grb[2];
    }
    if(legacy_stream(&fa,path,(uint64_t)base+aoff,boff-aoff) ||
       legacy_stream(&fb,path,(uint64_t)base+boff,blen) ||
       legacy_stream(&px,path,(uint64_t)base+poff,plen)) goto done;
    flags=calloc(nflags,1);
    history=calloc((size_t)17*words,2);
    rgb=malloc((size_t)w*3);
    if(!flags || !history || !rgb || legacy_scale_init(&scale,out,w,ht*repeat,mw,mh)) goto done;
    out->format="MAG (MAKI02)"; out->colors=colors;
    unsigned mask=0,ab=0;
    static const unsigned dx[16]={0,1,2,4,0,1,0,1,2,0,1,2,0,1,2,0};
    static const unsigned dy[16]={0,0,0,0,1,1,2,2,2,4,4,4,8,8,8,16};
    for(unsigned y=0;y<ht;y++) {
        unsigned char *row=history+(size_t)(y%17)*words*2;
        for(unsigned j=0;j<nflags;j++) {
            if(!mask) { c=legacy_byte(&fa); if(c<0) goto done; ab=(unsigned)c; mask=128; }
            if(ab&mask) { c=legacy_byte(&fb); if(c<0) goto done; flags[j]^=(unsigned char)c; }
            mask>>=1;
            for(unsigned k=0;k<2;k++) {
                unsigned op=k?(flags[j]&15):(flags[j]>>4),x=j*2+k;
                unsigned char *dest=row+(size_t)x*2;
                if(!op) {
                    int a=legacy_byte(&px),b=legacy_byte(&px);
                    if(a<0 || b<0) goto done;
                    dest[0]=(unsigned char)a; dest[1]=(unsigned char)b;
                } else {
                    /* References may wrap to the preceding row through a
                     * negative x, but never before the first decoded word. */
                    int64_t source=(int64_t)y*words+x-(int64_t)dy[op]*words-dx[op];
                    int64_t current=(int64_t)y*words+x;
                    if(source<0 || source>=current) goto done;
                    unsigned sy=(unsigned)(source/words),sx=(unsigned)(source%words);
                    const unsigned char *p=history+((size_t)(sy%17)*words+sx)*2;
                    dest[0]=p[0]; dest[1]=p[1];
                }
            }
        }
        for(unsigned x=0;x<w;x++) {
            unsigned ix=x+legacy_u16(h+4)-left,index;
            index=colors==256?row[ix]:(row[ix/2]>>((ix&1)?0:4))&15;
            memcpy(rgb+(size_t)x*3,palette[index],3);
        }
        for(unsigned r=0;r<repeat;r++) if(legacy_scale_row(&scale,rgb)) goto done;
    }
    if(scale.dy!=scale.dh) goto done;
    rc=0;
done:
    if(fa.f) fclose(fa.f);
    if(fb.f) fclose(fb.f);
    if(px.f) fclose(px.f);
    free(flags); free(history); free(rgb); legacy_scale_free(&scale);
    return rc;
}

/* Pi (16 colors). A 4-line ring of palette indices holds everything a
 * position code can reach (at most 2 lines + 1 dot back, or 4 dots); finished
 * lines go straight to the area scaler. The bit stream is read MSB first. */
typedef struct { FILE *f; unsigned acc; int n, eof; } PiBits;
static int pi_bit(PiBits *b)
{
    if(!b->n) {
        int c=fgetc(b->f);
        if(c==EOF) { b->eof=1; return 0; }
        b->acc=(unsigned)c; b->n=8;
    }
    --b->n; return (int)((b->acc>>b->n)&1);
}
static uint64_t pi_bits(PiBits *b,int n)
{ uint64_t v=0; while(n-->0) v=v<<1|(uint64_t)pi_bit(b); return v; }
/* Color code 1x / 00x / 010xx / 011xxx selects a place in the move-to-front
 * list of the color to the left. */
static unsigned pi_color(PiBits *b,unsigned char table[16][16],unsigned left)
{
    unsigned i;
    if(pi_bit(b)) i=(unsigned)pi_bit(b);
    else if(!pi_bit(b)) i=2+(unsigned)pi_bit(b);
    else if(!pi_bit(b)) i=4+(unsigned)pi_bits(b,2);
    else i=8+(unsigned)pi_bits(b,3);
    unsigned char *t=table[left&15],c=t[i];
    memmove(t+1,t,i); t[0]=c; return c;
}
typedef struct {
    unsigned char *ring; uint64_t size,line_end,width;
    unsigned char (*palette)[3]; unsigned char *rgb;
    unsigned repeat; LegacyScale *scale; int failed;
} PiOut;
static void pi_put(PiOut *o,uint64_t p,unsigned c)
{
    o->ring[p%o->size]=(unsigned char)c;
    if(p+1!=o->line_end) return;
    for(uint64_t x=0;x<o->width;x++)
        memcpy(o->rgb+x*3,o->palette[o->ring[(o->line_end-o->width+x)%o->size]&15],3);
    for(unsigned r=0;r<o->repeat;r++) if(legacy_scale_row(o->scale,o->rgb)) o->failed=1;
    o->line_end+=o->width;
}
static int pi_skip_to(FILE *f,int end)
{
    for(unsigned i=0;i<1048576;i++) {
        int c=fgetc(f);
        if(c==EOF) return -1;
        if(c==end) return 0;
    }
    return -1;
}
static int legacy_pi(FILE *f,int mw,int mh,Img *out,char *err,size_t errlen)
{
    unsigned char h[10],palette[16][3],table[16][16];
    LegacyScale scale={0}; PiOut o={0};
    int rc=-1;
    /* Comment ends at 0x1a, then a dummy comment ends at 0x00. */
    if(pi_skip_to(f,0x1a) || pi_skip_to(f,0) || fread(h,1,10,f)!=10) goto done;
    unsigned mode=h[0],ax=h[1],ay=h[2],planes=h[3];
    unsigned ext=(unsigned)h[8]<<8|h[9];
    if(planes!=4) {
        if(errlen) snprintf(err,errlen,T("%u 色の Pi は未対応です"),planes<=8?1u<<planes:0u);
        goto done;
    }
    if(ext && fseek(f,(long)ext,SEEK_CUR)) goto done;
    unsigned char wh[4];
    if(fread(wh,1,4,f)!=4) goto done;
    unsigned w=(unsigned)wh[0]<<8|wh[1],ht=(unsigned)wh[2]<<8|wh[3];
    if(w<2 || ht<1 || (uint64_t)w*ht>67108864) goto done;
    if(mode&0x80) {
        /* No palette in the file: the PC-98 digital colors, bit 0 blue,
         * bit 1 red, bit 2 green, bit 3 bright. */
        for(unsigned i=0;i<16;i++) {
            unsigned char v=(i&8)?255:119;
            palette[i][0]=(i&2)?v:0; palette[i][1]=(i&4)?v:0; palette[i][2]=(i&1)?v:0;
        }
    } else if(fread(palette,1,48,f)!=48) goto done;
    for(unsigned i=0;i<16;i++) for(unsigned j=0;j<16;j++) table[i][j]=(unsigned char)((i-j)&15);
    o.repeat=(ax && ay && ay==2*ax)?2:1;
    o.width=w; o.size=(uint64_t)w*4+8; o.palette=palette; o.scale=&scale;
    o.ring=malloc((size_t)o.size); o.rgb=malloc((size_t)w*3);
    if(!o.ring || !o.rgb || legacy_scale_init(&scale,out,w,ht*o.repeat,mw,mh)) goto done;
    out->format="Pi"; out->colors=16;
    PiBits b={f,0,0,0};
    unsigned c1=pi_color(&b,table,0),c2=pi_color(&b,table,c1);
    /* Two virtual lines above the picture hold the first two dots. */
    uint64_t p,end=(uint64_t)w*2+(uint64_t)w*ht;
    for(p=0;p<(uint64_t)w*2;p++) o.ring[p%o.size]=(unsigned char)((p&1)?c2:c1);
    o.line_end=(uint64_t)w*3;
    int prev=-1;
    while(p<end && !b.eof && !o.failed) {
        int pos=(int)pi_bits(&b,2);
        if(pos==3) pos+=pi_bit(&b);
        if(pos==prev) {
            /* Colors read directly, two dots at a time, while a 1 follows. */
            do {
                unsigned c=pi_color(&b,table,o.ring[(p-1)%o.size]);
                pi_put(&o,p++,c);
                if(p<end) { c=pi_color(&b,table,c); pi_put(&o,p++,c); }
            } while(p<end && !b.eof && pi_bit(&b));
            prev=-1;
        } else {
            int n=0;
            while(pi_bit(&b)) if(++n>40 || b.eof) goto done;
            uint64_t len=((uint64_t)1<<n)+pi_bits(&b,n),d;
            switch(pos) {
            case 0: d=o.ring[(p-1)%o.size]==o.ring[(p-2)%o.size]?4:2; break;
            case 1: d=w; break;
            case 2: d=(uint64_t)w*2; break;
            case 3: d=w-1; break;
            default: d=(uint64_t)w+1; break;
            }
            for(uint64_t k=0;k<len*2 && p<end;k++,p++) pi_put(&o,p,o.ring[(p-d)%o.size]);
            prev=pos;
        }
    }
    if(p<end || o.failed || scale.dy!=scale.dh) goto done;
    rc=0;
done:
    free(o.ring); free(o.rgb); legacy_scale_free(&scale);
    return rc;
}

int img_legacy_load(const char *path,int maxw,int maxh,Img *out,char *err,size_t errlen)
{
    unsigned char magic[8];
    FILE *f=fopen(path,"rb"); int rc=-1,pi=0;
    if(!f) { if(errlen) snprintf(err,errlen,"%s", T("画像を開けません")); return -1; }
    size_t got=maxw>0 && maxh>0 ? fread(magic,1,8,f) : 0;
    if(got==8 && !memcmp(magic,"MAKI02  ",8))
        rc=legacy_mag(f,path,maxw,maxh,out);
    else if(got>=2 && magic[0]=='P' && magic[1]=='i' && !fseek(f,2,SEEK_SET)) {
        pi=1; rc=legacy_pi(f,maxw,maxh,out,err,errlen);
    }
    fclose(f);
    if(rc) {
        img_free(out);
        if(errlen && !err[0]) snprintf(err,errlen,pi?T("壊れた Pi 画像です"):T("壊れた MAG 画像です"));
    }
    return rc;
}
