// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Streaming modern image readers. Large raster allocation is limited to GIF
 * compositing and interlaced PNG (64 MiB); ordinary PNG/JPEG use scanlines,
 * HEIC one row of tiles. */
#include "img_decode.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <limits.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <jpeglib.h>
#include <png.h>
#include <gif_lib.h>
#include <webp/decode.h>
#define LIMIT (64u*1024u*1024u)
static int fail(char *e,size_t n,const char *s){if(n)snprintf(e,n,"%s",s);return -1;}
static int dims(int w,int h,int c){return w>0&&h>0&&w<=1000000&&h<=1000000&&(uint64_t)w*h*c<=LIMIT;}
/* Integer overlap weights give exact box/area resampling, including fractional
 * edges. Only one destination row of accumulation is held. The picture is
 * never enlarged (img_size), so a source dot reaches at most two destination
 * columns: those and their weights are worked out once per column in
 * box_init (no 64-bit division per dot, slow on the Pomera's ARMv7). A row
 * is first summed across in integers (row: at most 255 * sw a column), then
 * added with its vertical weight; every sum stays an exact integer, as when
 * each dot was weighted in double. The tables live in the same block as sum
 * (callers free(b.sum)). */
typedef struct {int ox,w0,w1;} BoxCol;
typedef struct {int sw,sh,dw,dh; Img *im; double *sum; BoxCol *col; uint32_t *row;} Box;
static int box_init(Box *b,int sw,int sh,int mw,int mh,Img *im){
 int w,h;img_size(sw,sh,mw,mh,&w,&h);if(!dims(w,h,3)||img_alloc(im,w,h))return -1;
 memset(b,0,sizeof(*b));b->sw=sw;b->sh=sh;b->dw=w;b->dh=h;b->im=im;
 b->sum=calloc(1,(size_t)w*3*sizeof(double)+(size_t)sw*sizeof(BoxCol)+((size_t)w+1)*3*sizeof(uint32_t));if(!b->sum)return -1;
 b->col=(BoxCol*)(b->sum+(size_t)w*3);b->row=(uint32_t*)(b->col+sw);
 for(int x=0;x<sw;x++){int64_t l=(int64_t)x*w,r=l+w;int ox=(int)(l/sw);int64_t z=(int64_t)(ox+1)*sw;
  b->col[x].ox=ox;b->col[x].w0=(int)((r<z?r:z)-l);b->col[x].w1=w-b->col[x].w0;}
 return 0;
}
static void box_row(Box *b,const unsigned char *row,int y){
 uint32_t *h=b->row;memset(h,0,((size_t)b->dw+1)*3*sizeof(*h));
 for(int x=0;x<b->sw;x++){
  const BoxCol *k=b->col+x;const unsigned char *p=row+x*3;uint32_t *d=h+k->ox*3,w0=(uint32_t)k->w0,w1=(uint32_t)k->w1;
  d[0]+=p[0]*w0;d[1]+=p[1]*w0;d[2]+=p[2]*w0;d[3]+=p[0]*w1;d[4]+=p[1]*w1;d[5]+=p[2]*w1;
 }
 int ya=(int)((int64_t)y*b->dh/b->sh),yb=(int)(((int64_t)(y+1)*b->dh-1)/b->sh);
 for(int oy=ya;oy<=yb;oy++){
  int64_t yl=(int64_t)y*b->dh,yr=(int64_t)(y+1)*b->dh;
  int64_t a=(int64_t)oy*b->sh,z=(int64_t)(oy+1)*b->sh;
  double wy=(double)((yr<z?yr:z)-(yl>a?yl:a));
  for(int x=0;x<b->dw*3;x++)b->sum[x]+=h[x]*wy;
  if(yr>=z){double area=(double)b->sw*b->sh;unsigned char *dst=b->im->pixels+(size_t)oy*b->dw*3;
   for(int x=0;x<b->dw*3;x++){dst[x]=(unsigned char)(b->sum[x]/area+.5);b->sum[x]=0;}}
 }
}
static void metadata(Img *im,int w,int h,unsigned colors,const char *f){im->original_width=w;im->original_height=h;im->colors=colors;im->format=f;}
static unsigned u16(const unsigned char *p,int le){return le?p[0]|p[1]<<8:p[0]<<8|p[1];}
static uint32_t u32(const unsigned char*p,int le){return le?(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24:(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int exif(const unsigned char *p,size_t n){
 if(n<14||memcmp(p,"Exif\0\0",6))return 1;
 p+=6;n-=6;
 int le=p[0]=='I'&&p[1]=='I';
 if(!le&&!(p[0]=='M'&&p[1]=='M'))return 1;
 if(u16(p+2,le)!=42)return 1;
 uint32_t off=u32(p+4,le);
 if(off>n-2)return 1;
 unsigned count=u16(p+off,le);off+=2;
 for(unsigned i=0;i<count&&off<=n&&n-off>=12;i++,off+=12)
  if(u16(p+off,le)==0x112&&u16(p+off+2,le)==3&&u32(p+off+4,le)==1){int o=u16(p+off+8,le);return o>=1&&o<=8?o:1;}
 return 1;
}
static int orient(Img *im,int o){
 if(o==1)return 0;
 int w=im->width,h=im->height,nw=o>=5?h:w,nh=o>=5?w:h;
 unsigned char *p=malloc((size_t)w*h*3);if(!p)return -1;
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){int xx=x,yy=y;
 switch(o){case 2:xx=w-1-x;break;case 3:xx=w-1-x;yy=h-1-y;break;case 4:yy=h-1-y;break;
 case 5:xx=y;yy=x;break;case 6:xx=h-1-y;yy=x;break;case 7:xx=h-1-y;yy=w-1-x;break;case 8:xx=y;yy=w-1-x;break;}
 memcpy(p+((size_t)yy*nw+xx)*3,im->pixels+((size_t)y*w+x)*3,3);}
 free(im->pixels);im->pixels=p;im->width=nw;im->height=nh;return 0;
}
typedef struct {struct jpeg_error_mgr pub;jmp_buf jump;char message[JMSG_LENGTH_MAX];} JErr;
static void jerror(j_common_ptr c){JErr *e=(JErr*)c->err;(*c->err->format_message)(c,e->message);longjmp(e->jump,1);}
static void jmessage(j_common_ptr c,int level){if(level<0)jerror(c);}
typedef struct {struct jpeg_decompress_struct c;JErr e;FILE *f;Box b;unsigned char *row,*rgb;int created,mw,mh;} JState;
static int jpeg_load(const char *path,int mw,int mh,Img *im,char *err,size_t en){
 JState *s=calloc(1,sizeof(*s));if(!s)return fail(err,en,T("JPEG: メモリが足りません"));volatile int result=-1;
 /* Nothing changed after setjmp lives outside s (longjmp may clobber locals). */
 s->mw=mw;s->mh=mh;
 s->c.err=jpeg_std_error(&s->e.pub);s->e.pub.error_exit=jerror;s->e.pub.emit_message=jmessage;
 if(setjmp(s->e.jump)){fail(err,en,T("JPEG: 壊れています"));goto done;}
 if(!(s->f=fopen(path,"rb"))){fail(err,en,T("JPEG: 開けません"));goto done;}
 jpeg_create_decompress(&s->c);s->created=1;jpeg_stdio_src(&s->c,s->f);jpeg_save_markers(&s->c,JPEG_APP0+1,65535);jpeg_read_header(&s->c,TRUE);
 int o=1;for(jpeg_saved_marker_ptr m=s->c.marker_list;m;m=m->next)if(m->marker==JPEG_APP0+1&&m->data_length>=6&&!memcmp(m->data,"Exif\0\0",6))o=exif(m->data,m->data_length);
 int w=(int)s->c.image_width,h=(int)s->c.image_height,tw,th;if(w<=0||h<=0){fail(err,en,T("JPEG: 壊れています"));goto done;}
 if(s->c.progressive_mode&&(uint64_t)w*h>24000000){fail(err,en,T("JPEG: 大きすぎます"));goto done;}
 if(o>=5){s->mw=mh;s->mh=mw;}
 img_size(w,h,s->mw,s->mh,&tw,&th);
 s->c.scale_num=1;s->c.scale_denom=1;
 while(s->c.scale_denom<8&&(w/(int)(s->c.scale_denom*2)>=tw)&&(h/(int)(s->c.scale_denom*2)>=th))s->c.scale_denom*=2;
 int cmyk=s->c.jpeg_color_space==JCS_CMYK||s->c.jpeg_color_space==JCS_YCCK;
 s->c.out_color_space=cmyk?JCS_CMYK:JCS_RGB;jpeg_start_decompress(&s->c);
 if(s->c.output_components!=(cmyk?4:3)||s->c.output_width>1000000||s->c.output_height>1000000||box_init(&s->b,s->c.output_width,s->c.output_height,s->mw,s->mh,im)){fail(err,en,T("JPEG: 大きすぎます"));goto done;}
 s->row=malloc((size_t)s->c.output_width*s->c.output_components);if(cmyk)s->rgb=malloc((size_t)s->c.output_width*3);if(!s->row||(cmyk&&!s->rgb)){fail(err,en,T("JPEG: メモリが足りません"));goto done;}
 while(s->c.output_scanline<s->c.output_height){int y=s->c.output_scanline;JSAMPROW r=s->row;jpeg_read_scanlines(&s->c,&r,1);
 if(cmyk){for(unsigned x=0;x<s->c.output_width;x++){unsigned k=s->row[x*4+3];for(int c=0;c<3;c++){unsigned v=s->row[x*4+c];s->rgb[x*3+c]=s->c.saw_Adobe_marker?(v*k+127)/255:((255-v)*(255-k)+127)/255;}}}box_row(&s->b,cmyk?s->rgb:s->row,y);}
 jpeg_finish_decompress(&s->c);if(orient(im,o)){fail(err,en,T("JPEG: メモリが足りません"));goto done;}metadata(im,o>=5?h:w,o>=5?w:h,16777216,"JPEG");result=0;
 done:if(s->created)jpeg_destroy_decompress(&s->c);if(s->f)fclose(s->f);free(s->row);free(s->rgb);free(s->b.sum);free(s);if(result)img_free(im);return result;
}
typedef struct {png_structp p;png_infop info;FILE *f;Box b;unsigned char *row,*rgb,*all;} PState;
static int png_load(const char *path,int mw,int mh,Img *im,char *err,size_t en){
 PState *s=calloc(1,sizeof(*s));if(!s)return fail(err,en,T("PNG: メモリが足りません"));int result=-1;
 s->f=fopen(path,"rb");s->p=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);if(s->p)s->info=png_create_info_struct(s->p);
 if(!s->f||!s->p||!s->info){fail(err,en,T("PNG: 開けません"));goto done;}
 if(setjmp(png_jmpbuf(s->p))){fail(err,en,T("PNG: 壊れています"));goto done;}
 png_init_io(s->p,s->f);png_set_user_limits(s->p,1000000,1000000);png_set_chunk_malloc_max(s->p,LIMIT);png_set_chunk_cache_max(s->p,128);png_read_info(s->p,s->info);
 int w=png_get_image_width(s->p,s->info),h=png_get_image_height(s->p,s->info),ct=png_get_color_type(s->p,s->info),depth=png_get_bit_depth(s->p,s->info);unsigned colors=ct==PNG_COLOR_TYPE_PALETTE?(1u<<depth):16777216;
 if(depth==16)png_set_strip_16(s->p);
 if(ct==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(s->p);
 if(ct==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(s->p);
 if(png_get_valid(s->p,s->info,PNG_INFO_tRNS))png_set_tRNS_to_alpha(s->p);
 if(ct==PNG_COLOR_TYPE_GRAY||ct==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(s->p);
 if(!(ct&PNG_COLOR_MASK_ALPHA)&&!png_get_valid(s->p,s->info,PNG_INFO_tRNS))png_set_add_alpha(s->p,255,PNG_FILLER_AFTER);
 int passes=png_set_interlace_handling(s->p);png_read_update_info(s->p,s->info);size_t stride=png_get_rowbytes(s->p,s->info);
 if(stride!=(size_t)w*4||box_init(&s->b,w,h,mw,mh,im)){fail(err,en,T("PNG: 大きすぎます"));goto done;}
 s->rgb=malloc((size_t)w*3);s->row=malloc(stride);if(!s->rgb||!s->row){fail(err,en,T("PNG: メモリが足りません"));goto done;}
 if(passes>1){if(!dims(w,h,4)){fail(err,en,T("PNG: 大きすぎます"));goto done;}s->all=calloc((size_t)h,stride);if(!s->all){fail(err,en,T("PNG: メモリが足りません"));goto done;}}
 for(int pass=0;pass<passes;pass++)for(int y=0;y<h;y++){
 unsigned char *r=s->all?s->all+(size_t)y*stride:s->row;png_read_row(s->p,r,NULL);
 if(pass==passes-1){for(int x=0;x<w;x++){unsigned a=r[x*4+3];for(int c=0;c<3;c++)s->rgb[x*3+c]=(r[x*4+c]*a+(c==2?85:0)*(255-a)+127)/255;}box_row(&s->b,s->rgb,y);}}
 png_read_end(s->p,NULL);metadata(im,w,h,colors,"PNG");result=0;
 done:if(s->p)png_destroy_read_struct(&s->p,s->info?&s->info:NULL,NULL);if(s->f)fclose(s->f);free(s->row);free(s->rgb);free(s->all);free(s->b.sum);free(s);if(result)img_free(im);return result;
}
static int webp_load(const char *path,int mw,int mh,Img *im,char *err,size_t en){
 FILE *f=fopen(path,"rb");if(!f)return fail(err,en,T("WebP: 開けません"));
 unsigned char chunk[16384];size_t n=fread(chunk,1,sizeof(chunk),f);WebPDecoderConfig cfg;
 if(!WebPInitDecoderConfig(&cfg)){fclose(f);return fail(err,en,T("WebP: ライブラリの版が合いません"));}
 VP8StatusCode st=WebPGetFeatures(chunk,n,&cfg.input);
 if(st!=VP8_STATUS_OK||cfg.input.has_animation){fclose(f);return fail(err,en,cfg.input.has_animation?T("WebP: 動く WebP は未対応です"):T("WebP: 壊れています"));}
 int w,h;img_size(cfg.input.width,cfg.input.height,mw,mh,&w,&h);
 if(!dims(w,h,4)){fclose(f);return fail(err,en,T("WebP: 大きすぎます"));}
 cfg.options.use_scaling=1;cfg.options.scaled_width=w;cfg.options.scaled_height=h;cfg.output.colorspace=MODE_RGBA;
 WebPIDecoder *dec=WebPIDecode(NULL,0,&cfg);if(!dec){fclose(f);return fail(err,en,T("WebP: メモリが足りません"));}
 size_t total=0;do {total+=n;if(total>LIMIT){st=VP8_STATUS_OUT_OF_MEMORY;break;}st=WebPIAppend(dec,chunk,n);if(st!=VP8_STATUS_SUSPENDED)break;n=fread(chunk,1,sizeof(chunk),f);}while(n);
 int result=-1;
 if(st!=VP8_STATUS_OK)fail(err,en,st==VP8_STATUS_OUT_OF_MEMORY?T("WebP: 大きすぎます"):T("WebP: 壊れています"));
 else if(img_alloc(im,w,h))fail(err,en,T("WebP: メモリが足りません"));
 else {for(int y=0;y<h;y++){unsigned char *r=cfg.output.u.RGBA.rgba+(size_t)y*cfg.output.u.RGBA.stride,*d=im->pixels+(size_t)y*w*3;for(int x=0;x<w;x++){unsigned a=r[4*x+3];for(int c=0;c<3;c++)d[3*x+c]=(r[4*x+c]*a+(c==2?85:0)*(255-a)+127)/255;}}
 metadata(im,cfg.input.width,cfg.input.height,16777216,"WebP");result=0;}
 WebPIDelete(dec);WebPFreeDecBuffer(&cfg.output);fclose(f);if(result)img_free(im);return result;
}
static void gif_clear(unsigned char *canvas,int sw,int x,int y,int w,int h,const unsigned char bg[3]){
 for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)memcpy(canvas+((size_t)yy*sw+xx)*3,bg,3);
}
/* First pass skips compressed blocks to find duration. Second pass replays only
 * through the requested frame. No array of decoded animation frames is kept. */
static int gif_pass(const char *path,double target,int mw,int mh,Img *im,double *duration,int *loops,unsigned *count,double *until,char *err,size_t en){
 int ec=0,result=-1;GifFileType *g=DGifOpenFileName(path,&ec);unsigned char *canvas=NULL,*saved=NULL,*row=NULL;Box b={0};
 if(!g)return fail(err,en,T("GIF: 開けません"));
 int sw=g->SWidth,sh=g->SHeight;
 if(!dims(sw,sh,6)){fail(err,en,T("GIF: 大きすぎます"));goto done;}
 if(im){canvas=malloc((size_t)sw*sh*3);row=malloc((size_t)sw);if(!canvas||!row){fail(err,en,T("GIF: メモリが足りません"));goto done;}unsigned char blue[]={0,0,85};gif_clear(canvas,sw,0,0,sw,sh,blue);}
 GraphicsControlBlock gc={0,0,0,NO_TRANSPARENT_COLOR};double t=0;unsigned frames=0;uint64_t work=0;int shown=0;
 for(;;){GifRecordType rec;if(DGifGetRecordType(g,&rec)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}
 if(rec==TERMINATE_RECORD_TYPE)break;
 if(rec==EXTENSION_RECORD_TYPE){int code;GifByteType *ext;if(DGifGetExtension(g,&code,&ext)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}
  if(code==GRAPHICS_EXT_FUNC_CODE&&ext&&ext[0]==4)DGifExtensionToGCB(4,ext+1,&gc);
  int app=code==APPLICATION_EXT_FUNC_CODE&&ext&&ext[0]==11&&(!memcmp(ext+1,"NETSCAPE2.0",11)||!memcmp(ext+1,"ANIMEXTS1.0",11));
  while(ext){if(DGifGetExtensionNext(g,&ext)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}if(app&&loops&&ext&&ext[0]==3&&ext[1]==1)*loops=ext[2]|ext[3]<<8;}continue;}
 if(rec!=IMAGE_DESC_RECORD_TYPE)continue;
 if(DGifGetImageDesc(g)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}
 GifImageDesc *d=&g->Image;int x=d->Left,y=d->Top,w=d->Width,h=d->Height;
 if(++frames>100000||w<=0||h<=0||x<0||y<0||x>sw-w||y>sh-h){fail(err,en,T("GIF: 大きすぎます"));goto done;}
 double delay=gc.DelayTime>0?gc.DelayTime*.01:.1;
 if(!im){int code;GifByteType *block;if(DGifGetCode(g,&code,&block)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}while(block)if(DGifGetCodeNext(g,&block)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}t+=delay;memset(&gc,0,sizeof(gc));gc.TransparentColor=NO_TRANSPARENT_COLOR;continue;}
 work+=(uint64_t)w*h;if(work>256u*1024u*1024u){fail(err,en,T("GIF: 大きすぎます"));goto done;}
 if(frames==1&&gc.TransparentColor==NO_TRANSPARENT_COLOR&&g->SColorMap&&g->SBackGroundColor<g->SColorMap->ColorCount){GifColorType c=g->SColorMap->Colors[g->SBackGroundColor];unsigned char bg[]={c.Red,c.Green,c.Blue};gif_clear(canvas,sw,0,0,sw,sh,bg);}
 ColorMapObject *cm=d->ColorMap?d->ColorMap:g->SColorMap;if(!cm){fail(err,en,T("GIF: 壊れています"));goto done;}
 if(gc.DisposalMode==DISPOSE_PREVIOUS){if(!saved)saved=malloc((size_t)sw*sh*3);if(!saved){fail(err,en,T("GIF: メモリが足りません"));goto done;}memcpy(saved,canvas,(size_t)sw*sh*3);}
 int passes=d->Interlace?4:1;static const int start[]={0,4,2,1},step[]={8,8,4,2};
 for(int pass=0;pass<passes;pass++)for(int yy=d->Interlace?start[pass]:0;yy<h;yy+=d->Interlace?step[pass]:1){
 if(DGifGetLine(g,row,w)==GIF_ERROR){fail(err,en,T("GIF: 壊れています"));goto done;}
 for(int xx=0;xx<w;xx++){int ix=row[xx];if(ix==gc.TransparentColor)continue;if(ix>=cm->ColorCount){fail(err,en,T("GIF: 壊れています"));goto done;}GifColorType c=cm->Colors[ix];unsigned char *p=canvas+((size_t)(y+yy)*sw+x+xx)*3;p[0]=c.Red;p[1]=c.Green;p[2]=c.Blue;}}
 shown=1;
 if(target<t+delay){if(box_init(&b,sw,sh,mw,mh,im)){fail(err,en,T("GIF: 大きすぎます"));goto done;}for(int yy=0;yy<sh;yy++)box_row(&b,canvas+(size_t)yy*sw*3,yy);metadata(im,sw,sh,cm->ColorCount,"GIF");if(until)*until=t+delay;result=0;goto done;}
 if(gc.DisposalMode==DISPOSE_BACKGROUND){unsigned char bg[]={0,0,85};if(gc.TransparentColor==NO_TRANSPARENT_COLOR&&g->SColorMap&&g->SBackGroundColor<g->SColorMap->ColorCount){GifColorType c=g->SColorMap->Colors[g->SBackGroundColor];bg[0]=c.Red;bg[1]=c.Green;bg[2]=c.Blue;}gif_clear(canvas,sw,x,y,w,h,bg);}
 else if(gc.DisposalMode==DISPOSE_PREVIOUS)memcpy(canvas,saved,(size_t)sw*sh*3);
 t+=delay;memset(&gc,0,sizeof(gc));gc.TransparentColor=NO_TRANSPARENT_COLOR;
 }
 if(!im&&frames){*duration=t;if(count)*count=frames;result=0;}else fail(err,en,shown?T("GIF: 壊れています"):T("GIF: 壊れています"));
 done:free(canvas);free(saved);free(row);free(b.sum);DGifCloseFile(g,&ec);if(result&&im)img_free(im);return result;
}
int img_gif_load(const char *path,int maxw,int maxh,double seconds,Img *out,char *err,size_t errlen){
 double duration=0,until=0;int loops=-1;unsigned frames=0;
 if(gif_pass(path,0,maxw,maxh,NULL,&duration,&loops,&frames,NULL,err,errlen))return -1;
 if(!isfinite(seconds)||seconds<0)seconds=0;
 /* NETSCAPE2.0 count 0 loops forever, n plays n+1 times; without the block
  * the animation plays once and stays on the last frame. */
 double base=0,local=seconds;int last=0;
 if(loops==0||(loops>0&&seconds<duration*(loops+1))){local=fmod(seconds,duration);base=seconds-local;}
 else if(seconds>=duration){local=duration-1e-7;last=1;}
 if(gif_pass(path,local,maxw,maxh,out,NULL,NULL,NULL,&until,err,errlen))return -1;
 out->frames=frames;out->frame_end=frames<2||last||(loops!=0&&base+until>=duration*(loops>0?loops+1:1))?INFINITY:base+until;
 return 0;
}
/* HEIC/HEIF (iPhone photos). libheif is read with dlopen when the first one
 * is opened, not linked: FDTP builds without libheif-dev, and once libheif1
 * is installed (apt, any time later) HEIC opens without rebuilding.
 * FDTP_LIBHEIF replaces the library's name (the tests use a missing one).
 * Only the few functions below are used, declared here with the layouts of
 * libheif's API (heif_error, heif_image_tiling, heif_decoding_options).
 * An iPhone photo is a grid of HEVC tiles: each row of tiles is decoded and
 * reduced through Box, so a 24-megapixel picture never sits in memory whole
 * (the Pomera has 1 GiB; 54 MiB at most here, 119 MiB decoding it whole).
 * The tiles of a row are decoded on every core, each thread with a context
 * of its own (one context decodes one tile at a time: 13 s for that photo
 * on the Pomera, 4 s on its 4 cores), while the row before is reduced.
 * irot/imir/clap are then applied to the reduced picture. A picture that is
 * not a grid, and any with libheif older than 1.19 (no tile API), is
 * decoded whole with libheif's own transformations. */
#ifdef __APPLE__
#define HEIF_LIBS {"/opt/homebrew/opt/libheif/lib/libheif.1.dylib","/usr/local/opt/libheif/lib/libheif.1.dylib","libheif.1.dylib"}
#define HEIF_INSTALL "brew install libheif"
#define HEIF_PLUGIN_INSTALL "brew install libheif"
#else
#define HEIF_LIBS {"libheif.so.1"}
#define HEIF_INSTALL "sudo apt install libheif1 libheif-plugin-libde265"
#define HEIF_PLUGIN_INSTALL "sudo apt install libheif-plugin-libde265"
#endif
typedef struct {int code,subcode;const char *message;} HErr;
typedef struct {int version;uint32_t columns,rows,tw,th,w,h,top,left;uint8_t extra;uint32_t extra_size[8];} HTiling;
enum {H_RGB=1,H_RGB24=10,H_INTERLEAVED=10,H_HEVC=1,H_UNSUPPORTED=4,H_NO_MEMORY=6,H_NO_CODEC=3000};
#define FOURCC(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))
static struct {
 void *lib;
 HErr (*init)(void*);
 void *(*ctx_alloc)(void);void (*ctx_free)(void*);
 HErr (*read_file)(void*,const char*,const void*);
 HErr (*primary)(void*,void**);
 void (*handle_release)(const void*);
 int (*have_decoder)(int);
 HErr (*decode)(const void*,void**,int,int,const void*);
 void (*image_release)(const void*);
 const uint8_t *(*plane)(const void*,int,int*);
 int (*img_w)(const void*,int),(*img_h)(const void*,int);
 /* 1.19 and later: tiles and the transformative properties. */
 uint32_t (*item_id)(const void*);
 void *(*opts_alloc)(void);void (*opts_free)(void*);
 HErr (*tiling)(const void*,int,HTiling*);
 HErr (*tile)(const void*,void**,int,int,const void*,uint32_t,uint32_t);
 int (*tprops)(const void*,uint32_t,uint32_t*,int);
 uint32_t (*ptype)(const void*,uint32_t,uint32_t);
 int (*mirror)(const void*,uint32_t,uint32_t),(*rotation)(const void*,uint32_t,uint32_t);
 void (*crop)(const void*,uint32_t,uint32_t,int,int,int*,int*,int*,int*);
} H;
static void sym(void **fn,const char *name){*fn=dlsym(H.lib,name);}
/* 0 when libheif is there. Tried again on every open until it is found. */
static int heif_open(void){
 if(H.lib)return 0;
 const char *env=getenv("FDTP_LIBHEIF"),*names[]=HEIF_LIBS;
 if(env&&*env)H.lib=dlopen(env,RTLD_NOW|RTLD_LOCAL);
 else for(size_t i=0;i<sizeof(names)/sizeof(*names)&&!H.lib;i++)H.lib=dlopen(names[i],RTLD_NOW|RTLD_LOCAL);
 if(!H.lib)return -1;
 sym((void**)&H.init,"heif_init");sym((void**)&H.ctx_alloc,"heif_context_alloc");sym((void**)&H.ctx_free,"heif_context_free");
 sym((void**)&H.read_file,"heif_context_read_from_file");sym((void**)&H.primary,"heif_context_get_primary_image_handle");
 sym((void**)&H.handle_release,"heif_image_handle_release");sym((void**)&H.have_decoder,"heif_have_decoder_for_format");
 sym((void**)&H.decode,"heif_decode_image");sym((void**)&H.image_release,"heif_image_release");
 sym((void**)&H.plane,"heif_image_get_plane_readonly");sym((void**)&H.img_w,"heif_image_get_width");sym((void**)&H.img_h,"heif_image_get_height");
 sym((void**)&H.item_id,"heif_image_handle_get_item_id");sym((void**)&H.opts_alloc,"heif_decoding_options_alloc");sym((void**)&H.opts_free,"heif_decoding_options_free");
 sym((void**)&H.tiling,"heif_image_handle_get_image_tiling");sym((void**)&H.tile,"heif_image_handle_decode_image_tile");
 sym((void**)&H.tprops,"heif_item_get_transformation_properties");sym((void**)&H.ptype,"heif_item_get_property_type");
 sym((void**)&H.mirror,"heif_item_get_property_transform_mirror");sym((void**)&H.rotation,"heif_item_get_property_transform_rotation_ccw");
 sym((void**)&H.crop,"heif_item_get_property_transform_crop_borders");
 if(!H.ctx_alloc||!H.ctx_free||!H.read_file||!H.primary||!H.handle_release||!H.decode||!H.image_release||!H.plane||!H.img_w||!H.img_h){dlclose(H.lib);memset(&H,0,sizeof(H));return -1;}
 /* heif_init loads the decoder plugins (libheif-plugin-libde265). It is
  * never balanced by heif_deinit: the library stays for FDTP's lifetime. */
 if(H.init)H.init(NULL);
 return 0;
}
static int heif_tiled(void){return H.item_id&&H.opts_alloc&&H.opts_free&&H.tiling&&H.tile&&H.tprops&&H.ptype&&H.mirror&&H.rotation&&H.crop;}
static int heif_fail(HErr e,char *err,size_t en){
 if(e.code==H_UNSUPPORTED&&e.subcode==H_NO_CODEC){if(en)snprintf(err,en,T("HEIC: 解く部品がありません。次で入れると開けます:\n%s"),HEIF_PLUGIN_INSTALL);return -1;}
 return fail(err,en,e.code==H_NO_MEMORY?T("HEIC: メモリが足りません"):T("HEIC: 壊れています"));
}
/* Cuts borders from the reduced picture. */
static int crop_img(Img *im,int l,int t,int r,int b){
 int w=im->width-l-r,h=im->height-t-b;if(w<1||h<1)return -1;
 for(int y=0;y<h;y++)memmove(im->pixels+(size_t)y*w*3,im->pixels+((size_t)(y+t)*im->width+l)*3,(size_t)w*3);
 im->width=w;im->height=h;return 0;
}
static int heif_whole(void *h,int mw,int mh,Img *im,char *err,size_t en){
 void *img=NULL;HErr e=H.decode(h,&img,H_RGB,H_RGB24,NULL);if(e.code)return heif_fail(e,err,en);
 int stride=0,result=-1;const uint8_t *p=H.plane(img,H_INTERLEAVED,&stride);int w=H.img_w(img,H_INTERLEAVED),hh=H.img_h(img,H_INTERLEAVED);
 Box b;memset(&b,0,sizeof(b));
 if(!p||w<1||hh<1||stride<w*3)fail(err,en,T("HEIC: 壊れています"));
 else if(box_init(&b,w,hh,mw,mh,im))fail(err,en,T("HEIC: 大きすぎます"));
 else{for(int y=0;y<hh;y++)box_row(&b,p+(size_t)y*stride,y);metadata(im,w,hh,16777216,"HEIC");result=0;}
 free(b.sum);H.image_release(img);return result;
}
/* One row of tiles into strip: the threads take its columns in turn. */
typedef struct {const HTiling *t;unsigned char *strip;const void *opts;int W,row,lines,next;pthread_mutex_t mu;} HeifRow;
typedef struct {HeifRow *job;void *ctx,*h;HErr e;int bad;} HeifWorker;
static void *heif_worker(void *arg){
 HeifWorker *w=arg;HeifRow *j=w->job;int tw=(int)j->t->tw;
 while(!w->e.code&&!w->bad){
  pthread_mutex_lock(&j->mu);int col=j->next++;pthread_mutex_unlock(&j->mu);
  if(col>=(int)j->t->columns||col*tw>=j->W)break;
  void *img=NULL;w->e=H.tile(w->h,&img,H_RGB,H_RGB24,j->opts,(uint32_t)col,(uint32_t)j->row);if(w->e.code)break;
  int stride=0;const uint8_t *p=H.plane(img,H_INTERLEAVED,&stride);int iw=H.img_w(img,H_INTERLEAVED),ih=H.img_h(img,H_INTERLEAVED);
  int cols=j->W-col*tw<tw?j->W-col*tw:tw;
  if(!p||iw<cols||ih<j->lines||stride<iw*3)w->bad=1;
  else for(int y=0;y<j->lines;y++)memcpy(j->strip+((size_t)y*j->W+(size_t)col*tw)*3,p+(size_t)y*stride,(size_t)cols*3);
  H.image_release(img);
 }
 return NULL;
}
static int heif_by_tiles(const char *path,void *ctx,void *h,int mw,int mh,Img *im,char *err,size_t en){
 HTiling t;memset(&t,0,sizeof(t));
 HErr e=H.tiling(h,0,&t);if(e.code)return heif_fail(e,err,en);
 if(!t.columns||!t.rows||!t.tw||!t.th||t.top||t.left||t.w<1||t.h<1||t.w>1000000||t.h>1000000||t.tw>65536||t.th>65536)return fail(err,en,T("HEIC: 壊れています"));
 /* One coded picture (not a grid): libheif decodes it whole, as it would a tile. */
 if(t.columns==1&&t.rows==1)return heif_whole(h,mw,mh,im,err,en);
 int W=(int)t.w,Hh=(int)t.h,th=(int)t.th;
 /* The transformative properties, in order: the full size each one sees. */
 uint32_t id=H.item_id(h),props[8];int np=H.tprops(ctx,id,props,8);if(np<0)np=0;if(np>8)np=8;
 struct {int kind,a,l,t,r,b,fw,fh;} op[8];int fw=W,fh=Hh;
 for(int i=0;i<np;i++){uint32_t type=H.ptype(ctx,id,props[i]);op[i].kind=0;op[i].fw=fw;op[i].fh=fh;
  if(type==FOURCC('i','r','o','t')){op[i].kind='r';op[i].a=H.rotation(ctx,id,props[i]);if(op[i].a==90||op[i].a==270){int x=fw;fw=fh;fh=x;}}
  else if(type==FOURCC('i','m','i','r')){op[i].kind='m';op[i].a=H.mirror(ctx,id,props[i]);}
  else if(type==FOURCC('c','l','a','p')){op[i].kind='c';op[i].l=op[i].t=op[i].r=op[i].b=0;H.crop(ctx,id,props[i],fw,fh,&op[i].l,&op[i].t,&op[i].r,&op[i].b);
   if(op[i].l<0||op[i].t<0||op[i].r<0||op[i].b<0||op[i].l+op[i].r>=fw||op[i].t+op[i].b>=fh)return fail(err,en,T("HEIC: 壊れています"));
   fw-=op[i].l+op[i].r;fh-=op[i].t+op[i].b;}}
 /* The box as stored: the bounding box turned with irot; with a clap, the
  * scale the cropped picture fits by. */
 int bw=mw,bh=mh,crops=0,turns=0;
 for(int i=0;i<np;i++){crops|=op[i].kind=='c';turns+=op[i].kind=='r'&&(op[i].a==90||op[i].a==270);}
 if(crops){int ow,oh;img_size(fw,fh,mw,mh,&ow,&oh);double s=fmin(1.,fmax((double)ow/fw,(double)oh/fh));bw=(int)fmin(W,ceil(W*s));bh=(int)fmin(Hh,ceil(Hh*s));}
 else if(turns&1){bw=mh;bh=mw;}
 /* Two rows of tiles: one decoded while the one before is reduced. */
 int rows=(int)((Hh+th-1)/th)<(int)t.rows?(int)((Hh+th-1)/th):(int)t.rows,bufs=rows>1?2:1;
 if((uint64_t)W*th*3*bufs>LIMIT)return fail(err,en,T("HEIC: 大きすぎます"));
 Box b;memset(&b,0,sizeof(b));unsigned char *strip=malloc((size_t)W*th*3*bufs);void *opts=H.opts_alloc();int result=-1,n=1;
 HeifRow job;memset(&job,0,sizeof(job));pthread_mutex_init(&job.mu,NULL);HeifWorker wk[4];memset(wk,0,sizeof(wk));pthread_t tid[4];
 if(!strip||!opts){fail(err,en,T("HEIC: メモリが足りません"));goto done;}
 if(box_init(&b,W,Hh,bw,bh,im)){fail(err,en,T("HEIC: 大きすぎます"));goto done;}
 ((uint8_t*)opts)[1]=1; /* ignore_transformations: tiles as stored */
 /* Up to 4 threads (the Pomera's cores); the others read the file again. */
 long cpus=sysconf(_SC_NPROCESSORS_ONLN);n=cpus<1?1:cpus>4?4:(int)cpus;if(n>(int)t.columns)n=(int)t.columns;
 job.t=&t;job.opts=opts;job.W=W;
 wk[0].job=&job;wk[0].ctx=ctx;wk[0].h=h;
 for(int i=1;i<n;i++){
  wk[i].job=&job;wk[i].ctx=H.ctx_alloc();wk[i].h=NULL;
  if(!wk[i].ctx||H.read_file(wk[i].ctx,path,NULL).code||H.primary(wk[i].ctx,&wk[i].h).code){
   if(wk[i].h)H.handle_release(wk[i].h);
   if(wk[i].ctx)H.ctx_free(wk[i].ctx);
   n=i;break;}}
 for(int row=0;row<=rows;row++){
  int started=0;
  if(row<rows){
   job.row=row;job.lines=Hh-row*th<th?Hh-row*th:th;job.next=0;job.strip=strip+(size_t)(row%bufs)*W*th*3;
   for(int i=0;i<n;i++){wk[i].e.code=0;wk[i].bad=0;}
   for(;started<n;started++)if(pthread_create(&tid[started],NULL,heif_worker,&wk[started]))break;}
  if(row>0){int r=row-1,lines=Hh-r*th<th?Hh-r*th:th;const unsigned char *src=strip+(size_t)(r%bufs)*W*th*3;
   for(int y=0;y<lines;y++)box_row(&b,src+(size_t)y*W*3,r*th+y);}
  if(row==rows)break;
  for(int i=started;i<n;i++)heif_worker(&wk[i]); /* threads that did not start */
  for(int i=0;i<started;i++)pthread_join(tid[i],NULL);
  for(int i=0;i<n;i++){
   if(wk[i].e.code){heif_fail(wk[i].e,err,en);goto done;}
   if(wk[i].bad){fail(err,en,T("HEIC: 壊れています"));goto done;}}}
 for(int i=0;i<np;i++){
  if(op[i].kind=='r'&&op[i].a>0){if(orient(im,op[i].a==90?8:op[i].a==180?3:6)){fail(err,en,T("HEIC: メモリが足りません"));goto done;}}
  else if(op[i].kind=='m'&&op[i].a>=0){if(orient(im,op[i].a==1?2:4)){fail(err,en,T("HEIC: メモリが足りません"));goto done;}}
  else if(op[i].kind=='c'){double sx=(double)im->width/op[i].fw,sy=(double)im->height/op[i].fh;
   if(crop_img(im,(int)(op[i].l*sx+.5),(int)(op[i].t*sy+.5),(int)(op[i].r*sx+.5),(int)(op[i].b*sy+.5))){fail(err,en,T("HEIC: 壊れています"));goto done;}}}
 metadata(im,fw,fh,16777216,"HEIC");result=0;
 done:for(int i=1;i<n;i++){H.handle_release(wk[i].h);H.ctx_free(wk[i].ctx);}
 pthread_mutex_destroy(&job.mu);free(strip);free(b.sum);if(opts)H.opts_free(opts);return result;
}
static int heif_load(const char *path,int mw,int mh,Img *im,char *err,size_t en){
 if(heif_open()){if(en)snprintf(err,en,T("libheif が入っていません。次で入れると開けます:\n%s"),HEIF_INSTALL);return -1;}
 if(H.have_decoder&&!H.have_decoder(H_HEVC)){if(en)snprintf(err,en,T("HEIC: 解く部品がありません。次で入れると開けます:\n%s"),HEIF_PLUGIN_INSTALL);return -1;}
 void *ctx=H.ctx_alloc(),*h=NULL;int result=-1;
 if(!ctx)return fail(err,en,T("HEIC: メモリが足りません"));
 HErr e=H.read_file(ctx,path,NULL);
 if(!e.code)e=H.primary(ctx,&h);
 if(e.code)heif_fail(e,err,en);
 else result=heif_tiled()?heif_by_tiles(path,ctx,h,mw,mh,im,err,en):heif_whole(h,mw,mh,im,err,en);
 if(h)H.handle_release(h);
 H.ctx_free(ctx);if(result)img_free(im);return result;
}
/* ISO BMFF "ftyp" with a HEIF brand (major brand; mif1/msf1 files are
 * opened too and fail in libheif when their codec has no decoder). */
static int is_heif(const unsigned char *s,size_t n){
 static const char *brands[]={"heic","heix","heim","heis","hevc","hevx","mif1","msf1"};
 if(n<12||memcmp(s+4,"ftyp",4))return 0;
 for(size_t i=0;i<sizeof(brands)/sizeof(*brands);i++)if(!memcmp(s+8,brands[i],4))return 1;
 return 0;
}
int img_modern_load(const char *path,int maxw,int maxh,Img *out,char *err,size_t errlen){
 unsigned char sig[12]={0};FILE *f=fopen(path,"rb");if(!f)return fail(err,errlen,T("画像: 開けません"));size_t n=fread(sig,1,sizeof(sig),f);fclose(f);
 if(n>=2&&sig[0]==255&&sig[1]==216)return jpeg_load(path,maxw,maxh,out,err,errlen);
 if(n>=8&&!png_sig_cmp(sig,0,8))return png_load(path,maxw,maxh,out,err,errlen);
 if(n>=6&&(!memcmp(sig,"GIF87a",6)||!memcmp(sig,"GIF89a",6)))return img_gif_load(path,maxw,maxh,0,out,err,errlen);
 if(n>=12&&!memcmp(sig,"RIFF",4)&&!memcmp(sig+8,"WEBP",4))return webp_load(path,maxw,maxh,out,err,errlen);
 if(is_heif(sig,n))return heif_load(path,maxw,maxh,out,err,errlen);
 return fail(err,errlen,T("画像: 未知の形式です"));
}
