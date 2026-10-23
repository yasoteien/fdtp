// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "i18n.h"
#include "commands.h"
#include <string.h>
typedef struct { const char *id, *key, *help; } Definition;
/* Revision 1 §2 (2026-10-22): plain letters, Ctrl+letters and Shift+F1..F10
 * (FD 3.13's Shift band Attr Info Move rmDir mKdir sHell Write Backup View
 * Pack). Item 27 (2026-10-23, the Pomera has an Alt key after all): what FD
 * had on Alt works on Alt again, besides the Ctrl key: Alt+e the external
 * editor, Alt+r Retro MIDI Player; the other Alt+letters are free for bind.
 * Item 31: Alt+i the system information. Items 37 and 39: Alt+n split,
 * Alt+h join, Alt+p the PDF menu.
 * Shift+letters are left unbound: the filer moves to the next name with that
 * initial. Commands without a key (S T, the tree variants) stay bindable in
 * ~/.fdtprc. */
static const Definition definitions[]={
#define C(id,key) {#id,key,NULL}
C(CUR_UP,"UP"), C(CUR_DOWN,"DOWN"), C(CUR_LEFT,"LEFT"), C(CUR_RIGHT,"RIGHT"), C(ROLL_DOWN,"PGUP"), C(ROLL_UP,"PGDN"),
C(CUR_TOP,"<"),C(CUR_BOTTOM,">"),
C(ONE_COLUMNS,"1"),C(TWO_COLUMNS,"2"),C(THREE_COLUMNS,"3"),C(FIVE_COLUMNS,"5"),
C(FNAME_RIGHT,"("),C(FNAME_LEFT,")"),
C(MARK_FILE,"TAB"),C(MARK_FILE2,"SPC"),C(MARK_FILE3,"C-SPC"),
C(MARK_ALL,"HOME"),C(MARK_ALL,"+"),C(MARK_REVERSE,"END"),C(MARK_REVERSE,"-"),C(MARK_FIND,"*"),
C(IN_DIR,"RET"),C(OUT_DIR,"BS"),C(LOG_TOP,"\\"),C(LOG_TOP,"¥"),C(REREAD_DIR,"C-l"),
C(LOG_DIR,"l"),C(LOG_DIR,"F1"),
C(PUSH_FILE,"DEL"),C(PUSH_FILE,"]"),C(POP_FILE,"INS"),C(POP_FILE,"["),
C(SORT_DIR,"s"),C(SORT_DIR,"F6"),C(FIND_FILE,"f"),C(FIND_FILE,"F7"),
C(SEARCH_FORW,"C-s"),C(SEARCH_BACK,"C-r"),
C(SYMLINK_MODE,NULL),C(FILETYPE_MODE,NULL),C(DOTFILE_MODE,"C-h"),C(FILEFLG_MODE,NULL),
C(SPLIT_WINDOW,"/"),C(NEXT_WINDOW,"^"),C(WIDEN_WINDOW,"C-w"),C(NARROW_WINDOW,"C-n"),C(KILL_WINDOW,"C-k"),
C(HELP_MESSAGE,"?"),C(HELP_MESSAGE,"h"),C(QUIT_SYSTEM,"q"),C(QUIT_SYSTEM,"ESC"),
C(EDIT_NOVEL,"e"),C(EDIT_NOVEL,"F9"),C(EDIT_FILE,"C-e"),C(EDIT_FILE,"M-e"),C(PLAY_MIDI,"C-p"),C(PLAY_MIDI,"M-r"),
C(EXECUTE_FILE,"x"),C(EXECUTE_FILE,"F2"),
C(COPY_FILE,"c"),C(COPY_FILE,"F3"),C(DELETE_FILE,"d"),C(DELETE_FILE,"F4"),C(RENAME_FILE,"r"),C(RENAME_FILE,"F5"),
C(ATTR_FILE,"a"),C(ATTR_FILE,"F11"),C(ATTR_FILE,"S-F1"),
C(INFO_FILE,"i"),C(INFO_FILESYS,"F12"),C(INFO_FILESYS,"S-F2"),C(INFO_SYSTEM,"M-i"),
C(MOVE_FILE,"m"),C(MOVE_FILE,"S-F3"),C(DELETE_DIR,"S-F4"),C(DELETE_DIR,"C-d"),
C(MAKE_DIR,"k"),C(MAKE_DIR,"S-F5"),C(EXECUTE_SH,"S-F6"),C(WRITE_DIR,"w"),C(WRITE_DIR,"S-F7"),
C(TREE_DIR,"t"),C(TREE_DIR,"F8"),
C(LOG_TREE,NULL),C(COPY_TREE,NULL),C(MOVE_TREE,NULL),C(ATTR_DIR,"C-a"),C(FIND_DIR,"C-f"),
C(VIEW_FILE,"v"),C(VIEW_FILE,"S-F9"),C(HEX_FILE,"b"),
C(UNPACK_FILE,"u"),C(UNPACK_FILE,"F10"),C(UNPACK_TREE,NULL),C(PACK_FILE,"p"),C(PACK_FILE,"S-F8"),C(PACK_FILE,"S-F10"),
C(EDIT_CONFIG,"z"),
C(SPLIT_FILE,"M-n"),C(JOIN_FILE,"M-h"),C(PDF_MENU,"M-p"),
C(WARNING_BELL,NULL),C(NO_OPERATION,NULL)
#undef C
};
/* The help box (? and h; revision 1 item 25). Headings (key NULL) open the
 * sections; "" continues the item above. The keys follow definitions[]
 * above (the source of truth) and revision 1 §2. Only characters in the
 * font (tests/unit.c checks). */
#define H(text) {NULL,text}
static const WidgetHelpItem help_items[]={
    H(N_("移動・マーク")),
    {"↑ ↓ ← →",N_("カーソルの移動")},
    {"PgUp PgDn",N_("前・次のページ")},
    {"< >",N_("先頭・最後へ")},
    {N_("Shift+英字"),N_("その字で始まる次の名前へ（続けて押すと次へ）")},
    {"( )",N_("長い名前の表示をずらす・戻す")},
    {"C-s C-r",N_("名前の検索（下へ・上へ）")},
    {"Tab",N_("マーク")},
    {"Space",N_("マークして下へ")},
    {"C-Space",N_("マークして下へ（ページの中だけ）")},
    {"Home + End -",N_("全部をマーク・マークの反転")},
    {"*",N_("条件でマーク")},
    H(N_("開く・表示")),
    {"Enter v Shift+F9",N_("開く（ディレクトリなら入る）")},
    {"",N_("知らない形式でバイナリなら表示の仕方を聞く")},
    {"b",N_("16 進表示")},
    {"F5",N_("スライドショー（pptx の画面）")},
    {"BS",N_("親のディレクトリへ")},
    {N_("\\ ￥"),N_("ルートへ")},
    {"l F1",N_("パスを入れて移る")},
    {"f F7",N_("名前で絞って表示（検索表示）")},
    {"t F8",N_("ツリー")},
    {"C-f",N_("ディレクトリ検索")},
    {"e F9",N_("小説用エディタ（どのファイルも）")},
    {"C-e Alt+e",N_("外部のエディタ（$VISUAL・$EDITOR・vi）")},
    {"C-p Alt+r",N_("Retro MIDI Player で開く")},
    {"",N_(".mid .rcp .r36 .g36 は Enter v でも")},
    {"x F2",N_("実行")},
    {"i",N_("ファイルの情報")},
    {"F12 Shift+F2",N_("ファイルシステムの情報")},
    {"Alt+i",N_("システム情報")},
    H(N_("ファイル操作")),
    {"c F3",N_("コピー")},
    {"m Shift+F3",N_("移動")},
    {"d F4",N_("削除")},
    {"r F5",N_("名前の変更")},
    {"a F11 Shift+F1",N_("属性")},
    {"C-a",N_("下の階層の属性")},
    {"k Shift+F5",N_("ディレクトリ作成")},
    {"C-d Shift+F4",N_("ディレクトリ削除")},
    {"w Shift+F7",N_("ディレクトリの書き込み")},
    {"p Shift+F10",N_("圧縮（書庫の作成）")},
    {"u F10",N_("解凍")},
    {"Del ] Ins [",N_("名前を外して積む・カーソルの所に戻す（5 個まで）")},
    {N_("↓（入力の中）"),N_("前に使った物の一覧（履歴）")},
    {N_("F8（入力の中）"),N_("行先・パスをツリーで選ぶ")},
    H(N_("窓・表示の切り替え")),
    {"1 2 3 5",N_("表示の列数")},
    {"s F6",N_("並べ替え")},
    {"C-h",N_("隠しファイルの表示・非表示")},
    {"C-l",N_("読み直す")},
    {"/",N_("窓の分割")},
    {"^",N_("次の窓へ")},
    {"C-w C-n",N_("窓を広げる・狭める")},
    {"C-k",N_("窓を閉じる")},
    H(N_("Shift を押している間の帯（Shift+F1〜F10）")),
    {"Shift+F1",N_("Attr    属性")},
    {"Shift+F2",N_("Info    ファイルシステムの情報")},
    {"Shift+F3",N_("Move    移動")},
    {"Shift+F4",N_("rmDir   ディレクトリ削除")},
    {"Shift+F5",N_("mKdir   ディレクトリ作成")},
    {"Shift+F6",N_("sHell   シェル")},
    {"Shift+F7",N_("Write   ディレクトリの書き込み")},
    {"Shift+F8",N_("Backup  圧縮（p と同じ）")},
    {"Shift+F9",N_("View    表示")},
    {"Shift+F10",N_("Pack    圧縮")},
    H(N_("その他（設定・終了・ヘルプ・エディタの中）")),
    {"Alt+n Alt+h Alt+p",N_("分割・結合・PDF のメニュー")},
    {"z",N_("設定（カスタマイザ）")},
    {"h ?",N_("このヘルプ")},
    {"q Esc",N_("終了（確認あり）")},
    {N_("F1（エディタ）"),N_("小説用エディタのキーの一覧")},
    {N_("F2（エディタ）"),N_("変換（書式の整形・原稿と Web の形式・Word 書き出し）")},
};
#undef H
/* The customizer's names of the commands (item 27: 「e  内蔵エディタ（Reserved）」). */
static const struct { const char *id, *label; } labels[]={
    {"CUR_UP",N_("上へ")},{"CUR_DOWN",N_("下へ")},{"CUR_LEFT",N_("左へ")},{"CUR_RIGHT",N_("右へ")},
    {"ROLL_DOWN",N_("前のページ")},{"ROLL_UP",N_("次のページ")},{"CUR_TOP",N_("先頭へ")},{"CUR_BOTTOM",N_("最後へ")},
    {"ONE_COLUMNS",N_("1 列表示")},{"TWO_COLUMNS",N_("2 列表示")},{"THREE_COLUMNS",N_("3 列表示")},{"FIVE_COLUMNS",N_("5 列表示")},
    {"FNAME_RIGHT",N_("長い名前をずらす")},{"FNAME_LEFT",N_("長い名前を戻す")},
    {"MARK_FILE",N_("マーク")},{"MARK_FILE2",N_("マークして下へ")},{"MARK_FILE3",N_("マークして下へ（ページの中）")},
    {"MARK_ALL",N_("全部をマーク")},{"MARK_REVERSE",N_("マークの反転")},{"MARK_FIND",N_("条件でマーク")},
    {"IN_DIR",N_("開く")},{"OUT_DIR",N_("親のディレクトリへ")},{"LOG_TOP",N_("ルートへ")},{"REREAD_DIR",N_("読み直す")},
    {"LOG_DIR",N_("パスを入れて移る")},{"PUSH_FILE",N_("名前を外して積む")},{"POP_FILE",N_("積んだ名前を戻す")},
    {"SORT_DIR",N_("並べ替え")},{"FIND_FILE",N_("検索表示")},{"SEARCH_FORW",N_("名前の検索（下へ）")},{"SEARCH_BACK",N_("名前の検索（上へ）")},
    {"SYMLINK_MODE",N_("リンク先の表示")},{"FILETYPE_MODE",N_("種類の記号")},{"DOTFILE_MODE",N_("隠しファイルの表示")},{"FILEFLG_MODE",N_("ファイルの印")},
    {"SPLIT_WINDOW",N_("窓の分割")},{"NEXT_WINDOW",N_("次の窓へ")},{"WIDEN_WINDOW",N_("窓を広げる")},{"NARROW_WINDOW",N_("窓を狭める")},{"KILL_WINDOW",N_("窓を閉じる")},
    {"HELP_MESSAGE",N_("ヘルプ")},{"QUIT_SYSTEM",N_("終了")},
    {"EDIT_NOVEL",N_("内蔵エディタ")},{"EDIT_FILE",N_("外部のエディタ")},{"PLAY_MIDI","Retro MIDI Player"},
    {"EXECUTE_FILE",N_("実行")},{"COPY_FILE",N_("コピー")},{"DELETE_FILE",N_("削除")},{"RENAME_FILE",N_("名前の変更")},
    {"ATTR_FILE",N_("属性")},{"INFO_FILE",N_("ファイルの情報")},{"INFO_FILESYS",N_("ファイルシステムの情報")},{"INFO_SYSTEM",N_("システム情報")},
    {"MOVE_FILE",N_("移動")},{"DELETE_DIR",N_("ディレクトリ削除")},{"MAKE_DIR",N_("ディレクトリ作成")},{"EXECUTE_SH",N_("シェル")},
    {"WRITE_DIR",N_("ディレクトリの書き込み")},{"TREE_DIR",N_("ツリー")},{"LOG_TREE",N_("ツリーで移る")},{"COPY_TREE",N_("ツリーでコピー")},
    {"MOVE_TREE",N_("ツリーで移動")},{"ATTR_DIR",N_("下の階層の属性")},{"FIND_DIR",N_("ディレクトリ検索")},
    {"VIEW_FILE",N_("表示")},{"HEX_FILE",N_("16 進表示")},{"UNPACK_FILE",N_("解凍")},{"UNPACK_TREE",N_("ツリーで解凍")},{"PACK_FILE",N_("圧縮")},
    {"EDIT_CONFIG",N_("設定（カスタマイザ）")},{"SPLIT_FILE",N_("分割")},{"JOIN_FILE",N_("結合")},{"PDF_MENU",N_("PDF のメニュー")},{"WARNING_BELL",N_("ベル")},{"NO_OPERATION",N_("何もしない")},
};
const char *commands_label(const char *id)
{
    if(!id) return NULL;
    for(size_t i=0;i<sizeof(labels)/sizeof(*labels);++i) if(!strcmp(labels[i].id,id)) return T(labels[i].label);
    return NULL;
}
const CommandTable *commands_defaults(void)
{
    static CommandTable table; static int ready;
    if(!ready) { commands_init(&table); ready=1; }
    return &table;
}
const char COMMAND_SHELL_ID[]="SHELL_COMMAND";
const char *commands_id(const char *name)
{
    if(!name) return NULL;
    for(size_t i=0;i<sizeof(definitions)/sizeof(*definitions);++i) if(!strcmp(definitions[i].id,name)) return definitions[i].id;
    return NULL;
}
int commands_known(const char *id) { return commands_id(id)!=NULL; }
const char *commands_name(size_t index)
{
    for(size_t i=0;i<sizeof(definitions)/sizeof(*definitions);++i) {
        int first=1;
        for(size_t j=0;j<i;++j) if(!strcmp(definitions[j].id,definitions[i].id)) { first=0; break; }
        if(first && !index--) return definitions[i].id;
    }
    return NULL;
}
static void copy(char *to,size_t size,const char *from)
{
    size_t n=from ? strlen(from) : 0; if(n>=size) n=size-1;
    if(n) memcpy(to,from,n);
    to[n]=0;
}
static CommandBinding *slot(CommandTable *t,const Key *key)
{
    for(size_t i=0;i<t->count;++i) if(keys_equal(&t->bindings[i].key,key)) {
        CommandBinding *b=&t->bindings[i];
        if(b->shell) t->shells[b->shell-1].used=0;
        b->shell=0; b->dir_id=NULL; b->comment[0]=0; return b;
    }
    if(t->count==COMMAND_BINDINGS) return NULL;
    CommandBinding *b=&t->bindings[t->count++]; memset(b,0,sizeof(*b)); b->key=*key; return b;
}
int commands_bind_full(CommandTable *t,const Key *key,const char *id,const char *dir_id,const char *comment)
{
    const char *known=commands_id(id),*second=dir_id ? commands_id(dir_id) : NULL;
    if(!known || (dir_id && !second)) return -1;
    CommandBinding *b=slot(t,key); if(!b) return -1;
    b->id=known; b->dir_id=second; copy(b->comment,sizeof(b->comment),comment); return 0;
}
int commands_bind(CommandTable *t,const Key *key,const char *id) { return commands_bind_full(t,key,id,NULL,NULL); }
int commands_bind_shell(CommandTable *t,const Key *key,const char *command,const char *comment)
{
    if(!command || !*command || strlen(command)>=COMMAND_TEXT) return -1;
    int free_slot=-1;
    for(size_t i=0;i<t->count;++i) if(keys_equal(&t->bindings[i].key,key) && t->bindings[i].shell) { free_slot=t->bindings[i].shell-1; break; }
    if(free_slot<0) for(int i=0;i<COMMAND_SHELLS;++i) if(!t->shells[i].used) { free_slot=i; break; }
    if(free_slot<0) return -1;
    CommandBinding *b=slot(t,key); if(!b) return -1;
    copy(t->shells[free_slot].text,COMMAND_TEXT,command); t->shells[free_slot].used=1;
    b->id=COMMAND_SHELL_ID; b->shell=free_slot+1; copy(b->comment,sizeof(b->comment),comment); return 0;
}
int commands_unbind(CommandTable *t,const Key *key)
{
    for(size_t i=0;i<t->count;++i) if(keys_equal(&t->bindings[i].key,key)) {
        if(t->bindings[i].shell) t->shells[t->bindings[i].shell-1].used=0;
        memmove(t->bindings+i,t->bindings+i+1,(t->count-i-1)*sizeof(*t->bindings)); --t->count; return 0;
    }
    return -1;
}
int commands_keymap(CommandTable *t,const Key *from,const Key *to)
{
    for(size_t i=0;i<t->map_count;++i) if(keys_equal(&t->keymaps[i].from,from)) { t->keymaps[i].to=*to; return 0; }
    if(t->map_count==COMMAND_KEYMAPS) return -1;
    t->keymaps[t->map_count++]=(CommandKeymap){*from,*to}; return 0;
}
void commands_init(CommandTable *t)
{
    memset(t,0,sizeof(*t));
    for(size_t i=0;i<sizeof(definitions)/sizeof(*definitions);++i) {
        Key k;
        if(definitions[i].key && keys_parse(definitions[i].key,&k)) commands_bind(t,&k,definitions[i].id);
    }
}
const CommandBinding *commands_find(const CommandTable *t,const Key *key)
{
    Key k=*key;
    for(size_t i=0;i<t->map_count;++i) if(keys_equal(&t->keymaps[i].from,&k)) { k=t->keymaps[i].to; break; }
    for(size_t i=0;i<t->count;++i) if(keys_equal(&t->bindings[i].key,&k)) return &t->bindings[i];
    return NULL;
}
const char *commands_shell(const CommandTable *t,const CommandBinding *b)
{ return b && b->shell>0 && b->shell<=COMMAND_SHELLS ? t->shells[b->shell-1].text : NULL; }
const char *commands_lookup(const CommandTable *t,const Key *key)
{
    const CommandBinding *b=commands_find(t,key); return b ? b->id : NULL;
}
const WidgetHelpItem *commands_help(int *count)
{
    static WidgetHelpItem translated[sizeof(help_items)/sizeof(*help_items)];
    *count=(int)(sizeof(help_items)/sizeof(*help_items));
    for(int i=0;i<*count;++i) {
        translated[i].key=help_items[i].key ? T(help_items[i].key) : NULL;
        translated[i].text=T(help_items[i].text);
    }
    return translated;
}
