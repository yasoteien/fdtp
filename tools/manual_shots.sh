#!/bin/bash
# Screen pictures for the manual, in Japanese and English (revision 1 item 48).
#
#   fdtp/tools/manual_shots.sh [--remote HOST] [--keep] [SHOT...]
#
# Runs the Mac's fdtp-preview headless at 85x25 (the Pomera's 1024x600) and
# writes docs/media/fdtp/manual/{ja,en}/NN-name.png, the same names in both.
# With SHOT names (e.g. 01-filer) only those are taken again; the name pdf
# stands for all the PDF shots (18-20).
#
# The PDF viewer, its annotations and the PDF menu (18-20) need MuPDF, which
# the Mac build does not have (mk/pdfview.mk): they are taken on the Debian
# build host instead. The tree (fdtp/ and port/app/platform) and the sample
# files go to its ~/work/fdtp-manual-shots, fdtp is built there against the
# sysroot of tools/make_sysroot.sh -a ARCH -d (~/work/sysroot-ARCH-deb, ARCH
# the host's own) and run with --headless and LD_LIBRARY_PATH on it; the
# snapshots come back here and ~/work/fdtp-manual-shots is removed (--keep
# leaves it). The host is --remote HOST, else FDTP_BUILD_HOST (environment
# or ~/.config/fdtp-dev/env, as tests/dev_env.py reads it). With no SHOT
# names and no host the PDF shots are left out with a notice.
#
# Everything on the screen comes from here: the files are the test fixtures
# (tools/make_test_office.py, make_test_images.py, make_test_pdf.py) and a
# short novel written for this script. $HOME is the sample directory, so the
# filer says ~; the clock is fixed (--time). Each shot starts from a fresh
# copy of the sample directory (the PDF shots save into sample.pdf). The work
# goes in fdtp/build/manual-shots (not /tmp). After the run every snapshot's
# text is searched for addresses, home paths and the user and host names
# (of the build host too): any found stops the script.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

APP="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$APP/.." && pwd)"
BUILD="$APP/build"
EXE="$BUILD/fdtp-preview"
WORK="$BUILD/manual-shots"
DEST="$REPO/docs/media/fdtp/manual"
SIZE=85x25
NOW=1700000000          # 2023-11-14 22:13 UTC
REMOTE_DIR=work/fdtp-manual-shots   # under the build host's home

HOST= KEEP=0
while [ $# -gt 0 ]; do
  case $1 in
    --remote) [ $# -ge 2 ] || { echo "manual_shots.sh: --remote HOST" >&2; exit 2; }; HOST=$2; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -*) sed -n '2,30p' "$0" >&2; exit 2 ;;
    *) break ;;
  esac
done
if [ -z "$HOST" ]; then
  HOST=$(cd "$APP/tests" && python3 -B -c 'import dev_env; print(dev_env.get("FDTP_BUILD_HOST", ""))')
fi

[ "$(uname)" = Darwin ] || { echo "manual_shots.sh: run it on the Mac (fdtp-preview)" >&2; exit 2; }
make -C "$APP" >/dev/null

rm -rf "$WORK"
mkdir -p "$WORK" "$BUILD/tmp"

# ---- the sample files ------------------------------------------------------
python3 -B - "$APP" "$WORK" "$NOW" <<'PY'
import importlib.util, os, sys, zipfile
from pathlib import Path

app, work, now = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

office = load('make_test_office', app / 'tools/make_test_office.py')
images = load('make_test_images', app / 'tools/make_test_images.py')
pdf = load('make_test_pdf', app / 'tools/make_test_pdf.py')
sys.path.insert(0, str(app / 'tests'))
import check_sysinfo  # make_root: a made-up /proc, /sys, /etc, /dev

# Written for this script (not from any work): ruby 《》, ｜…《》 and 傍点 《《》》.
NOVEL = '''灯台の話

　港町の外れに、古い灯台《とうだい》が立っていた。
　夜になると、｜光の帯《ひかりのおび》が海をなでて回る。
　少女はその光を《《毎晩》》数えていた。
　「今夜は、いくつ数えられるかな」
　風が窓を鳴らし、遠くで汽笛《きてき》が応えた。
　少女は指を折りながら、静かに目を閉じた。
'''
TEXT = {
    'ja': '''FDTP のテキスト表示の見本です。

ファイラーで v を押すと、このようにテキストを読めます。
Shift-JIS・EUC-JP・UTF-8 の文字コードを見分けて表示します。

  1. 上下の矢印で 1 行ずつ動く
  2. スペースで次のページ
  3. q で戻る

The quick brown fox jumps over the lazy dog.
0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ
''',
    'en': '''A sample for the FDTP text viewer.

Press v in the filer to read a text file like this one.
The viewer tells Shift-JIS, EUC-JP and UTF-8 apart by itself.

  1. The up and down arrows move one line
  2. Space shows the next page
  3. q goes back

The quick brown fox jumps over the lazy dog.
0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ
''',
}

fixtures = work / 'fixtures'
import contextlib, io
with contextlib.redirect_stdout(io.StringIO()):
    images.generate(fixtures / "images")
pdf_dir = pdf.generate(fixtures / 'pdf')
jpeg = (fixtures / 'images' / 'quadrants.jpg').read_bytes()

for lang in ('ja', 'en'):
    home = work / lang / 'pristine'
    (home / 'documents').mkdir(parents=True)
    (home / 'pictures').mkdir()
    (home / 'music').mkdir()
    office.make_docx(home / 'report.docx')
    office.make_xlsx(home / 'budget.xlsx')
    office.make_pptx(home / 'slides.pptx', jpeg)
    (home / 'sample.pdf').write_bytes((pdf_dir / 'sample.pdf').read_bytes())
    images.png(home / 'photo.png', 400, 300)
    (home / 'novel.txt').write_text(NOVEL, encoding='utf-8')
    (home / 'readme.txt').write_text(TEXT[lang], encoding='utf-8')
    (home / 'data.bin').write_bytes(bytes(range(256)) * 16)
    with zipfile.ZipFile(home / 'archive.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in [('docs/', b''), ('docs/readme.txt', TEXT[lang].encode()),
                           ('docs/novel.txt', NOVEL.encode()), ('photo.png', (home / 'photo.png').read_bytes()),
                           ('data.bin', bytes(range(256)) * 16)]:
            z.writestr(zipfile.ZipInfo(name, date_time=(2023, 11, 10, 9, 30, 0)), data)
    # Fixed times: a few hours apart, before --time.
    for i, p in enumerate(sorted(home.rglob('*'), key=str)):
        os.utime(p, (now - 3600 * (i + 1), now - 3600 * (i + 1)), follow_symlinks=False)
    os.utime(home, (now, now))
    # The filer's ..: shoot.sh gives WORK/LANG this file's time after each copy.
    (home.parent / 'parent.time').touch()
    os.utime(home.parent / 'parent.time', (now - 86400 * 3, now - 86400 * 3))

# The system information reads this tree (FDTP_SYSINFO_ROOT): GPU 15 minutes, a battery.
check_sysinfo.make_root(work, 900000)
PY

# ---- the shots -------------------------------------------------------------
# name|keys|extra environment (space separated KEY=VALUE)
SHOTS=(
  '01-filer|C-s "novel.txt" ESC SPC SPC|'
  '02-help|h|'
  '03-settings|z|'
  '04-first-run|SNAP|FIRST_RUN'
  '05-sysinfo|\ M-i|FDTP_SYSINFO_ROOT=@WORK@/sysroot'
  '06-text|C-s "readme.txt" ESC v|'
  '07-hex|C-s "data.bin" ESC b|'
  '08-docx|C-s "report.docx" ESC v|'
  '09-xlsx|C-s "budget.xlsx" ESC v|'
  '10-pptx|C-s "slides.pptx" ESC v|'
  '11-slideshow|C-s "slides.pptx" ESC v F5|'
  '12-image|C-s "photo.png" ESC v|'
  '13-archive|C-s "archive.zip" ESC RET WAIT:0 WAIT:0|'
  '14-split|C-s "data.bin" ESC M-n|'
  '15-novel-horizontal|C-s "novel.txt" ESC e|'
  '16-novel-vertical|C-s "novel.txt" ESC e C-t|'
  '17-novel-format|C-s "novel.txt" ESC e F2|'
)
# On the build host (MuPDF). sample.pdf page 1's text: "Hello FDTP search
# target" (characters 1-24), 日本語の明朝体 (25-31), ゴシック体の文字 (32-39).
# 19: FDTP highlighted, "search" underlined, a note on 日, saved (the mode's
# band stays). The note's text is typed as it is (no input method).
PDF_SHOTS=(
  '18-pdf|C-s "sample.pdf" ESC v WAIT:0 WAIT:0|'
  '19-pdf-annot|C-s "sample.pdf" ESC v WAIT:0 e WAIT:0 RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT S-RIGHT S-RIGHT S-RIGHT h WAIT:0 RIGHT RIGHT S-RIGHT S-RIGHT S-RIGHT S-RIGHT S-RIGHT u WAIT:0 RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT t "@NOTE@" RET WAIT:0 C-s WAIT:0 RIGHT LEFT WAIT:0|'
  '20-pdf-menu|C-s "sample.pdf" ESC v WAIT:0 M-p|'
)

# Which shots: all of them, or the names given (pdf: 18-20).
mac_list="$WORK/mac.list" pdf_list="$WORK/pdf.list"
: >"$mac_list"; : >"$pdf_list"
wanted=" $* "
for shot in "${SHOTS[@]}"; do
  if [ $# -eq 0 ] || [[ "$wanted" == *" ${shot%%|*} "* ]]; then printf '%s\n' "$shot" >>"$mac_list"; fi
done
for shot in "${PDF_SHOTS[@]}"; do
  if [ $# -eq 0 ] || [[ "$wanted" == *" ${shot%%|*} "* || "$wanted" == *" pdf "* ]]; then printf '%s\n' "$shot" >>"$pdf_list"; fi
done
for name in "$@"; do
  [ "$name" = pdf ] || grep -q "^$name|" "$mac_list" "$pdf_list" || { echo "manual_shots.sh: no shot $name" >&2; exit 2; }
done
if [ -s "$pdf_list" ] && [ -z "$HOST" ]; then
  if [ $# -gt 0 ]; then echo "manual_shots.sh: the PDF shots need a build host (--remote HOST or FDTP_BUILD_HOST)" >&2; exit 2; fi
  echo "manual_shots.sh: no build host (--remote HOST or FDTP_BUILD_HOST): the PDF shots are left out" >&2
  : >"$pdf_list"
fi

# shoot.sh WORK LIST EXE [ARG...]: each shot of LIST in ja and en, from a
# fresh copy of WORK/LANG/pristine, into WORK/snaps/LANG/NAME-NNN.{ppm,cells}.
# $SHOOT_LIBS, when set, is the LD_LIBRARY_PATH. Here and on the build host.
cat >"$WORK/shoot.sh" <<'SH'
#!/bin/bash
set -euo pipefail
WORK=$1 LIST=$2 SIZE=$3 NOW=$4; shift 4
mkdir -p "$WORK/tmp"
for lang in ja en; do
  home="$WORK/$lang/home"
  case $lang in ja) locale=ja_JP.UTF-8 note='要確認' ;; en) locale=en_US.UTF-8 note='Check this' ;; esac
  mkdir -p "$WORK/snaps/$lang"
  while IFS='|' read -r name keys extra; do
    [ -n "$name" ] || continue
    keys=${keys//@NOTE@/$note}
    # Each shot starts afresh: no editor state, no settings file, the files as made.
    rm -rf "$home" "$WORK/cache"
    cp -pR "$WORK/$lang/pristine" "$home"
    touch -r "$WORK/$lang/parent.time" "$WORK/$lang"
    prefix="$WORK/snaps/$lang/$name"
    rm -f "$prefix"-*
    env=(env -i PATH="$PATH" HOME="$home" TZ=UTC TMPDIR="$WORK/tmp" LANG="$locale"
         FDTP_RC_SYSTEM= FDTP_CACHE_DIR="$WORK/cache" FDTP_SNAPSHOT_PREFIX="$prefix" VISUAL=true EDITOR=true)
    [ -z "${SHOOT_LIBS:-}" ] || env+=(LD_LIBRARY_PATH="$SHOOT_LIBS")
    if [ "$extra" = FIRST_RUN ]; then
      # The language picker: no ~/.fdtprc, no FDTP_LANGUAGE (it would answer it).
      env+=(FDTP_FIRST_RUN_TEST=1)
      script="$keys"
    else
      env+=(FDTP_LANGUAGE="$lang")
      [ -z "$extra" ] || env+=("${extra//@WORK@/$WORK}")
      script="$keys SNAP"
    fi
    (cd "$home" && "${env[@]}" "$@" --size "$SIZE" --time "$NOW" --keys "$script" "$home" </dev/null)
    ls "$prefix"-*.ppm >/dev/null
  done <"$LIST"
done
SH

if [ -s "$mac_list" ]; then
  bash "$WORK/shoot.sh" "$WORK" "$mac_list" "$SIZE" "$NOW" "$EXE"
fi

remote_user= remote_host=
if [ -s "$pdf_list" ]; then
  SSH=(ssh -o ConnectTimeout=20 -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
  echo "== the PDF shots on the build host"
  "${SSH[@]}" "$HOST" "rm -rf ~/$REMOTE_DIR/work && mkdir -p ~/$REMOTE_DIR/port/app"
  # tar over ssh (the host need not have rsync); times kept, no Mac metadata.
  put() { COPYFILE_DISABLE=1 tar -c --no-mac-metadata -f - "$@" |
            "${SSH[@]}" "$HOST" "tar -x --warning=no-unknown-keyword -f - -C ~/$REMOTE_DIR"; }
  put -C "$APP" -s '|^\./|fdtp/|' --exclude './build*' --exclude __pycache__ .
  put -C "$REPO" port/app/platform
  put -C "$BUILD" -s '|^manual-shots|work|' --exclude snaps --exclude cache --exclude tmp --exclude 'manual-shots/*/home' manual-shots
  # Built as tools/make_deb.sh does (no RPATH), with the sysroot's libraries at run time.
  "${SSH[@]}" "$HOST" bash -s -- "$REMOTE_DIR" "$SIZE" "$NOW" <<'SH'
set -euo pipefail
R=$HOME/$1
case $(dpkg --print-architecture) in
  amd64) ARCH=amd64 TRIPLET=x86_64-linux-gnu ;;
  arm64) ARCH=arm64 TRIPLET=aarch64-linux-gnu ;;
  armhf) ARCH=armhf TRIPLET=arm-linux-gnueabihf ;;
  *) echo "manual_shots: $(dpkg --print-architecture) is not amd64, arm64 or armhf" >&2; exit 1 ;;
esac
SYSROOT=$HOME/work/sysroot-$ARCH-deb
[ -d "$SYSROOT/usr/lib/$TRIPLET" ] || { echo "manual_shots: no $SYSROOT (tools/make_sysroot.sh -a $ARCH -d)" >&2; exit 1; }
make -C "$R/fdtp" -j"$(nproc)" CROSS="$TRIPLET-" SYSROOT="$SYSROOT" FDTP_RPATH= BUILD="$R/build" all >"$R/build.log" 2>&1 ||
  { grep -E 'error' "$R/build.log" | head -20; echo "manual_shots: the build failed ($R/build.log)" >&2; exit 1; }
SHOOT_LIBS="$SYSROOT/usr/lib/$TRIPLET" bash "$R/work/shoot.sh" "$R/work" "$R/work/pdf.list" "$2" "$3" "$R/build/fdtp" --headless
SH
  for lang in ja en; do
    mkdir -p "$WORK/snaps/$lang"
    while IFS='|' read -r name _; do rm -f "$WORK/snaps/$lang/$name"-*; done <"$pdf_list"
  done
  "${SSH[@]}" "$HOST" "tar -c -f - -C ~/$REMOTE_DIR/work snaps" | tar -x -f - -C "$WORK"
  remote_user=$("${SSH[@]}" "$HOST" id -un)
  remote_host=$("${SSH[@]}" "$HOST" hostname -s)
  if [ "$KEEP" = 0 ]; then "${SSH[@]}" "$HOST" "rm -rf ~/$REMOTE_DIR"; fi
fi

# ---- the pictures ----------------------------------------------------------
taken=0
for lang in ja en; do
  mkdir -p "$DEST/$lang"
  while IFS='|' read -r name _; do
    [ -n "$name" ] || continue
    last=$(ls "$WORK/snaps/$lang/$name"-*.ppm | tail -1)
    python3 -B "$APP/tools/ppm2png.py" "$last" "$DEST/$lang/$name.png" >/dev/null
    taken=$((taken + 1))
  done < <(cat "$mac_list" "$pdf_list")
done

# ---- nothing private on the screens -----------------------------------------
python3 -B - "$WORK" "$(id -un)" "$(hostname -s)" "$HOST" "$remote_user" "$remote_host" <<'PY'
import json, sys
from pathlib import Path
work, names = Path(sys.argv[1]), sys.argv[2:]
words = ['192.' + '168.', '/Us' + 'ers/', '/ho' + 'me/', 'manual-shots'] + names
bad = []
for path in sorted(work.glob("snaps/*/*.cells")):
    rows = json.loads(path.read_text())['rows']
    text = '\n'.join(''.join(chr(c[0]) for c in row if c[3] != 2) for row in rows)
    bad += [f'{path.name}: {w!r}' for w in words if w and w.lower() in text.lower()]
if bad:
    sys.exit('private text on the screen:\n  ' + '\n  '.join(bad))
PY
echo "manual_shots.sh: $taken pictures in $DEST/{ja,en}"
