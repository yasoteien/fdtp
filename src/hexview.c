// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "hexview.h"
#include "app.h"
#include "i18n.h"
#include "jis.h"
#include "font24.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { off_t offset; unsigned char original, value; } Change;
typedef enum { HEX_IDLE, HEX_GOTO, HEX_SEARCH, HEX_SAVE, HEX_QUIT, HEX_CODE } HexPrompt;
/* Revision 1 item 29: the screen's frame (cells.h frame_open), sixteen bytes
 * on each of rows 1 .. FRAME_BOTTOM - 1 (21 at 25 rows): address 1..8, a line
 * down column 9, hex 11..58 (48 with the units' blanks), a line down column
 * 60, characters 61..77 (a wide one begun on byte 15 takes 77). Both lines
 * join the frame's top and bottom lines (┬ ┴). The name is in the top line
 * between them, the unit, the code and 「行 N/M」 in the bottom line there. */
#define BODY_TOP 1
#define ROWS FRAME_BODY_ROWS
#define ROWS_MAX (FD_ROWS_MAX - 4)
#define ADDRESS_COL 1
#define HEX_COL 11
#define RULE1 9
#define RULE2 60
#define CHAR_COL 61
typedef struct {
    Screen screen;
    char *path;
    int fd, character, nibble;
    off_t size, cursor, top;
    Change *changes;
    size_t count, capacity;
    unsigned char *pattern;
    size_t pattern_size;
    HexPrompt prompt;
    int group, swap, automatic; /* group: bytes per unit, 1, 2 or 4 */
    JisEncoding encoding, detected;
} HexView;
static const char *const code_options[]={N_("自動"),"Shift_JIS",N_("JIS（ISO-2022-JP）"),"EUC-JP","UTF-8","UTF-16LE","UTF-16BE"};
static const JisEncoding code_values[]={JIS_SHIFT_JIS,JIS_ISO2022JP,JIS_EUC_JP,JIS_UTF8,JIS_UTF16LE,JIS_UTF16BE};

static ssize_t read_at(int fd, void *data, size_t size, off_t offset)
{
    ssize_t n;
    do { n=pread(fd,data,size,offset); } while(n<0 && errno==EINTR);
    return n;
}
static size_t change_index(const HexView *v, off_t offset)
{
    size_t lo=0, hi=v->count;
    while(lo<hi) { size_t mid=lo+(hi-lo)/2; if(v->changes[mid].offset<offset) lo=mid+1; else hi=mid; }
    return lo;
}
static void overlay(const HexView *v, unsigned char *data, size_t length, off_t offset)
{
    for(size_t i=change_index(v,offset);i<v->count && v->changes[i].offset-offset<(off_t)length;++i)
        data[v->changes[i].offset-offset]=v->changes[i].value;
}
static void reveal(HexView *v)
{
    off_t row=v->cursor/16;
    if(row<v->top/16) v->top=row*16;
    if(row>=v->top/16+ROWS) v->top=(row-(ROWS-1))*16;
}
static int set_byte(HexView *v, App *app, unsigned char value)
{
    if(v->cursor>=v->size) return 0;
    size_t i=change_index(v,v->cursor);
    if(i<v->count && v->changes[i].offset==v->cursor) {
        if(value==v->changes[i].original) {
            memmove(v->changes+i,v->changes+i+1,(v->count-i-1)*sizeof(*v->changes)); --v->count;
        } else v->changes[i].value=value;
        return 1;
    }
    unsigned char original;
    if(read_at(v->fd,&original,1,v->cursor)!=1) { app_notice(app,T("ファイルを読めません")); return 0; }
    if(original==value) return 1;
    if(v->count==v->capacity) {
        size_t capacity=v->capacity ? v->capacity*2 : 64;
        if(capacity<v->capacity || capacity>SIZE_MAX/sizeof(*v->changes)) { app_notice(app,T("メモリが足りません")); return 0; }
        Change *changes=realloc(v->changes,capacity*sizeof(*changes));
        if(!changes) { app_notice(app,T("メモリが足りません")); return 0; }
        v->changes=changes; v->capacity=capacity;
    }
    memmove(v->changes+i+1,v->changes+i,(v->count-i)*sizeof(*v->changes));
    v->changes[i]=(Change){v->cursor,original,value}; ++v->count; return 1;
}
static int hex_digit(uint32_t c)
{
    if(c>='0' && c<='9') return (int)c-'0';
    if(c>='a' && c<='f') return (int)c-'a'+10;
    if(c>='A' && c<='F') return (int)c-'A'+10;
    return -1;
}
static void save_changes(HexView *v, App *app)
{
    int fd=open(v->path,O_WRONLY);
    if(fd<0) { app_notice(app,T("書き込み用に開けません")); return; }
    struct stat source, target;
    if(fstat(v->fd,&source)<0 || fstat(fd,&target)<0 || source.st_dev!=target.st_dev || source.st_ino!=target.st_ino || target.st_size!=v->size) {
        close(fd); app_notice(app,T("ファイルが入れ替わったか長さが変わりました")); return;
    }
    size_t written=0;
    for(;written<v->count;++written) {
        ssize_t n;
        do { n=pwrite(fd,&v->changes[written].value,1,v->changes[written].offset); } while(n<0 && errno==EINTR);
        if(n!=1) break;
    }
    int failed=written!=v->count;
    if(close(fd)<0) failed=1;
    if(written) { memmove(v->changes,v->changes+written,(v->count-written)*sizeof(*v->changes)); v->count-=written; }
    app_notice(app,failed ? T("書き込みに失敗しました（未保存の差分を保持）") : T("書き込みました"));
}
static int parse_pattern(HexView *v, const char *text)
{
    size_t length=strlen(text), count=0;
    unsigned char *pattern=malloc(length ? length : 1);
    if(!pattern) return 0;
    if(length>=2 && text[0]=='"' && text[length-1]=='"') {
        count=length-2; memcpy(pattern,text+1,count);
    } else {
        int high=-1;
        for(size_t i=0;i<length;++i) {
            if(isspace((unsigned char)text[i])) continue;
            int digit=hex_digit((unsigned char)text[i]);
            if(digit<0) { free(pattern); return 0; }
            if(high<0) high=digit;
            else { pattern[count++]=(unsigned char)(high*16+digit); high=-1; }
        }
        if(high>=0) { free(pattern); return 0; }
    }
    if(!count) { free(pattern); return 0; }
    free(v->pattern); v->pattern=pattern; v->pattern_size=count; return 1;
}
static void search_next(HexView *v, App *app, int from_current)
{
    if(!v->pattern_size) { app_notice(app,T("検索する値を指定してください")); return; }
    unsigned char data[8192+WIDGET_CAP];
    off_t offset=v->cursor+(from_current ? 0 : 1);
    while(offset<v->size) {
        size_t want=8192+v->pattern_size-1;
        if(v->size-offset<(off_t)want) want=(size_t)(v->size-offset);
        ssize_t got=read_at(v->fd,data,want,offset);
        if(got<0) { app_notice(app,T("検索中に読めませんでした")); return; }
        if(!got) break;
        overlay(v,data,(size_t)got,offset);
        size_t candidates=(size_t)got>=v->pattern_size ? (size_t)got-v->pattern_size+1 : 0;
        if(candidates>8192) candidates=8192;
        for(size_t i=0;i<candidates;++i) if(!memcmp(data+i,v->pattern,v->pattern_size)) {
            v->cursor=offset+(off_t)i; v->nibble=0; reveal(v); return;
        }
        if(!candidates) break;
        offset+=(off_t)candidates;
    }
    app_notice(app,T("見つかりません"));
}
static void choose_encoding(HexView *v, int selected)
{
    v->automatic=selected<=0 || selected>JIS_ENCODING_COUNT;
    v->encoding=v->automatic ? v->detected : code_values[selected-1];
}
static int prompt_key(HexView *v, App *app, const Key *key)
{
    WidgetResult result=widgets_key(&app->widgets,key);
    if(result!=WIDGET_ACCEPT && result!=WIDGET_CANCEL) return 0;
    HexPrompt prompt=v->prompt; v->prompt=HEX_IDLE;
    if(result==WIDGET_CANCEL) return 0;
    if(prompt==HEX_QUIT) return 1;
    if(prompt==HEX_SAVE) save_changes(v,app);
    else if(prompt==HEX_CODE) choose_encoding(v,app->widgets.selected);
    else if(prompt==HEX_SEARCH) {
        if(parse_pattern(v,app->widgets.text)) search_next(v,app,1);
        else app_notice(app,T("検索値は引用した文字列か偶数桁の16進数で入力してください"));
    } else if(prompt==HEX_GOTO) {
        const char *text=app->widgets.text;
        while(isspace((unsigned char)*text)) ++text;
        char *end; errno=0;
        uintmax_t offset=strtoumax(text,&end,16);
        while(isspace((unsigned char)*end)) ++end;
        if(!*text || *text=='-' || *end || errno || offset>=(uintmax_t)v->size) app_notice(app,T("番地がファイルの範囲外です"));
        else { v->cursor=(off_t)offset; v->nibble=0; reveal(v); }
    }
    return 0;
}
/* Display slot (0..15) of byte column col in its row: units of v->group
 * bytes left to right, the bytes of a unit reversed when swapped. The
 * mapping is its own inverse. */
static int slot_of(const HexView *v, int col)
{
    int g=v->group;
    return v->swap && g>1 ? col/g*g+g-1-col%g : col;
}
static int hex_x(const HexView *v, int col)
{
    int g=v->group, slot=slot_of(v,col);
    return HEX_COL+slot/g*(2*g+1)+slot%g*2+(slot>=8);
}
/* Moves the cursor one slot left or right on screen, skipping slots past EOF. */
static void visual_step(HexView *v, int direction)
{
    off_t limit=(v->size+15)/16*16;
    off_t at=v->cursor/16*16+slot_of(v,(int)(v->cursor%16))+direction;
    for(;at>=0 && at<limit;at+=direction) {
        off_t offset=at/16*16+slot_of(v,(int)(at%16));
        if(offset<v->size) { v->cursor=offset; return; }
    }
}
static int hex_key(Screen *screen, App *app, const Key *key)
{
    HexView *v=screen->self;
    if(v->prompt!=HEX_IDLE) return prompt_key(v,app,key);
    if(key->key==KESC || (key->key==KCHAR && key->ch=='q' && !(key->mods&(MOD_CTRL|MOD_ALT)))) {
        if(!v->count) return 1;
        widgets_confirm(&app->widgets,T("書き込まずに戻りますか？")); v->prompt=HEX_QUIT; return 0;
    }
    if(key->key==KCHAR && key->ch=='s' && (key->mods&MOD_CTRL) && !(key->mods&MOD_ALT)) {
        if(!v->count) app_notice(app,T("書き換えはありません"));
        else { widgets_confirm(&app->widgets,T("書き込みますか？")); v->prompt=HEX_SAVE; }
        return 0;
    }
    if(key->mods&(MOD_CTRL|MOD_ALT)) return 0;
    if(key->key==KCHAR && key->ch=='g') {
        widgets_input(&app->widgets,T("番地（16進）"),"",NULL); v->prompt=HEX_GOTO; return 0;
    }
    if(key->key==KCHAR && key->ch=='/') {
        widgets_input(&app->widgets,T("検索（\"文字\" / 16進）"),"",NULL); v->prompt=HEX_SEARCH; return 0;
    }
    if(key->key==KCHAR && key->ch=='n') { search_next(v,app,0); return 0; }
    if(key->key==KCHAR && key->ch=='w' && !v->character) { v->group=v->group==4 ? 1 : v->group*2; v->nibble=0; return 0; }
    if(key->key==KCHAR && key->ch=='s' && !v->character) {
        if(v->group==1) app_notice(app,T("入れ替えはワードかダブルワードの時に使えます（w で切り替え）"));
        else { v->swap=!v->swap; v->nibble=0; }
        return 0;
    }
    if(key->key==KCHAR && key->ch=='c' && !v->character) {
        int current=0;
        if(!v->automatic) for(int i=0;i<JIS_ENCODING_COUNT;++i) if(code_values[i]==v->encoding) current=i+1;
        const char *options[JIS_ENCODING_COUNT+1];
        for(int i=0;i<JIS_ENCODING_COUNT+1;++i) options[i]=T(code_options[i]);
        widgets_choice(&app->widgets,T("文字コード"),options,JIS_ENCODING_COUNT+1);
        app->widgets.selected=current; v->prompt=HEX_CODE; return 0;
    }
    if(key->key==KTAB) { v->character=!v->character; v->nibble=0; return 0; }
    off_t movement=0;
    if(key->key==KUP) movement=-16;
    else if(key->key==KDOWN) movement=16;
    else if(key->key==KLEFT || key->key==KRIGHT) {
        int direction=key->key==KLEFT ? -1 : 1;
        if(v->character) movement=direction;
        else { visual_step(v,direction); v->nibble=0; reveal(v); return 0; }
    }
    else if(key->key==KPGUP) movement=-16*ROWS;
    else if(key->key==KPGDN) movement=16*ROWS;
    else if(key->key==KHOME || (key->key==KCHAR && key->ch=='<')) { v->cursor=0; v->nibble=0; reveal(v); return 0; }
    else if(key->key==KEND || (key->key==KCHAR && key->ch=='>')) { v->cursor=v->size ? v->size-1 : 0; v->nibble=0; reveal(v); return 0; }
    if(movement) {
        off_t next=v->cursor+movement;
        v->cursor=next<0 ? 0 : (next>=v->size ? (v->size ? v->size-1 : 0) : next);
        v->nibble=0; reveal(v); return 0;
    }
    if(key->key!=KCHAR || v->cursor>=v->size) return 0;
    if(v->character) {
        uint32_t ch=key->ch;
        if(ch>=0xff61 && ch<=0xff9f) ch=ch-0xff61+0xa1;
        else if(ch<32 || ch>126) return 0;
        if(set_byte(v,app,(unsigned char)ch) && v->cursor+1<v->size) ++v->cursor;
    } else {
        int digit=hex_digit(key->ch);
        if(digit<0) return 0;
        unsigned char value;
        if(read_at(v->fd,&value,1,v->cursor)!=1) { app_notice(app,T("ファイルを読めません")); return 0; }
        overlay(v,&value,1,v->cursor);
        value=v->nibble ? (unsigned char)((value&0xf0)|digit) : (unsigned char)((value&15)|(digit<<4));
        if(set_byte(v,app,value)) {
            /* After the low digit, the next byte on screen (the last byte keeps the cursor). */
            if(v->nibble) { visual_step(v,1); v->nibble=0; }
            else v->nibble=1;
        }
    }
    reveal(v); return 0;
}
/* What the character column shows for each byte: a scalar at the first byte
 * of a character, 0 for the rest of it (left blank), '.' otherwise. Decoding
 * starts before top so that a character begun on an earlier row (or an
 * ISO-2022-JP shift) is still known. */
static int printable(uint32_t cp)
{
    return cp>=32 && cp!=127 && !(cp>=0x80 && cp<0xa0) && cp!=0xfffd && cp!=0xfeff &&
           !(cp>=0xe000 && cp<=0xf8ff) && !(cp>=0xd800 && cp<=0xdfff);
}
static void characters(HexView *v, off_t top, size_t count, uint32_t *shown)
{
    for(size_t i=0;i<count;++i) shown[i]='.';
    if(!count) return;
    size_t window=v->encoding==JIS_ISO2022JP ? 65536 : 4096;
    size_t back=(uintmax_t)top<window ? (size_t)top : window;
    size_t want=back+count+8;
    unsigned char *data=malloc(want);
    if(!data) return;
    ssize_t got=read_at(v->fd,data,want,top-(off_t)back);
    if(got<=0) { free(data); return; }
    size_t length=(size_t)got, at=0;
    overlay(v,data,length,top-(off_t)back);
    switch(v->encoding) {
    case JIS_UTF8: at=back>3 ? back-3 : 0; break;
    case JIS_UTF16LE: case JIS_UTF16BE: at=back>2 ? back-2 : 0; break;
    case JIS_SHIFT_JIS: case JIS_EUC_JP:
        /* After a byte that cannot continue a character the decoder is in step. */
        for(at=back;at>0;--at) if(data[at-1]<(v->encoding==JIS_SHIFT_JIS ? 0x40 : 0x80)) break;
        break;
    case JIS_ISO2022JP:
        for(at=back;at>0;--at) if(data[at-1]==0x1b || data[at-1]==0x0e || data[at-1]==0x0f) { --at; break; }
        break;
    }
    unsigned state=JIS_STATE_INITIAL;
    while(at<back+count && at<length) {
        uint32_t cp; size_t used=jis_decode_step(data+at,length-at,v->encoding,&state,&cp);
        int bad=!used || cp==JIS_NO_SCALAR;
        if(!used) used=(v->encoding==JIS_UTF16LE || v->encoding==JIS_UTF16BE) && length-at>=2 ? 2 : 1;
        if(!bad && (!printable(cp) || (font24_width(cp)==2 && used<2))) bad=1;
        for(size_t i=0;i<used;++i) {
            size_t index=at+i;
            if(index<back || index>=back+count) continue;
            shown[index-back]=bad ? '.' : (i ? 0 : cp);
        }
        at+=used;
    }
    free(data);
}
static void hex_draw(Screen *screen, App *app, Canvas *canvas, Cells *cells)
{
    (void)app; (void)canvas;
    HexView *v=screen->self;
    if(frame_open(cells)<0) return;
    frame_vline(cells,RULE1,0,FRAME_BOTTOM); frame_vline(cells,RULE2,0,FRAME_BOTTOM);
    const char *name=strrchr(v->path,'/'); name=name ? name+1 : v->path;
    static const char *const units[]={"",N_("バイト"),N_("ワード"),"",N_("ダブルワード")};
    char position[160];
    snprintf(position,sizeof(position),T("16進 %s%s  %s%s  行 %jd/%jd"),T(units[v->group]),
             v->swap && v->group>1 ? T(" 入替") : "",jis_encoding_name(v->encoding),v->automatic ? T("（自動）") : "",
             (intmax_t)(v->size ? v->cursor/16+1 : 0),(intmax_t)((v->size+15)/16));
    const char *modified=v->count ? T("変更あり") : NULL;
    int modified_width=modified ? cells_width(modified)+1 : 0;
    frame_title(cells,RULE1+FRAME_TITLE_COL,name,RULE2-2-(RULE1+FRAME_TITLE_COL)-modified_width,modified,MAGENTA);
    frame_status(cells,RULE2-1,position,RULE2-1-RULE1-3);
    unsigned char data[16*ROWS_MAX];
    uint32_t shown[16*ROWS_MAX];
    ssize_t got=read_at(v->fd,data,16*(size_t)ROWS,v->top);
    if(got<0) { cells_text(cells,HEX_COL,BODY_TOP,T("ファイルを読めません"),YELLOW,BG,RULE2-1-HEX_COL); got=0; }
    overlay(v,data,(size_t)got,v->top);
    characters(v,v->top,(size_t)got,shown);
    static const char digits[]="0123456789ABCDEF";
    for(int row=0;row<ROWS && (off_t)(row*16)<got;++row) {
        int y=BODY_TOP+row;
        char address[32];
        snprintf(address,sizeof(address),"%08" PRIXMAX,(uintmax_t)(v->top+row*16));
        cells_text(cells,ADDRESS_COL,y,address,WHITE,BG,8);
        for(int col=0;col<16 && row*16+col<got;++col) {
            off_t offset=v->top+row*16+col;
            unsigned char value=data[row*16+col];
            size_t ci=change_index(v,offset);
            int changed=ci<v->count && v->changes[ci].offset==offset;
            int color=changed ? YELLOW : WHITE, x=hex_x(v,col);
            cells_put(cells,x,y,digits[value>>4],color,BG);
            cells_put(cells,x+1,y,digits[value&15],color,BG);
            uint32_t code=shown[row*16+col];
            if(code) cells_put(cells,CHAR_COL+col,y,code,color,BG);
        }
        if(v->cursor>=v->top+row*16 && v->cursor<v->top+row*16+16 && v->cursor-v->top<got) {
            int col=(int)(v->cursor%16), x;
            if(v->character) {
                x=CHAR_COL+col;
                if(cells->data[y][x].part==2) --x;
            } else x=hex_x(v,col)+v->nibble;
            Cell *cell=&cells->data[y][x];
            cells_put(cells,x,y,cell->code,cell->bg,cell->fg);
        }
    }
    static const char *const bands[]={"Search"," Next "," Goto "," Tab  "," Word "," Swap "," Code ","Write "," Top  "," Quit "};
    static const int hot[]={0,1,1,1,1,1,1,-1,1,1}; /* Write is Ctrl+s */
    for(int i=0;i<10;++i) cells_band_item(cells,1+i*8,6,bands[i],hot[i]);
}
static void hex_destroy(Screen *screen)
{
    HexView *v=screen->self;
    close(v->fd); free(v->path); free(v->changes); free(v->pattern); free(v);
}
Screen *hexview_create(App *app, const char *path)
{
    int fd=open(path,O_RDONLY);
    struct stat st;
    if(fd<0) { app_notice(app,T("ファイルを開けません")); return NULL; }
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode)) { close(fd); app_notice(app,T("通常のファイルを選んでください")); return NULL; }
    HexView *v=calloc(1,sizeof(*v));
    if(!v) { close(fd); app_notice(app,T("メモリが足りません")); return NULL; }
    v->fd=fd; v->size=st.st_size; v->path=strdup(path); v->group=1;
    if(!v->path) { close(fd); free(v); app_notice(app,T("メモリが足りません")); return NULL; }
    unsigned char sample[4096]; ssize_t n=read_at(fd,sample,sizeof(sample),0);
    /* The sample may end inside a character; detection only needs a guess. */
    v->detected=jis_detect_all(sample,n>0 ? (size_t)n : 0); v->encoding=v->detected; v->automatic=1;
    v->screen=(Screen){.self=v,.key=hex_key,.draw=hex_draw,.destroy=hex_destroy};
    return &v->screen;
}
