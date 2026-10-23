#!/bin/bash
# Mac -> build host (armhf cross build) -> Pomera.
# Sends the Mac working tree (fdtp/ and port/app/platform/; all of the public
# copy), uncommitted changes too, builds on the build host (amd64 Debian 13 with
# gcc-arm-linux-gnueabihf) with the sysroot of make_sysroot.sh and
# installs on the Pomera:
#   ~/.fdtp/bin/fdtp       the program (the previous one kept as fdtp.prev;
#                          fdtp.new is rsync's copy of the latest build)
#   ~/.fdtp/lib/           libraries the Pomera lacks (libmupdf and its own)
#   ~/.fdtp/bin/zip lhasa  commands the Pomera lacks (fdtp puts ~/.fdtp/bin first in PATH)
#   ~/bin/fdtp             link to ~/.fdtp/bin/fdtp (~/bin is on the PATH)
#   ~/.local/share/applications/fdtp.desktop, ~/.local/share/icons/hicolor/
#                          NxN/apps/fdtp.png   Xfce's menu entry and icon
# Does not start the app.
#   deploy.sh [-j JOBS]
# ssh names: FDTP_BUILD_HOST (default build-host) and FDTP_POMERA_HOST
# (default pomera), from the environment or from the KEY=VALUE lines of
# $FDTP_DEV_ENV (default ~/.config/fdtp-dev/env; tests/dev_env.py reads it too).
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail
JOBS=6
while getopts j:h opt; do
	case $opt in j) JOBS=$OPTARG ;; *) sed -n '2,18p' "$0"; exit 2 ;; esac
done
DEV_ENV=${FDTP_DEV_ENV:-$HOME/.config/fdtp-dev/env}
if [ -f "$DEV_ENV" ]; then
	while IFS='=' read -r key value; do
		case $key in FDTP_*_HOST) [ -n "${!key:-}" ] || printf -v "$key" '%s' "$value" ;; esac
	done < "$DEV_ENV"
fi
BUILD_HOST=${FDTP_BUILD_HOST:-build-host}
POMERA_HOST=${FDTP_POMERA_HOST:-pomera}
BUILD_DIR='~/work/fdtp-src'
SYSROOT='~/work/sysroot-armhf'
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
# The private tree (fdtp/ beside port/app/platform/) or the public copy
# (tools/make_public.sh: the app at the top, data/icons).
if [ -f fdtp/Makefile ]; then APP=fdtp SEND="fdtp port/app/platform" ICONS=fdtp/icons
else APP=. SEND=. ICONS=data/icons
fi

echo "== send $SEND to $BUILD_HOST:$BUILD_DIR"
{ git ls-files -z --cached --others --exclude-standard $SEND |
	xargs -0 -n 200 sh -c 'for f; do [ -f "$f" ] && printf "%s\0" "$f"; done' sh; } |
	COPYFILE_DISABLE=1 tar --null -T - -czf - |
	ssh "$BUILD_HOST" "mkdir -p $BUILD_DIR && tar -C $BUILD_DIR -m -xzf - 2>/dev/null"

echo "== cross build on $BUILD_HOST"
if ! ssh "$BUILD_HOST" "make -C $BUILD_DIR/$APP -j$JOBS CROSS=arm-linux-gnueabihf- all >$BUILD_DIR/cross-build.log 2>&1"; then
	ssh "$BUILD_HOST" "grep -E 'error|warning' $BUILD_DIR/cross-build.log | head -30; tail -5 $BUILD_DIR/cross-build.log"
	echo "deploy: build failed" >&2
	exit 1
fi
echo "warnings: $(ssh "$BUILD_HOST" "grep -c 'warning:' $BUILD_DIR/cross-build.log || true")"
BIN=$BUILD_DIR/$APP/build-arm-linux-gnueabihf/fdtp
ssh "$BUILD_HOST" "file $BIN" | grep -q 'ARM, EABI5' || { echo "deploy: $BIN is not armhf" >&2; exit 1; }

echo "== install on $POMERA_HOST:~/.fdtp"
LIBS="libmupdf.so.25.1 libjbig2dec.so.0 libgumbo.so.3 libmujs.so.3 liblhasa.so.0"
# build host -> Mac (wired), then Mac -> Pomera with rsync at ${RATE_KB:-200} KB/s:
# a fast 70 MB transfer froze the Pomera's SDIO Wi-Fi (kernel "dma-pl330
# Runtime PM usage count underflow" right before). rsync skips what the
# Pomera already has (checksums) and resumes a cut transfer (--partial).
DIST=$APP/build/dist
rm -rf "$DIST" && mkdir -p "$DIST/bin" "$DIST/lib" "$DIST/libexec"
ssh "$BUILD_HOST" "tar -chf - -C $BUILD_DIR/$APP/build-arm-linux-gnueabihf fdtp -C $SYSROOT/usr/lib/arm-linux-gnueabihf $LIBS -C $SYSROOT/usr/bin zip lhasa" |
	tar -C "$DIST" -xf -
mv "$DIST/fdtp" "$DIST/bin/fdtp.new"; mv "$DIST/zip" "$DIST/bin/"; mv "$DIST/lhasa" "$DIST/libexec/"
for f in $LIBS; do mv "$DIST/$f" "$DIST/lib/"; done
# Xfce's menu entry and its icon (revision 1 item 23): ~/.fdtp/share, then
# copied into ~/.local/share below. The PNGs come from the same script as
# the window's icon (gen_icon.py; $ICONS has the committed copies).
mkdir -p "$DIST/share"
python3 $APP/tools/gen_icon.py --png-dir "$DIST/share"
cp $ICONS/fdtp.desktop "$DIST/share/"
rsync -rltp -c --partial --bwlimit="${RATE_KB:-200}" \
	"$DIST/" "$POMERA_HOST:.fdtp/"
ssh "$POMERA_HOST" 'set -e; d=$HOME/.fdtp; mkdir -p $HOME/bin; rm -rf $d/stage
	# lhasa has no RPATH: the real one in libexec, a wrapper on the PATH.
	printf "#!/bin/sh\nLD_LIBRARY_PATH=\"\$HOME/.fdtp/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\" exec \"\$HOME/.fdtp/libexec/lhasa\" \"\$@\"\n" > $d/bin/lhasa
	chmod +x $d/bin/lhasa
	if ! cmp -s $d/bin/fdtp.new $d/bin/fdtp; then
		if [ -e $d/bin/fdtp ]; then cp -p $d/bin/fdtp $d/bin/fdtp.prev; fi
		cp -p $d/bin/fdtp.new $d/bin/fdtp.tmp && mv -f $d/bin/fdtp.tmp $d/bin/fdtp
	fi
	ln -sfn $d/bin/fdtp $HOME/bin/fdtp
	# The menu entry (Exec: this home) and the icon in every size.
	icons=$HOME/.local/share/icons/hicolor
	for s in 16 32 48 64 128; do
		mkdir -p $icons/${s}x${s}/apps
		cp -p $d/share/fdtp-$s.png $icons/${s}x${s}/apps/fdtp.png
	done
	touch $icons
	if [ -f $icons/icon-theme.cache ] && command -v gtk-update-icon-cache >/dev/null; then
		gtk-update-icon-cache -f -t $icons >/dev/null 2>&1 || true
	fi
	mkdir -p $HOME/.local/share/applications
	sed "s|@HOME@|$HOME|g" $d/share/fdtp.desktop > $HOME/.local/share/applications/fdtp.desktop.tmp
	mv -f $HOME/.local/share/applications/fdtp.desktop.tmp $HOME/.local/share/applications/fdtp.desktop
	ls -l $d/bin $d/lib $d/libexec $d/share | sed "s/^/  /"; $d/bin/fdtp -v'
