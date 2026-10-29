// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "keys.h"
#include "widgets.h"
#include "app.h"
#include "filer.h"
#include "raster.h"
#include "launcher.h"
#include "noveledit.h"
#include "font24.h"
#include "fdscreen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void key(Widgets *w, const char *s) { Key k; assert(keys_parse(s, &k)); assert(widgets_key(w, &k) != WIDGET_PASS); }
static void touch(const char *s) { FILE *f=fopen(s,"w"); assert(f); assert(!fclose(f)); }
static int destroyed;
static int mock_key(Screen *s, App *a, const Key *k) { (void)s; (void)a; return k->key==KESC; }
static void mock_draw(Screen *s, App *a, Canvas *c, Cells *cells) { (void)s; (void)a; (void)c; cells_put(cells,4,8,'X',WHITE,BG); }
static void mock_destroy(Screen *s) { ++destroyed; free(s); }
static Screen *mock_screen(void) { Screen *s=calloc(1,sizeof(*s)); assert(s); s->key=mock_key; s->draw=mock_draw; s->destroy=mock_destroy; return s; }
static void pixel_draw(Screen *s, App *a, Canvas *c, Cells *cells) { (void)s; (void)a; (void)cells; canvas_clear(c,0xff0000); }
static void application_tests(void) {
 App *a=calloc(1,sizeof(*a)); Canvas *canvas=malloc(sizeof(*canvas)); assert(a&&canvas); Cells cells; cells_init(&cells); canvas_init(canvas); app_init(a);
 Screen *first=mock_screen(), *second=mock_screen(); assert(!app_push(a,first)&&!app_push(a,second)&&a->depth==2&&app_top(a)==second);
 Key escape={KESC,0,0}; app_key(a,&escape); assert(a->depth==1&&app_top(a)==first&&destroyed==1&&!a->quit);
 app_draw(a,canvas,&cells); FDRect rects[FD_ROWS_MAX]; assert(raster_update(canvas,&cells,rects)>0);
 a->changed=1; app_draw(a,canvas,&cells); assert(raster_update(canvas,&cells,rects)==0);
 Screen *pixel=mock_screen(); pixel->pixel=1; pixel->draw=pixel_draw; assert(!app_push(a,pixel)); app_draw(a,canvas,&cells); assert(CANVAS_PX(canvas,0,0)[0]==255);
 app_pop(a); app_draw(a,canvas,&cells); assert(raster_update(canvas,&cells,rects)>0); assert(CANVAS_PX(canvas,0,0)[0]==0&&CANVAS_PX(canvas,0,0)[1]==0&&CANVAS_PX(canvas,0,0)[2]==0x55);
 cells_put(&cells,4,8,'Y',WHITE,BG); assert(raster_update(canvas,&cells,rects)==1);
 app_tick(a,120); assert(a->now==120&&a->changed); a->changed=0; app_tick(a,121); assert(!a->changed);
 char program[]="editor", arg[]="a b.txt", cwd[]="/directory"; const char *args[]={program,arg,NULL};
 assert(!app_request_external_at(a,args,APP_EXTERNAL_PAUSE,cwd)); program[0]='E'; arg[0]='A'; cwd[1]='D';
 assert(!strcmp(a->external.argv[0],"editor")&&!strcmp(a->external.argv[1],"a b.txt")&&!strcmp(a->external.cwd,"/directory")&&a->external.flags==APP_EXTERNAL_PAUSE);
 assert(app_request_external(a,args,0)==-1); app_external_clear(a); assert(!a->external.pending&&!a->external.argv[0]&&!a->external.cwd);
 const char *too_many[APP_EXTERNAL_ARGS+1]; for(int i=0;i<APP_EXTERNAL_ARGS;++i) too_many[i]="x"; too_many[APP_EXTERNAL_ARGS]=NULL;
 assert(app_request_external(a,too_many,0)==-1&&!a->external.pending); assert(!app_request_external(a,args,0)); app_external_done(a,1); assert(!a->external.pending&&a->widgets.kind==WIDGET_ERROR);
 for(int i=1;i<APP_SCREENS;++i) assert(!app_push(a,mock_screen())); Screen *overflow=mock_screen(); assert(app_push(a,overflow)==-1); free(overflow);
 app_destroy(a); assert(a->quit&&a->depth==0&&destroyed==APP_SCREENS+2); free(canvas); free(a);
}
/* Revision 1 item 25: the help boxes use only characters in the font (one
 * missing shows as ? or 〓) and no Alt key binding. */
static void help_glyphs(const WidgetHelpItem *items, int count) {
 assert(items&&count>0&&!items[0].key);
 for(int i=0;i<count;++i) for(int j=0;j<2;++j) {
  const char *p=j ? items[i].text : items[i].key; if(!p) continue;
  assert(!strstr(p,"M-")&&!strstr(p,"Alt+F")&&!strstr(p,"Meta"));
  while(*p) { uint32_t code=font24_decode(&p); const Font24Glyph *g=font24_lookup(code);
   if(g->code!=code) { fprintf(stderr,"help item %d: U+%04X is not in the font\n",i,(unsigned)code); assert(0); } }
 }
}
static void help_tests(void) {
 int n; const WidgetHelpItem *items=commands_help(&n); help_glyphs(items,n);
 items=noveledit_help(&n); help_glyphs(items,n);
}
static void command_tests(void) {
 CommandTable t; commands_init(&t); Key k, from, to;
 const char *aliases[]={"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","C-SPC","¥","C-e",
  /* revision 1 §2 */ "S-F1","S-F2","S-F3","S-F4","S-F5","S-F6","S-F7","S-F8","S-F9","S-F10","C-d","C-f","C-a","C-h","C-w","C-n","C-k","C-p","h","?","i","e","a","m","k","w","v","p","z",
  /* revision 1 item 27: Alt again */ "M-e","M-r", /* item 31 */ "M-i"};
 const char *ids[]={"LOG_DIR","EXECUTE_FILE","COPY_FILE","DELETE_FILE","RENAME_FILE","SORT_DIR","FIND_FILE","TREE_DIR","EDIT_NOVEL","UNPACK_FILE","ATTR_FILE","INFO_FILESYS","MARK_FILE3","LOG_TOP","EDIT_FILE",
  "ATTR_FILE","INFO_FILESYS","MOVE_FILE","DELETE_DIR","MAKE_DIR","EXECUTE_SH","WRITE_DIR","PACK_FILE","VIEW_FILE","PACK_FILE","DELETE_DIR","FIND_DIR","ATTR_DIR","DOTFILE_MODE","WIDEN_WINDOW","NARROW_WINDOW","KILL_WINDOW","PLAY_MIDI","HELP_MESSAGE","HELP_MESSAGE","INFO_FILE","EDIT_NOVEL","ATTR_FILE","MOVE_FILE","MAKE_DIR","WRITE_DIR","VIEW_FILE","PACK_FILE","EDIT_CONFIG",
  "EDIT_FILE","PLAY_MIDI","INFO_SYSTEM"};
 assert(sizeof(aliases)==sizeof(ids));
 for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i) { assert(keys_parse(aliases[i],&k)); const char *id=commands_lookup(&t,&k); assert(id&&!strcmp(id,ids[i])); }
 /* Shift+letters, the tree variants and the S T keys are unbound; the
  * Alt+letters but e and r are free (item 27). */
 const char *unbound[]={"A","C","D","E","F","H","K","L","M","N","O","S","T","U","V","W","M-a","M-b","M-d","M-z","M-SPC"};
 for(size_t i=0;i<sizeof(unbound)/sizeof(*unbound);++i) { assert(keys_parse(unbound[i],&k)); assert(!commands_lookup(&t,&k)); }
 assert(keys_parse("F1",&k)&&!strcmp(commands_lookup(&t,&k),"LOG_DIR")); /* F1 without Shift keeps its command */
 assert(commands_known("LOG_TREE")&&commands_known("COPY_TREE")&&commands_known("MOVE_TREE")&&commands_known("UNPACK_TREE")&&commands_known("SYMLINK_MODE")&&commands_known("FILETYPE_MODE"));
 help_tests();
 /* Item 27: every command has a name for the customizer, in the font; the
  * defaults table is the one commands_init makes. */
 for(size_t i=0;commands_name(i);++i) {
  const char *label=commands_label(commands_name(i)); assert(label);
  while(*label) { uint32_t code=font24_decode(&label); assert(font24_lookup(code)->code==code); }
 }
 assert(!commands_label("nonexistent")&&!commands_label(NULL));
 const CommandTable *d=commands_defaults(); assert(d->count==t.count&&d==commands_defaults());
 assert(keys_parse("M-e",&k)&&!strcmp(commands_lookup(d,&k),"EDIT_FILE"));
 assert(keys_parse("C-a",&k)); assert(!commands_bind(&t,&k,"CUR_DOWN")); assert(!strcmp(commands_lookup(&t,&k),"CUR_DOWN"));
 assert(commands_bind(&t,&k,"unknown-command")==-1); assert(!strcmp(commands_lookup(&t,&k),"CUR_DOWN"));
 assert(keys_parse("M-a",&from)&&keys_parse("C-a",&to)); assert(!commands_keymap(&t,&from,&to)); assert(!strcmp(commands_lookup(&t,&from),"CUR_DOWN"));
 assert(!commands_bind(&t,&k,"CUR_UP")); assert(!strcmp(commands_lookup(&t,&from),"CUR_UP"));
 assert(commands_known("NO_OPERATION")&&!commands_known("nonexistent")&&!commands_known(NULL));
}
/* Item 28: the keep-awake repaints (app_awake_due), with the boundaries. */
static void awake_tests(void) {
 double w;
 assert(app_awake_due(15,100,0,100,&w)==1&&w==APP_AWAKE_PERIOD);           /* first one at once */
 assert(app_awake_due(15,100,100,100.1,&w)==0&&w>0.0499&&w<0.0501);        /* not yet: 0.05 s left */
 assert(app_awake_due(15,100,100,100+APP_AWAKE_PERIOD,&w)==1);              /* exactly the period */
 assert(app_awake_due(15,100,0,100+15*60-0.01,&w)==1&&w==-1);              /* the last one: none after it */
 assert(app_awake_due(15,100,0,100+15*60,&w)==0&&w==-1);                   /* the time is over */
 assert(app_awake_due(15,100,100+15*60-0.1,100+15*60-0.01,&w)==0&&w==-1);  /* next would be past the end */
 assert(app_awake_due(0,100,0,100,&w)==0&&w==-1);                           /* 0: never */
 assert(app_awake_due(-1,100,0,100,NULL)==0);
 assert(app_awake_due(1440,0,0,1440*60-1,NULL)==1&&app_awake_due(1440,0,0,1440*60,NULL)==0);
 App *a=calloc(1,sizeof(*a)); assert(a); app_init(a); assert(a->settings.awake_minutes==APP_AWAKE_DEFAULT&&APP_AWAKE_DEFAULT==15); app_destroy(a); free(a);
}
void hook(void *c) { ++*(int *)c; }
static void external_modal_test(const char *fixture) {
 App *a=calloc(1,sizeof(*a)); assert(a); app_init(a); Screen *s=filer_create(fixture); assert(s&&!app_push(a,s));
 Key k; assert(keys_parse("l",&k)); app_key(a,&k); assert(a->widgets.kind==WIDGET_INPUT);
 app_external_done(a,1); assert(a->widgets.kind==WIDGET_INPUT);
 assert(keys_parse("ESC",&k)); app_key(a,&k); assert(a->widgets.kind==WIDGET_ERROR); assert(keys_parse("q",&k)); app_key(a,&k); assert(a->widgets.kind==WIDGET_NONE&&a->depth==1);
 app_key(a,&k); assert(a->widgets.kind==WIDGET_CONFIRM);
 assert(keys_parse("y",&k)); app_key(a,&k); assert(a->quit); app_destroy(a); free(a);
}
/* The top row's Total, Used and Free share one unit (FDTM: Free was in
 * bytes beside Total and Used in K), on the Pomera's 80 and 85 columns and
 * wider ones, and stay in their fields. */
static void top_row_units(void) {
 const int sizes[][2]={{80,25},{85,25},{120,36},{160,45}};
 const uint64_t cases[][3]={{494384795648ull,382213701632ull,112171094016ull},{1000000ull,400000ull,600000ull},{8ull<<40,(8ull<<40)-1,1}};
 for(size_t i=0;i<sizeof(sizes)/sizeof(*sizes);++i) for(size_t j=0;j<sizeof(cases)/sizeof(*cases);++j) {
  fd_set_size(sizes[i][0],sizes[i][1]);
  DirList list={0}; list.path="/x"; list.total=cases[j][0]; list.used=cases[j][1]; list.free_bytes=cases[j][2];
  Cells c; cells_init(&c); fdscreen_build(&c,&list,0,2,0);
  char row[FD_COLS_MAX+1]; for(int x=0;x<FD_COLS;++x) { uint32_t code=c.data[1][x].code; row[x]=code<128&&code>=32 ? (char)code : '|'; } row[FD_COLS]=0;
  const char *labels[]={"Total:","Used:","Free:"}; char unit[3];
  for(int k=0;k<3;++k) { const char *at=strstr(row,labels[k]); assert(at); at+=strlen(labels[k]);
   while(*at==' ') ++at; const char *end=at; while((*end>='0'&&*end<='9')||*end==',') ++end;
   assert(end>at); unit[k]=(*end=='K'||*end=='M'||*end=='G') ? *end : ' '; }
  if(unit[0]!=unit[1]||unit[1]!=unit[2]) { fprintf(stderr,"top row %dx%d: %s\n",sizes[i][0],sizes[i][1],row); assert(0); }
  /* Nothing ran into the next label: the field ends are blank or a divider. */
  assert(strstr(row,"Total:")&&strstr(row,"Used:")&&strstr(row,"Free:"));
 }
 fd_set_size(80,25);
}
/* FDTM: a pane notices its directory changing on the disk (filer_stale), and
 * filer_unmark takes every mark off (after a drag moved them away). */
static void stale_tests(const char *base) {
 char dir[4096], file[4200]; assert(snprintf(dir,sizeof(dir),"%s/stale-XXXXXX",base)>0); assert(mkdtemp(dir));
 snprintf(file,sizeof(file),"%s/a.txt",dir); touch(file);
 App *a=calloc(1,sizeof(*a)); assert(a); app_init(a); Screen *s=filer_create(dir); assert(s&&!app_push(a,s));
 assert(!filer_stale(s)); assert(!filer_stale(s));
 snprintf(file,sizeof(file),"%s/b.txt",dir); touch(file); assert(filer_stale(s)); assert(!filer_stale(s));
 assert(!unlink(file)); assert(filer_stale(s));
 filer_reload(s,a); Key k; assert(keys_parse("DOWN",&k)); app_key(a,&k); assert(keys_parse("SPC",&k)); app_key(a,&k); assert(filer_marked(s));
 filer_unmark(s,a); assert(!filer_marked(s));
 snprintf(file,sizeof(file),"%s/a.txt",dir); assert(!unlink(file)); assert(!rmdir(dir)); app_destroy(a); free(a);
}
int main(int argc, char **argv) {
 assert(argc==3); application_tests(); top_row_units(); stale_tests(argv[1]); command_tests(); awake_tests(); external_modal_test(argv[2]);
 Key k,a,b; assert(keys_evdev(3,MOD_SHIFT,&k)&&k.ch=='"');
 assert(keys_evdev(26,0,&k)&&k.ch=='@'); assert(keys_evdev(40,MOD_SHIFT,&k)&&k.ch=='*');
 assert(!keys_evdev(58,0,&k)); assert(keys_evdev(30,128,&k)&&k.ch=='a'&&k.mods==0);
 assert(keys_parse("C-SPC",&k)&&k.ch==' '&&k.mods==MOD_CTRL);
 assert(keys_parse("M-A",&k)&&k.ch=='A'&&k.mods==(MOD_ALT|MOD_SHIFT));
 assert(keys_parse("S-a",&k)&&k.ch=='A');
 assert(keys_parse("F12",&k)&&k.key==KF12); assert(!keys_parse("F13",&k));
 assert(!keys_parse("xx",&k)); assert(!keys_parse("\xc0\xaf",&k));
 assert(keys_parse("A",&a)); b=a; b.mods=0; assert(keys_equal(&a,&b));
 assert(keys_parse("TAB",&a)); b=a; b.mods=MOD_SHIFT; assert(!keys_equal(&a,&b));
 const int lettercodes[]={30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
 for(int i=0;i<26;++i) { assert(keys_evdev(lettercodes[i],0,&k)&&k.ch==(uint32_t)('a'+i)); assert(keys_evdev(lettercodes[i],MOD_SHIFT,&k)&&k.ch==(uint32_t)('A'+i)); }
 const int symbolcodes[]={2,3,4,5,6,7,8,9,10,11,12,13,26,27,39,40,43,51,52,53,57,89,124};
 const uint32_t unshift[]={'1','2','3','4','5','6','7','8','9','0','-','^','@','[',';',':',']',',','.','/',' ','\\',0xa5};
 const uint32_t shifted[]={'!','"','#','$','%','&','\'','(',')','0','=','~','`','{','+','*','}','<','>','?',' ','_','|'};
 for(size_t i=0;i<sizeof(symbolcodes)/sizeof(symbolcodes[0]);++i) { assert(keys_evdev(symbolcodes[i],0,&k)&&k.ch==unshift[i]); assert(keys_evdev(symbolcodes[i],MOD_SHIFT,&k)&&k.ch==shifted[i]); }
 const int specialcodes[]={1,14,15,28,96,102,103,104,105,106,107,108,109,110,111,59,60,61,62,63,64,65,66,67,68,87,88};
 const int specialkeys[]={KESC,KBS,KTAB,KRET,KRET,KHOME,KUP,KPGUP,KLEFT,KRIGHT,KEND,KDOWN,KPGDN,KINS,KDEL,KF1,KF2,KF3,KF4,KF5,KF6,KF7,KF8,KF9,KF10,KF11,KF12};
 for(size_t i=0;i<sizeof(specialcodes)/sizeof(specialcodes[0]);++i) assert(keys_evdev(specialcodes[i],MOD_CTRL|MOD_SHIFT,&k)&&k.key==specialkeys[i]&&k.mods==(MOD_CTRL|MOD_SHIFT));
 KeyScript s; ScriptEvent e; keys_script_init(&s,"\"\" a A C-s M-e UP SPC \"日 a\\\"\\\\\\t\\n\" SNAP WAIT:0.125 #comment\nRET");
 const uint32_t expected[]={'a','A','s','e',0,' ',0x65e5,' ','a','"','\\',0,0};
 const int special[]={KCHAR,KCHAR,KCHAR,KCHAR,KUP,KCHAR,KCHAR,KCHAR,KCHAR,KCHAR,KCHAR,KTAB,KRET};
 for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) { e=keys_script_next(&s); assert(e.kind==SCRIPT_KEY); assert(e.key.key==special[i]); assert(e.key.ch==expected[i]); }
 assert(keys_script_next(&s).kind==SCRIPT_SNAP); e=keys_script_next(&s); assert(e.kind==SCRIPT_WAIT&&e.seconds==0.125);
 assert(keys_script_next(&s).key.key==KRET); assert(keys_script_next(&s).kind==SCRIPT_END);
 const char *bad[]={"\"x", "WAIT:", "WAIT:-1", "WAIT:nan", "WAIT:inf", "\"\\q\"", "\"a\"b", "\"\xc0\xaf\""};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { keys_script_init(&s,bad[i]); do { e=keys_script_next(&s); } while(e.kind==SCRIPT_KEY); assert(e.kind==SCRIPT_ERROR); assert(s.error[0]); }
 Widgets *w=calloc(1,sizeof(*w)); assert(w); widgets_init(w);
 widgets_input(w,"入力","abc","."); key(w,"LEFT"); key(w,"X"); assert(!strcmp(w->text,"abXc")&&w->cursor==3);
 key(w,"BS"); key(w,"DEL"); assert(!strcmp(w->text,"ab")); key(w,"C-a"); key(w,"z"); key(w,"C-k"); assert(!strcmp(w->text,"z"));
 key(w,"RET"); assert(w->kind==WIDGET_NONE&&!strcmp(w->text,"z"));
 widgets_input(w,"入力","draft","."); key(w,"UP"); assert(!strcmp(w->text,"z")); key(w,"DOWN"); assert(!strcmp(w->text,"draft")); key(w,"C-u"); assert(!w->text[0]&&!w->cursor);
 key(w,"ESC"); assert(w->kind==WIDGET_NONE);
 widgets_input(w,"入力","日本a","."); key(w,"LEFT"); key(w,"BS"); assert(!strcmp(w->text,"日a")); key(w,"HOME"); key(w,"DEL"); assert(!strcmp(w->text,"a"));
 widgets_confirm(w,"終了しますか？"); assert(keys_parse("y",&k)); assert(widgets_key(w,&k)==WIDGET_ACCEPT&&w->kind==WIDGET_NONE);
 widgets_confirm(w,"終了しますか？"); assert(keys_parse("ESC",&k)); assert(widgets_key(w,&k)==WIDGET_CANCEL);
 widgets_notice(w,"notice"); assert(widgets_key(w,&k)==WIDGET_PASS&&w->kind==WIDGET_NONE);
 /* Revision 1 §7: notices that read as errors become a box that takes the key. */
 widgets_notice(w,"ファイルを開けません"); assert(w->kind==WIDGET_ERROR&&widgets_message(w)&&!widgets_row23(w)); assert(widgets_key(w,&k)==WIDGET_HANDLED&&w->kind==WIDGET_NONE);
 const char *errors[]={"ディレクトリではありません","書き込みに失敗しました（未保存の差分を保持）","番地がファイルの範囲外です","名前が空です","値が正しくありません","外部コマンドが正常に終了しませんでした"};
 for(size_t i=0;i<sizeof(errors)/sizeof(*errors);++i) { widgets_notice(w,errors[i]); assert(w->kind==WIDGET_ERROR); }
 const char *plain[]={"見つかりません","変更はありません","積んだ項目はもうありません","保存しました","","コピー中 1/3 a.txt","取り消す操作はありません"};
 for(size_t i=0;i<sizeof(plain)/sizeof(*plain);++i) { widgets_notice(w,plain[i]); assert(w->kind==WIDGET_NOTICE&&widgets_row23(w)); }
 widgets_error(w,"保存しました"); assert(w->kind==WIDGET_ERROR); w->kind=WIDGET_NONE;
 /* HOLD-SHIFT / RELEASE-SHIFT (revision 1 §2) */
 keys_script_init(&s,"HOLD-SHIFT S-F1 RELEASE-SHIFT"); e=keys_script_next(&s); assert(e.kind==SCRIPT_MODS&&e.mods==MOD_SHIFT);
 e=keys_script_next(&s); assert(e.kind==SCRIPT_KEY&&e.key.key==KF1&&e.key.mods==MOD_SHIFT); e=keys_script_next(&s); assert(e.kind==SCRIPT_MODS&&e.mods==0);
 /* Mnemonics "(T)" pick at once; the label wraps over rows. */
 const char *binary[]={"テキスト表示(T)","バイナリ表示(B)","キャンセル"};
 widgets_choice(w,"このファイルはバイナリ形式の可能性があります。参照方法を選んでください。",binary,3); key(w,"b"); assert(w->kind==WIDGET_NONE&&w->selected==1);
 widgets_choice(w,"x",binary,3); assert(keys_parse("T",&k)&&widgets_key(w,&k)==WIDGET_ACCEPT&&w->selected==0);
 widgets_choice(w,"x",binary,3); assert(keys_parse("C-t",&k)&&widgets_key(w,&k)==WIDGET_HANDLED&&w->kind==WIDGET_CHOICE); /* Ctrl is not a mnemonic */
 key(w,"ESC");
 { Cells box; cells_init(&box); widgets_choice(w,"このファイルはバイナリ形式の可能性があります。参照方法を選んでください。",binary,3); widgets_draw(w,&box);
   assert(box.box_count==1&&box.boxes[0].h==5); char row[400]=""; for(int y=box.boxes[0].row+1;y<box.boxes[0].row+3;++y) for(int x=0;x<80;++x) if(box.data[y][x].part!=2 && box.data[y][x].code>0x3000) { char u[5]={0}; uint32_t c=box.data[y][x].code; u[0]=(char)(0xe0|c>>12); u[1]=(char)(0x80|(c>>6&63)); u[2]=(char)(0x80|(c&63)); strcat(row,u); }
   assert(strstr(row,"ください")); key(w,"ESC"); }
 /* F8 in a destination/path input calls the hook; other kinds do not. */
 { static int called; void hook(void *c); widgets_tree_hook(w,hook,&called);
   widgets_input_history(w,"コピー先","",".",WIDGET_HISTORY_DESTINATION); key(w,"F8"); assert(called==1&&w->kind==WIDGET_INPUT);
   widgets_input_history(w,"名前","",".",WIDGET_HISTORY_NAME); key(w,"F8"); assert(called==1); key(w,"ESC");
   widgets_input_history(w,"移動先","",".",WIDGET_HISTORY_PATH); key(w,"S-F8"); assert(called==1); key(w,"F8"); assert(called==2); key(w,"ESC");
   widgets_tree_hook(w,NULL,NULL); }
 const char *opts[]={"name","extension","size"}; widgets_choice(w,"順序",opts,3); key(w,"RIGHT"); assert(w->selected==1); key(w,"s"); assert(w->selected==2); key(w,"RET"); assert(w->kind==WIDGET_NONE&&w->selected==2);
 char temp[4096]; assert(snprintf(temp,sizeof(temp),"%s/widgets-XXXXXX",argv[1])>0); assert(mkdtemp(temp)); char path[8192];
 /* launcher_binary boundaries (revision 1 §1 2): the first 8192 bytes only,
  * one control byte in 16 is still text, UTF-16 without a BOM is text. */
 { struct { size_t n; int at; unsigned char byte; int fill; int expect; } cases[]={
     {0,-1,0,'a',0}, {1,0,0,'a',1}, {8192,8191,0,'a',1}, {8193,8192,0,'a',0}, {1,0,0x1b,'a',0}, {1,0,0x1a,'a',0},
     {2,-1,0,'a',0} };
   for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
     snprintf(path,sizeof(path),"%s/bin%zu",temp,i); FILE *f=fopen(path,"wb"); assert(f);
     for(size_t j=0;j<cases[i].n;++j) fputc((int)j==cases[i].at ? cases[i].byte : cases[i].fill,f); assert(!fclose(f));
     assert(launcher_binary(path)==cases[i].expect); }
   /* 160 bytes: 10 control bytes (=n/16) is text, 11 is binary. */
   for(int extra=0;extra<2;++extra) { snprintf(path,sizeof(path),"%s/ctl%d",temp,extra); FILE *f=fopen(path,"wb"); assert(f);
     for(int j=0;j<160;++j) fputc(j<10+extra ? 1 : 'x',f); assert(!fclose(f)); assert(launcher_binary(path)==extra); }
   /* UTF-16LE: "AB。\n"; "一　A\n" (NULs on both sides: U+4E00, U+3000); odd length; NULs only; a BOM;
    * UTF-16BE "AB\n\n"; ASCII with one NUL (too few ASCII pairs as UTF-16). */
   const unsigned char le[]={'A',0,'B',0,0x02,0x30,'\n',0}, kanji[]={0,0x4e,0,0x30,'A',0,'\n',0}, odd[]={'A',0,'B'}, zeros[]={0,0,0,0},
     bom[]={0xff,0xfe,0,0,0,0}, be[]={0,'A',0,'B',0,'\n',0,'\n'};
   static unsigned char one_nul[256]; memset(one_nul,'a',sizeof(one_nul)); one_nul[101]=0;
   const unsigned char *data[]={le,kanji,odd,zeros,bom,be,one_nul}; size_t sizes[]={sizeof(le),sizeof(kanji),sizeof(odd),sizeof(zeros),sizeof(bom),sizeof(be),sizeof(one_nul)};
   int want[]={0,0,1,1,0,0,1};
   for(int i=0;i<7;++i) { snprintf(path,sizeof(path),"%s/u16-%d",temp,i); FILE *f=fopen(path,"wb"); assert(f); fwrite(data[i],1,sizes[i],f); assert(!fclose(f)); assert(launcher_binary(path)==want[i]); }
   snprintf(path,sizeof(path),"%s/missing-file",temp); assert(launcher_binary(path)==-1); }
 snprintf(path,sizeof(path),"%s/alpha.txt",temp); touch(path); snprintf(path,sizeof(path),"%s/alpine",temp); touch(path);
 snprintf(path,sizeof(path),"%s/directory",temp); assert(!mkdir(path,0700));
 widgets_input(w,"path","al",temp); key(w,"TAB"); assert(!strcmp(w->text,"alp"));
 widgets_input(w,"path","alph",temp); key(w,"TAB"); assert(!strcmp(w->text,"alpha.txt"));
 widgets_input(w,"path","di",temp); key(w,"TAB"); assert(!strcmp(w->text,"directory/"));
 snprintf(path,sizeof(path),"%s/日本",temp); touch(path); snprintf(path,sizeof(path),"%s/日記",temp); touch(path);
 widgets_input(w,"path","",temp); key(w,"TAB"); assert(!strcmp(w->text,""));
 widgets_input(w,"path","日",temp); key(w,"TAB"); assert(!strcmp(w->text,"日"));
 widgets_input(w,"input","",temp); key(w,"¥"); assert(!strcmp(w->text,"¥")&&w->cursor==2); key(w,"BS"); assert(!w->text[0]&&!w->cursor);
 Cells cells; cells_init(&cells); cells_put(&cells,0,0,'Z',WHITE,BG); char longtext[200]; memset(longtext,'a',sizeof(longtext)-1); longtext[sizeof(longtext)-1]=0;
 widgets_input(w,"入力",longtext,temp); widgets_draw(w,&cells); assert(cells.data[0][0].code=='Z'&&w->scroll>0);
 int cursor_count=0; for(int x=7;x<73;++x) if(cells.data[11][x].bg==BG && cells.data[11][x].fg==WHITE) ++cursor_count; assert(cursor_count==1&&cells.box_count==1);
 key(w,"HOME"); widgets_draw(w,&cells); assert(w->scroll==0);
 free(w); puts("keys/widgets tests passed"); return 0;
}
