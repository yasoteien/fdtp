#!/bin/bash
# Run on the build host (amd64 Debian 13 with armhf multiarch). Fetches armhf .debs with apt-get download (no root) and
# unpacks them into a sysroot for the cross build, plus the runtime pieces
# the Pomera lacks (shipped with fdtp under ~/.fdtp/lib and ~/.fdtp/bin).
#   make_sysroot.sh [-a ARCH] [-d] [SYSROOT]
#     -a ARCH   armhf (default), amd64 or arm64: the Debian architecture of
#               the .debs (apt must know it: dpkg --add-architecture)
#     -d        for tools/make_deb.sh: also the runtime libraries of the -dev
#               packages here (without them their .so links are left dangling
#               and the Pomera build links giflib and libwebp statically), so
#               that the .deb's fdtp uses Debian's shared ones
#   default SYSROOT ~/work/sysroot-ARCH (with -d ~/work/sysroot-ARCH-deb)
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail
ARCH=armhf DEB=0
while getopts a:dh opt; do
	case $opt in a) ARCH=$OPTARG ;; d) DEB=1 ;; *) sed -n '2,12p' "$0"; exit 2 ;; esac
done
shift $((OPTIND - 1))
case $ARCH in
armhf) TRIPLET=arm-linux-gnueabihf ;;
amd64) TRIPLET=x86_64-linux-gnu ;;
arm64) TRIPLET=aarch64-linux-gnu ;;
*) echo "make_sysroot: ARCH is armhf, amd64 or arm64" >&2; exit 2 ;;
esac
ROOT=${1:-$HOME/work/sysroot-$ARCH$(if [ "$DEB" = 1 ]; then echo -deb; fi)}
DEBS=$ROOT/debs
mkdir -p "$DEBS"
PKGS=(
	libmupdf-dev libmupdf25.1 libfreetype-dev libfreetype6
	libzip-dev libzip5 zlib1g-dev
	libexpat1-dev libexpat1
	libssl-dev libssl3t64
	libjpeg62-turbo-dev libpng-dev libgif-dev libwebp-dev libsharpyuv-dev
	libuim-dev libuim8 libuim-custom2 libuim-scm0 libgcroots-dev libgcroots0
	libjbig2dec0 libgumbo3 libmujs3 libopenjp2-7 libharfbuzz0b
	libjpeg62-turbo libglib2.0-0t64 libgraphite2-3 libpcre2-8-0
	lhasa liblhasa0 zip unzip
)
[ "$DEB" = 0 ] || PKGS+=(libgif7 libwebp7 libsharpyuv0 libpng16-16t64 zlib1g)
cd "$DEBS"
for p in "${PKGS[@]}"; do
	ls "${p}"_*_"$ARCH".deb "${p}"_*_all.deb >/dev/null 2>&1 || apt-get download "$p:$ARCH" >/dev/null 2>&1 || apt-get download "$p" >/dev/null 2>&1 || echo "make_sysroot: cannot download $p" >&2
done
for d in *.deb; do dpkg-deb -x "$d" "$ROOT"; done
# pkg-config files and .so links point at /usr/...: rewrite to the sysroot.
find "$ROOT/usr/lib/$TRIPLET/pkgconfig" -name '*.pc' -exec sed -i "s|^prefix=/usr|prefix=$ROOT/usr|" {} + 2>/dev/null || true
find "$ROOT/usr/lib/$TRIPLET" -maxdepth 1 -type l | while read -r l; do
	t=$(readlink "$l")
	case $t in /*) ln -sf "$ROOT$t" "$l" ;; esac
done
ls "$ROOT/usr/include" | head -50 | tr '\n' ' '; echo
