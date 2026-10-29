#!/bin/bash
# The manual's pictures of the browser download page, as a phone shows it:
# docs/media/fdtp/manual/{ja,en}/26-browser-pin.png (the PIN page) and
# 27-browser-list.png (the files after the PIN), 390 x 540 at 2x.
#
#   fdtp/tools/manual_browser_shots.sh
#
# Runs the Mac's build/fdtp-localsend serve on 127.0.0.1 with the files of
# screen 25 (readme.txt, report.docx; their content does not show) and takes
# the pages with Chrome headless. The name shown is pomera; no address is in
# the page itself. Work in fdtp/build/manual-browser (not /tmp).
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail
APP=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$APP/.." && pwd)
DEST=$REPO/docs/media/fdtp/manual
WORK=$APP/build/manual-browser
EXE=$APP/build/fdtp-localsend
CHROME=${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}
PORT=53391 PIN=482913
[ -x "$EXE" ] || { echo "manual_browser_shots.sh: make first ($EXE)" >&2; exit 2; }
[ -x "$CHROME" ] || { echo "manual_browser_shots.sh: no Chrome ($CHROME)" >&2; exit 2; }
rm -rf "$WORK" && mkdir -p "$WORK/files" "$WORK/home"
printf 'readme\n' >"$WORK/files/readme.txt"
head -c 18432 /dev/zero >"$WORK/files/report.docx"
HOME=$WORK/home "$EXE" serve --port $PORT --alias pomera --pin $PIN -- "$WORK/files/readme.txt" "$WORK/files/report.docx" \
  >"$WORK/serve.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q '^ready' "$WORK/serve.log" && break; sleep 0.1; done
# Chrome headless has a minimum window width: the page goes in a 390 x 540
# frame in the middle of a wider window, and sips cuts out the middle.
shoot() {   # LANG NAME URL
  mkdir -p "$DEST/$1"
  printf '<!doctype html><body style="margin:0;height:100vh;display:flex;align-items:center;justify-content:center"><iframe src="%s" style="border:0;width:390px;height:540px;display:block"></iframe>' \
    "$3" >"$WORK/frame.html"
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --accept-lang="$1" --lang="$1" --user-data-dir="$WORK/chrome-$1" \
    --window-size=800,900 --force-device-scale-factor=2 --screenshot="$WORK/full.png" "file://$WORK/frame.html" >/dev/null 2>&1
  sips -c 1080 780 "$WORK/full.png" --out "$DEST/$1/$2.png" >/dev/null
}
for lang in ja en; do
  shoot $lang 26-browser-pin "http://127.0.0.1:$PORT/"
  shoot $lang 27-browser-list "http://127.0.0.1:$PORT/?pin=$PIN"
done
echo "manual_browser_shots.sh: 4 pictures in $DEST/{ja,en}"
