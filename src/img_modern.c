// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Streaming modern image readers. Large raster allocation is limited to GIF
 * compositing and interlaced PNG (64 MiB); ordinary PNG/JPEG use scanlines. */
#include "img_decode.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <limits.h>
#include <math.h>
#include <jpeglib.h>
#include <png.h>
#include <gif_lib.h>
#include <webp/decode.h>
#define LIMIT (64u*1024u*1024u)
static int fail(char *e,size_t n,const char *s){if(n)snprintf(e,n,"%s",s);return -1;}
static int dims(int w,int h,int c){return w>0&&h>0&&w<=1000000&&h<=1000000&&(uint64_t)w*h*c<=LIMIT;}
/* Integer overlap weights give exact box/area resampling, including fractional
 * edges. Only one destination row of accumulation is held. */
typedef struct {int sw,sh,dw,dh; Img *im; double *sum;} Box;
static int box_init(Box *b,int sw,int sh,int mw,int mh,Img *im){
 int w,h;img_size(sw,sh,mw,mh,&w,&h);if(!dims(w,h,3)||img_alloc(im,w,h))return -1;
 memset(b,0,sizeof(*b));b->sw=sw;b->sh=sh;b->dw=w;b->dh=h;b->im=im;
 b->sum=calloc((size_t)w*3,sizeof(double));return b->sum?0:-1;
}
static void box_row(Box *b,const unsigned char *row,int y){
 int ya=(int)((int64_t)y*b->dh/b->sh),yb=(int)(((int64_t)(y+1)*b->dh-1)/b->sh);
 for(int oy=ya;oy<=yb;oy++){
  int64_t yl=(int64_t)y*b->dh,yr=(int64_t)(y+1)*b->dh;
  int64_t a=(int64_t)oy*b->sh,z=(int64_t)(oy+1)*b->sh;
  double wy=(double)((yr<z?yr:z)-(yl>a?yl:a));
  for(int x=0;x<b->sw;x++){
   int xa=(int)((int64_t)x*b->dw/b->sw),xb=(int)(((int64_t)(x+1)*b->dw-1)/b->sw);
   for(int ox=xa;ox<=xb;ox++){
    int64_t l=(int64_t)x*b->dw,r=(int64_t)(x+1)*b->dw,aa=(int64_t)ox*b->sw,zz=(int64_t)(ox+1)*b->sw;
    double wt=wy*(double)((r<zz?r:zz)-(l>aa?l:aa));
    for(int c=0;c<3;c++)b->sum[ox*3+c]+=row[x*3+c]*wt;
   }
  }
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
int img_modern_load(const char *path,int maxw,int maxh,Img *out,char *err,size_t errlen){
 unsigned char sig[12]={0};FILE *f=fopen(path,"rb");if(!f)return fail(err,errlen,T("画像: 開けません"));size_t n=fread(sig,1,sizeof(sig),f);fclose(f);
 if(n>=2&&sig[0]==255&&sig[1]==216)return jpeg_load(path,maxw,maxh,out,err,errlen);
 if(n>=8&&!png_sig_cmp(sig,0,8))return png_load(path,maxw,maxh,out,err,errlen);
 if(n>=6&&(!memcmp(sig,"GIF87a",6)||!memcmp(sig,"GIF89a",6)))return img_gif_load(path,maxw,maxh,0,out,err,errlen);
 if(n>=12&&!memcmp(sig,"RIFF",4)&&!memcmp(sig+8,"WEBP",4))return webp_load(path,maxw,maxh,out,err,errlen);
 return fail(err,errlen,T("画像: 未知の形式です"));
}
