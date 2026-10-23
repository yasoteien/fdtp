// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "textview.h"
#include "app.h"
#include "i18n.h"
#include "jis.h"
#include "font24.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* Revision 1 item 29: the screen's frame (cells.h frame_open). The body is
 * rows 1 .. FRAME_BOTTOM - 1 (21 at 25 rows), columns 1 .. FD_COLS - 2. Line
 * numbers (at least four digits) are in columns 1 .. digits, a line down
 * column digits + 1 joins the frame's top and bottom lines (┬ ┴), and the
 * text starts after it. The name is in the top line (after the line when the
 * numbers show), the code and 「行 N/M」 in the bottom line. */
#define BODY_TOP 1
#define ROWS FRAME_BODY_ROWS

/* A checkpoint every 256 display rows bounds the index even for huge,
 * newline-free files. No file contents are retained beyond the read cache.
 * state is the decoder state (ISO-2022-JP shift) at off. */
typedef struct { off_t off; size_t line, col; unsigned tab, state; } Position;
typedef struct {
    Screen screen;
    int fd, wrap, eof, failed, searching, choosing, numbers, automatic, laid_cols, number_digits;
    char *path, query[WIDGET_CAP];
    off_t size, cache_at, match, start;
    unsigned match_state;
    unsigned char cache[16384];
    size_t cache_n, top, horizontal, known, total, total_lines, count, capacity;
    Position *checkpoints;
    JisEncoding encoding, detected;
} TextView;

/* Order of the c window; index 0 is automatic detection. */
static const char *const code_options[]={N_("自動"),"Shift_JIS",N_("JIS（ISO-2022-JP）"),"EUC-JP","UTF-8","UTF-16LE","UTF-16BE"};
static const JisEncoding code_values[]={JIS_SHIFT_JIS,JIS_ISO2022JP,JIS_EUC_JP,JIS_UTF8,JIS_UTF16LE,JIS_UTF16BE};

static int body_left(const TextView *v) { return v->numbers ? v->number_digits+2 : 1; }
static size_t body_width(const TextView *v) { return (size_t)(FRAME_RIGHT-body_left(v)); }
static int byte_at(TextView *v, off_t at, unsigned char *b)
{
    if(at >= v->size) return 0;
    if(at < v->cache_at || (uint64_t)(at-v->cache_at) >= v->cache_n) {
        ssize_t n;
        do { n=pread(v->fd,v->cache,sizeof(v->cache),at); } while(n<0 && errno==EINTR);
        if(n<=0) { if(n<0) v->failed=1; return 0; }
        v->cache_at=at; v->cache_n=(size_t)n;
    }
    *b=v->cache[at-v->cache_at]; return 1;
}
static int wide_units(const TextView *v) { return v->encoding==JIS_UTF16LE || v->encoding==JIS_UTF16BE; }
/* One step of the decoder at at. *cp is JIS_NO_SCALAR for an escape
 * sequence; invalid bytes become U+FFFD. Returns 0 only at end of file. */
static size_t decode_at(TextView *v, off_t at, unsigned *state, uint32_t *cp)
{
    unsigned char b[8]; size_t n=0;
    if(!byte_at(v,at,b)) return 0;
    if(b[0]<128 && !wide_units(v) && (v->encoding!=JIS_ISO2022JP ||
       (*state==JIS_STATE_INITIAL && b[0]!=0x1b && b[0]!=0x0e && b[0]!=0x0f))) { *cp=b[0]; return 1; }
    n=1;
    while(n<sizeof(b) && byte_at(v,at+(off_t)n,&b[n])) ++n;
    size_t used=jis_decode_step(b,n,v->encoding,state,cp);
    if(!used) { *cp=0xfffd; used=wide_units(v) && n>=2 ? 2 : 1; }
    return used;
}
/* The character at p.off after skipping escape sequences, without moving p. */
static uint32_t peek(TextView *v, off_t off, unsigned state, off_t *after, unsigned *after_state)
{
    uint32_t cp=JIS_NO_SCALAR;
    while(cp==JIS_NO_SCALAR) {
        size_t used=decode_at(v,off,&state,&cp);
        if(!used) return JIS_NO_SCALAR;
        off+=(off_t)used;
    }
    *after=off; *after_state=state; return cp;
}
static void glyph(TextView *v,Cells *c,int row,size_t col,size_t left,uint32_t cp,int color)
{
    int width=font24_width(cp);
    if(c && col>=left && col-left+(size_t)width<=body_width(v))
        cells_put(c,(int)(col-left)+body_left(v),row,cp,color,BG);
}
static int newline(TextView *v, Position *p, uint32_t cp, off_t after, unsigned state)
{
    if(cp!='\r' && cp!='\n') return 0;
    p->off=after; p->state=state;
    if(cp=='\r') {
        off_t next; unsigned next_state;
        if(peek(v,p->off,p->state,&next,&next_state)=='\n') { p->off=next; p->state=next_state; }
    }
    ++p->line; p->col=0; return 1;
}
/* Returns the next display-row position. Tabs may straddle wrap boundaries;
 * control notation and wide glyphs are kept together. */
static Position next_row(TextView *v,Position p,Cells *c,int row)
{
    size_t x=0, left=v->wrap ? 0 : v->horizontal, width_limit=body_width(v);
    while(p.off<v->size || p.tab) {
        if(p.tab) {
            glyph(v,c,row,x,left,' ',WHITE); ++x; ++p.col; --p.tab;
            if(v->wrap && x==width_limit) return p;
            continue;
        }
        off_t after; unsigned state;
        uint32_t cp=peek(v,p.off,p.state,&after,&state);
        if(cp==JIS_NO_SCALAR) { p.off=v->size; break; }
        if(newline(v,&p,cp,after,state)) return p;
        if(cp=='\t') { p.off=after; p.state=state; p.tab=8-(unsigned)(p.col%8); continue; }
        int control=cp<32 || cp==127;
        int width=control ? 2 : font24_width(cp);
        if(v->wrap && x+(size_t)width>width_limit) return p;
        if(control) {
            glyph(v,c,row,x,left,'^',CYAN);
            glyph(v,c,row,x+1,left,cp==127 ? '?' : cp+64,CYAN);
        } else glyph(v,c,row,x,left,cp,WHITE);
        x+=(size_t)width; p.col+=(size_t)width; p.off=after; p.state=state;
        if(v->wrap && x==width_limit) {
            /* A newline immediately after a full row does not add a blank row. */
            cp=peek(v,p.off,p.state,&after,&state);
            newline(v,&p,cp,after,state);
            return p;
        }
    }
    return p;
}
static int checkpoint(TextView *v,Position p)
{
    if(v->count==v->capacity) {
        size_t cap=v->capacity ? v->capacity*2 : 16;
        Position *q=realloc(v->checkpoints,cap*sizeof(*q));
        if(!q) { v->failed=1; return 0; }
        v->checkpoints=q; v->capacity=cap;
    }
    v->checkpoints[v->count++]=p; return 1;
}
static void reset_index(TextView *v)
{
    v->count=0; v->known=0; v->eof=0; v->total=1; v->total_lines=1;
    checkpoint(v,(Position){v->start,1,0,0,JIS_STATE_INITIAL});
}
static Position position(TextView *v,size_t target)
{
    size_t block=target/256;
    if(block>=v->count) block=v->count-1;
    size_t n=block*256; Position p=v->checkpoints[block];
    while(n<target) {
        if(p.off>=v->size && !p.tab) { v->eof=1; v->total=n ? n : 1;
            v->total_lines=p.line-(p.col==0 && p.line>1 ? 1 : 0); break; }
        Position q=next_row(v,p,NULL,0);
        if(q.off==p.off && q.tab==p.tab) { v->eof=1; v->total=n+1; v->total_lines=p.line; break; }
        p=q; ++n;
        if(n>v->known) v->known=n;
        if(n%256==0 && n/256==v->count && !checkpoint(v,p)) break;
    }
    return p;
}
static void bottom(TextView *v)
{
    while(!v->eof && !v->failed) position(v,v->known+256);
    v->top=v->total>(size_t)ROWS ? v->total-(size_t)ROWS : 0;
}
static void move(TextView *v,size_t target)
{
    position(v,target+ROWS);
    size_t max=v->eof && v->total>(size_t)ROWS ? v->total-(size_t)ROWS : 0;
    v->top=v->eof && target>max ? max : target;
}
/* Puts the display row holding byte offset at on top and returns that row's
 * start position. */
static Position reveal_offset(TextView *v, off_t at)
{
    size_t block=0;
    while(block+1<v->count && v->checkpoints[block+1].off<=at) ++block;
    Position p=v->checkpoints[block]; size_t row=block*256;
    while(p.off<at) {
        Position q=next_row(v,p,NULL,0);
        if(q.off>at || q.off==p.off) break;
        p=q; ++row;
        if(row%256==0 && row/256==v->count && !checkpoint(v,p)) break;
    }
    v->top=row; return p;
}
static void find(TextView *v,App *app,int reverse,int initial)
{
    if(!v->query[0]) { app_notice(app,T("検索文字を入力してください")); return; }
    uint32_t needle[WIDGET_CAP]; size_t len=0;
    const char *s=v->query;
    while(*s && len<WIDGET_CAP) needle[len++]=font24_decode(&s);
    size_t prefix[WIDGET_CAP], j=0; prefix[0]=0;
    for(size_t i=1;i<len;++i) {
        while(j && needle[i]!=needle[j]) j=prefix[j-1];
        if(needle[i]==needle[j]) ++j;
        prefix[i]=j;
    }
    Position top=position(v,v->top);
    off_t begin=top.off, limit=top.off; unsigned state=top.state;
    if(!initial && v->match>=0) {
        /* Continue after the character that starts the current match. */
        unsigned st=v->match_state;
        off_t after; begin=v->match; limit=v->match;
        if(peek(v,begin,st,&after,&st)!=JIS_NO_SCALAR) { begin=after; state=st; }
    }
    if(reverse) { begin=v->start; state=JIS_STATE_INITIAL; }
    off_t ring[WIDGET_CAP], found=-1; unsigned states[WIDGET_CAP], found_state=0;
    j=0; size_t index=0;
    for(off_t at=begin; at<v->size;) {
        unsigned before=state; uint32_t cp; size_t n=decode_at(v,at,&state,&cp); if(!n) break;
        if(cp==JIS_NO_SCALAR) { at+=(off_t)n; continue; }
        ring[index%len]=at; states[index%len]=before;
        while(j && cp!=needle[j]) j=prefix[j-1];
        if(cp==needle[j]) ++j;
        if(j==len) {
            size_t first=(index+1-len)%len;
            off_t start=ring[first];
            if(reverse && start>=limit) break;
            found=start; found_state=states[first]; if(!reverse) break;
            j=prefix[j-1];
        }
        at+=(off_t)n; ++index;
    }
    if(found<0) { app_notice(app,T("見つかりません")); return; }
    v->match=found; v->match_state=found_state;
    Position p=reveal_offset(v,found);
    if(!v->wrap) {
        Position q=p;
        while(q.off<found) {
            off_t after; unsigned st;
            uint32_t cp=peek(v,q.off,q.state,&after,&st);
            if(cp==JIS_NO_SCALAR) break;
            q.col+=cp=='\t' ? 8-q.col%8 : (cp<32||cp==127 ? 2 : (size_t)font24_width(cp));
            q.off=after; q.state=st;
        }
        v->horizontal=(q.col/body_width(v))*body_width(v);
    }
}
/* Count modest files without building display rows. For larger files the byte
 * size bounds the physical lines, keeping sparse reads bounded on open. The
 * width is a high-water mark even if EOF later reveals far fewer lines. */
static void number_width(TextView *v)
{
    uintmax_t lines=v->size>0 ? (uintmax_t)v->size : 1;
    if(v->size<=4*1024*1024) {
        Position p={v->start,1,0,0,JIS_STATE_INITIAL};
        int ended=0;
        while(p.off<v->size) {
            off_t after; unsigned state;
            uint32_t cp=peek(v,p.off,p.state,&after,&state);
            if(cp==JIS_NO_SCALAR) break;
            ended=newline(v,&p,cp,after,state);
            if(!ended) { p.off=after; p.state=state; }
        }
        lines=p.line-(ended && p.line>1 ? 1 : 0);
    }
    int digits=1;
    while(lines>=10) { lines/=10; ++digits; }
    if(digits<4) digits=4;
    if(digits>v->number_digits) v->number_digits=digits;
}
/* Re-reads the start of the file for the BOM of the current encoding. */
static void set_encoding(TextView *v, JisEncoding encoding)
{
    unsigned char head[3]; size_t n=0;
    while(n<sizeof(head) && byte_at(v,(off_t)n,&head[n])) ++n;
    v->encoding=encoding; v->start=(off_t)jis_bom_length(head,n,encoding);
    number_width(v);
    reset_index(v); v->top=0; v->horizontal=0; v->match=-1;
}
/* Keeps the line on top when the row width changes. */
static void relayout(TextView *v)
{
    off_t at=position(v,v->top).off;
    reset_index(v); v->horizontal=0; v->match=-1;
    reveal_offset(v,at);
}
static int text_key(Screen *s,App *app,const Key *k)
{
    TextView *v=s->self;
    if(v->searching) {
        WidgetResult r=widgets_key(&app->widgets,k);
        if(r==WIDGET_ACCEPT) {
            snprintf(v->query,sizeof(v->query),"%s",app->widgets.text);
            v->match=-1; v->searching=0; find(v,app,0,1);
        } else if(r==WIDGET_CANCEL) v->searching=0;
        return 0;
    }
    if(v->choosing) {
        WidgetResult r=widgets_key(&app->widgets,k);
        if(r==WIDGET_ACCEPT) {
            int selected=app->widgets.selected;
            v->choosing=0; v->automatic=selected<=0 || selected>JIS_ENCODING_COUNT;
            set_encoding(v,v->automatic ? v->detected : code_values[selected-1]);
        } else if(r==WIDGET_CANCEL) v->choosing=0;
        return 0;
    }
    if(k->key==KESC || (k->key==KCHAR && k->ch=='q')) return 1;
    if(k->key==KUP) move(v,v->top ? v->top-1 : 0);
    else if(k->key==KDOWN) move(v,v->top+1);
    else if(k->key==KPGUP) move(v,v->top>(size_t)ROWS ? v->top-(size_t)ROWS : 0);
    else if(k->key==KPGDN || (k->key==KCHAR && k->ch==' ')) move(v,v->top+ROWS);
    else if(k->key==KLEFT) { if(v->horizontal>=8) v->horizontal-=8; }
    else if(k->key==KRIGHT) { if(v->horizontal<SIZE_MAX-8) v->horizontal+=8; }
    else if(k->key==KCHAR && !(k->mods&(MOD_CTRL|MOD_ALT))) switch(k->ch) {
    case '<': v->top=0; break;
    case '>': bottom(v); break;
    case 'c': {
        int current=0;
        if(!v->automatic) for(int i=0;i<JIS_ENCODING_COUNT;++i) if(code_values[i]==v->encoding) current=i+1;
        const char *options[JIS_ENCODING_COUNT+1];
        for(int i=0;i<JIS_ENCODING_COUNT+1;++i) options[i]=T(code_options[i]);
        widgets_choice(&app->widgets,T("文字コード"),options,JIS_ENCODING_COUNT+1);
        app->widgets.selected=current; v->choosing=1; break;
    }
    case '#': v->numbers=!v->numbers; relayout(v); break;
    case 'w': v->wrap=!v->wrap; relayout(v); break;
    case '/': widgets_input(&app->widgets,T("検索"),v->query,NULL); v->searching=1; break;
    case 'n': find(v,app,0,0); break;
    case 'N': find(v,app,1,0); break;
    }
    if(v->failed) app_notice(app,T("ファイルの読み込みに失敗しました"));
    return 0;
}
static void text_draw(Screen *s,App *app,Canvas *canvas,Cells *c)
{
    (void)app; (void)canvas; TextView *v=s->self;
    /* The screen changed width (an X window resized): rows wrap anew. */
    if(v->laid_cols!=FD_COLS) { v->laid_cols=FD_COLS; relayout(v); }
    if(frame_open(c)<0) return;
    if(v->numbers) frame_vline(c,v->number_digits+1,0,FRAME_BOTTOM);
    Position p=position(v,v->top); size_t line=p.line;
    for(int y=BODY_TOP;y<BODY_TOP+ROWS && (p.off<v->size || p.tab);++y) {
        if(v->numbers && p.col==0 && !p.tab) {
            char number[32]; snprintf(number,sizeof(number),"%*zu",v->number_digits,p.line);
            cells_text(c,1,y,number,CYAN,BG,v->number_digits);
        }
        p=next_row(v,p,c,y);
    }
    if(p.off>=v->size && !p.tab) {
        /* Establish the exact physical line count only after reaching EOF. */
        position(v,v->top+ROWS);
    }
    const char *name=strrchr(v->path,'/'); name=name ? name+1 : v->path;
    char status[160], total[32];
    if(v->eof) snprintf(total,sizeof(total),"%zu",v->total_lines);
    else snprintf(total,sizeof(total),"?");
    snprintf(status,sizeof(status),T("%s%s  行 %zu/%s"),jis_encoding_name(v->encoding),v->automatic ? T("（自動）") : "",line,total);
    int title=v->numbers ? v->number_digits+1+FRAME_TITLE_COL : FRAME_TITLE_COL;
    frame_title(c,title,name,FRAME_STATUS_END-title,NULL,0);
    frame_status(c,FRAME_STATUS_END,status,FD_COLS-8);
    const char *bands[]={" Top  ","Bottom","Search"," Next "," Code "," Wrap ","#Line "," Quit "};
    int hot[]={1,0,0,1,1,1,0,1};
    for(int i=0;i<8;++i) cells_band_item(c,2+i*10,6,bands[i],hot[i]);
}
static void text_destroy(Screen *s)
{
    TextView *v=s->self; close(v->fd); free(v->path); free(v->checkpoints); free(v);
}
Screen *textview_create(App *app,const char *path)
{
    int fd=open(path,O_RDONLY); struct stat st;
    if(fd<0 || fstat(fd,&st)<0 || !S_ISREG(st.st_mode)) {
        if(fd>=0) close(fd);
        app_notice(app,T("ファイルを開けません")); return NULL;
    }
    TextView *v=calloc(1,sizeof(*v));
    if(!v) { close(fd); app_notice(app,T("メモリが足りません")); return NULL; }
    v->fd=fd; v->size=st.st_size; v->path=strdup(path); v->cache_at=-1;
    v->wrap=1; v->match=-1; v->numbers=1; v->automatic=1; v->laid_cols=FD_COLS;
    unsigned char sample[65536]; ssize_t n=pread(fd,sample,sizeof(sample),0);
    /* Detection is bounded; c permits an explicit encoding override. */
    size_t sampled=n>0 ? (size_t)n : 0;
    if((off_t)sampled<v->size) {
        size_t at=0;
        while(at<sampled) {
            uint32_t cp; size_t used=jis_decode_one(sample+at,sampled-at,JIS_UTF8,&cp);
            if(used) { at+=used; continue; }
            unsigned char lead=sample[at];
            size_t need=lead>=0xc2 && lead<=0xdf ? 2 :
                (lead>=0xe0 && lead<=0xef ? 3 : (lead>=0xf0 && lead<=0xf4 ? 4 : 0));
            if(need && sampled-at<need) {
                size_t tail=at+1;
                while(tail<sampled && (sample[tail]&0xc0)==0x80) ++tail;
                if(tail==sampled) sampled=at;
            }
            break;
        }
    }
    v->detected=jis_detect_all(sample,sampled);
    v->screen=(Screen){v,text_key,text_draw,0,NULL,text_destroy};
    if(!v->path || n<0) {
        text_destroy(&v->screen); app_notice(app,T("ファイルを読み込めません")); return NULL;
    }
    set_encoding(v,v->detected);
    if(v->failed) {
        text_destroy(&v->screen); app_notice(app,T("ファイルを読み込めません")); return NULL;
    }
    return &v->screen;
}
