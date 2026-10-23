// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include "archive.h"
#include "i18n.h"
#include "app.h"
#include "fdscreen.h"
#include "jis.h"
#include "launcher.h"
#include "font24.h"
#include "lha_write.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* One object (archive.o, the Makefile's COMMON entry) built from parts. */
#include "archive_run.c"
#include "archive_parse.c"
#include "archive_tree.c"
#include "archive_ops.c"
#include "archive_browser.c"
#include "archive_pack.c"

int archive_browse(App *app, const char *path) { return browse(app,path); }

int archive_list(App *app, const FileSelection *selection)
{
    if(!selection || !selection->current || !*selection->current || !strcmp(selection->current,"..")) { app_notice(app,T("対象がありません")); return -1; }
    char *path=resolve(selection->directory,selection->current);
    if(!path) { app_notice(app,T("メモリが足りません")); return -1; }
    int r=browse(app,path); free(path); return r;
}

static int unpack(App *app,const FileSelection *selection,int tree)
{
    if(!selection || !selection->count) { app_notice(app,T("対象がありません")); return -1; }
    const char **names=calloc(selection->count,sizeof(*names)); size_t n=0;
    if(!names) { app_notice(app,T("メモリが足りません")); return -1; }
    ArchiveFormat refused=FMT_NONE;
    for(size_t i=0;i<selection->count;++i) {
        ArchiveFormat f=archive_format(selection->names[i]);
        if(f==FMT_TAR || f==FMT_ZIP || f==FMT_LHA) names[n++]=selection->names[i];
        else if(f==FMT_7Z) refused=f;
    }
    if(!n) { free(names); app_notice(app,refused==FMT_7Z ? T("この環境では使えません") : T("アーカイブではありません")); return -1; }
    for(size_t i=0;i<n;++i) if(archive_format(names[i])==FMT_LHA) {
        char *l=find_command("lhasa"); if(!l) { free(names); app_notice(app,T("lhasa がありません")); return -1; } free(l); break;
    }
    ArcOp *o=op_create(app,ARC_UNPACK,selection->directory,names,n); free(names);
    if(!o) return -1;
    return op_ask_destination(app,o,tree);
}
int archive_unpack(App *app, const FileSelection *selection) { return unpack(app,selection,0); }
int archive_unpack_tree(App *app, const FileSelection *selection) { return unpack(app,selection,1); }

int archive_pack(App *app, const FileSelection *selection)
{
    if(!selection || !selection->count) { app_notice(app,T("対象がありません")); return -1; }
    return pack_open(app,selection);
}
