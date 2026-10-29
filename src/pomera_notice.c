// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pomera_notice.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK_FILE "/var/lib/pomera-internal/check"

static void value_of(const char *line, const char *key, char *out, size_t size)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) || line[n] != '=') return;
    snprintf(out, size, "%s", line + n + 1);
    out[strcspn(out, "\r\n")] = 0;
}

void pomera_notice_start(App *a, int headless)
{
    const char *path = getenv("FDTP_POMERA_CHECK");
    if (!path || !*path) {
        if (headless || getenv("FDTP_TEST_KEYS")) return;
        path = CHECK_FILE;
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024], state[32] = "", mount[16] = "", boot[64] = "", when[64] = "", output[512] = "";
    while (fgets(line, sizeof(line), f)) {
        value_of(line, "state", state, sizeof(state));
        value_of(line, "mount", mount, sizeof(mount));
        value_of(line, "boot_id", boot, sizeof(boot));
        value_of(line, "time", when, sizeof(when));
        value_of(line, "fsck_output", output, sizeof(output));
    }
    fclose(f);
    if (strcmp(state, "damaged")) return;

    /* Once per boot: the boot_id last shown. */
    char seen_path[1024] = "";
    const char *seen = getenv("FDTP_POMERA_SEEN"), *home = getenv("HOME");
    if (seen && *seen) snprintf(seen_path, sizeof(seen_path), "%s", seen);
    else if (home && *home) snprintf(seen_path, sizeof(seen_path), "%s/.fdtp/internal-check-seen", home);
    if (*seen_path && *boot) {
        char last[64] = "";
        FILE *s = fopen(seen_path, "r");
        if (s) { if (fgets(last, sizeof(last), s)) last[strcspn(last, "\r\n")] = 0; fclose(s); }
        if (!strcmp(last, boot)) return;
        if (!seen || !*seen) { char dir[1024]; snprintf(dir, sizeof(dir), "%s/.fdtp", home); mkdir(dir, 0755); }
        s = fopen(seen_path, "w");
        if (s) { fprintf(s, "%s\n", boot); fclose(s); }
    }

    const char *names[] = { T("結果"), T("つなぎ方"), T("点検の時刻"), T("点検の出力"), T("直し方") };
    const char *values[] = {
        T("Pomera の起動時の点検で、内蔵メモリに破損が見つかりました"),
        !strcmp(mount, "ro") ? T("読み取り専用（/mnt/pomera-internal）") : T("つないでいません"),
        *when ? when : "-",
        *output ? output : "-",
        T("自動修復はしていません。写しから戻すか、sudo fsck.fat -r で 1 つずつ確かめて直してください。短い名前の改名を聞かれたら n で断ってください（純正の OS の日本語名です）"),
    };
    widgets_info(&a->widgets, T("内蔵メモリの点検"), names, values, 5);
    a->changed = 1;
}
