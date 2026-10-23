// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* Part of archive.o (included from archive.c): the directory tree that U
 * uses to choose where to unpack. Same keys and look as t/L/C/M
 * (fileops_tree.c, whose picker is not reachable from another module). */

#ifndef TREE_ROWS
#define TREE_ROWS (FD_ROWS - 4) /* rows 1 .. 21 at 25 rows, the path under them */
#endif
typedef struct TreeNode TreeNode;
struct TreeNode { char *path; TreeNode *parent, **children; size_t count; int loaded, open; };
typedef struct { TreeNode *root, *cursor, **visible; size_t visible_count, visible_cap, top; } Tree;

static void node_free(TreeNode *n)
{
    if(!n) return;
    for(size_t i=0;i<n->count;++i) node_free(n->children[i]);
    free(n->children); free(n->path); free(n);
}
static void tree_free(Tree *t) { node_free(t->root); free(t->visible); memset(t,0,sizeof(*t)); }
static int node_cmp(const void *a,const void *b)
{ return strcmp((*(TreeNode *const *)a)->path,(*(TreeNode *const *)b)->path); }
static char *tree_join(const char *base,const char *name)
{
    size_t n=strlen(base)+strlen(name)+2; char *p=malloc(n);
    if(p) snprintf(p,n,"%s%s%s",base,strcmp(base,"/") ? "/" : "",name);
    return p;
}
static int node_load(TreeNode *n)
{
    if(n->loaded) { n->open=1; return 0; }
    DIR *d=opendir(n->path); if(!d) return -1;
    struct dirent *e; int failed=0;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char *p=tree_join(n->path,e->d_name); struct stat st;
        if(!p) { failed=1; break; }
        if(lstat(p,&st) || !S_ISDIR(st.st_mode)) { free(p); continue; }
        TreeNode *child=calloc(1,sizeof(*child)); TreeNode **list=realloc(n->children,(n->count+1)*sizeof(*list));
        if(list) n->children=list;
        if(!child || !list) { free(child); free(p); failed=1; break; }
        child->path=p; child->parent=n; n->children[n->count++]=child;
    }
    closedir(d);
    if(failed) { for(size_t i=0;i<n->count;++i) node_free(n->children[i]); free(n->children); n->children=NULL; n->count=0; return -1; }
    if(n->count>1) qsort(n->children,n->count,sizeof(*n->children),node_cmp);
    n->loaded=n->open=1; return 0;
}
static int tree_collect(Tree *t,TreeNode *n)
{
    if(t->visible_count==t->visible_cap) {
        size_t cap=t->visible_cap ? t->visible_cap*2 : 64; TreeNode **v=realloc(t->visible,cap*sizeof(*v));
        if(!v) return -1;
        t->visible=v; t->visible_cap=cap;
    }
    t->visible[t->visible_count++]=n;
    if(n->open) for(size_t i=0;i<n->count;++i) if(tree_collect(t,n->children[i])) return -1;
    return 0;
}
static size_t tree_index(Tree *t)
{
    t->visible_count=0; tree_collect(t,t->root);
    for(size_t i=0;i<t->visible_count;++i) if(t->visible[i]==t->cursor) return i;
    return 0;
}
/* Opened from / down to start, with the cursor there. */
static int tree_init(Tree *t,const char *start)
{
    memset(t,0,sizeof(*t));
    if(!(t->root=calloc(1,sizeof(*t->root))) || !(t->root->path=strdup("/"))) { tree_free(t); return -1; }
    t->cursor=t->root;
    char *path=realpath(start,NULL);
    if(path) {
        TreeNode *n=t->root; char *part=path+1;
        while(*part) {
            char *end=strchr(part,'/'); if(end) *end=0;
            if(node_load(n)) break;
            TreeNode *next=NULL;
            for(size_t i=0;i<n->count;++i) { const char *base=strrchr(n->children[i]->path,'/'); if(base && !strcmp(base+1,part)) { next=n->children[i]; break; } }
            if(!next) break;
            n=next; t->cursor=n;
            if(!end) break;
            part=end+1;
        }
        free(path);
    }
    node_load(t->cursor);
    tree_index(t); return 0;
}
/* 1: chosen (t->cursor->path), -1: cancelled, 0: keep going. */
static int tree_key(Tree *t,App *a,const Key *k)
{
    size_t index=tree_index(t);
    if(k->key==KESC) return -1;
    if(k->key==KRET) return 1;
    if(k->key==KUP && index) --index;
    if(k->key==KDOWN && index+1<t->visible_count) ++index;
    if(k->key==KPGUP) index=index>(size_t)TREE_ROWS ? index-TREE_ROWS : 0;
    if(k->key==KPGDN && t->visible_count) index=index+TREE_ROWS<t->visible_count ? index+TREE_ROWS : t->visible_count-1;
    if(k->key==KHOME) index=0;
    if(k->key==KEND && t->visible_count) index=t->visible_count-1;
    t->cursor=t->visible[index];
    if(k->key==KRIGHT && node_load(t->cursor)) app_notice(a,T("ディレクトリを読めません"));
    if(k->key==KLEFT) { if(t->cursor->open) t->cursor->open=0; else if(t->cursor->parent) t->cursor=t->cursor->parent; }
    return 0;
}
static void tree_draw(Tree *t,Cells *c,const char *title)
{
    cells_clear(c); c->frame=0; cells_no_separators(c);
    cells_text(c,0,0,title,WHITE,BG,FD_COLS);
    size_t selected=tree_index(t);
    if(selected<t->top) t->top=selected;
    if(selected>=t->top+TREE_ROWS) t->top=selected-(TREE_ROWS-1);
    for(size_t i=t->top;i<t->visible_count && i<t->top+TREE_ROWS;++i) {
        TreeNode *n=t->visible[i]; int depth=0; for(TreeNode *p=n->parent;p;p=p->parent) ++depth;
        int col=depth*2; if(col>56+FD_XCOLS) col=56+FD_XCOLS;
        const char *name=strrchr(n->path,'/'); name=name && name[1] ? name+1 : "/";
        int row=(int)(i-t->top)+1,fg=n==t->cursor ? BG : CYAN,bg=n==t->cursor ? CYAN : BG;
        for(TreeNode *p=n;p->parent;p=p->parent) {
            int level=0; for(TreeNode *q=p->parent;q->parent;q=q->parent) ++level;
            int x=level*2; if(x>54+FD_XCOLS) continue;
            int last=p->parent->children[p->parent->count-1]==p;
            if(p==n) cells_text(c,x,row,last ? "└" : "├",CYAN,BG,2);
            else if(!last) cells_text(c,x,row,"│",CYAN,BG,2);
        }
        cells_put(c,col,row,n->loaded && !n->count ? ' ' : n->open ? '-' : '+',CYAN,BG);
        cells_text(c,col+1,row,name,fg,bg,FD_COLS-1-col);
    }
    cells_path(c,0,TREE_ROWS+1,t->cursor->path,WHITE,BG,FD_COLS);
    cells_text(c,0,FD_BAND,T("↑↓ 選択  → 開く  ← 閉じる/親  RET 決定  Esc 取消"),CYAN,BG,FD_COLS);
}
