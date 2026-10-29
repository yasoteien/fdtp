#!/bin/bash
# Makes FDTP's .deb for Debian 13 with dpkg-deb --build (no debhelper), on a
# Debian build host. For the Releases page: sudo apt install ./fdtp_VERSION_ARCH.deb
#
#   tools/make_deb.sh [-b BINARY] [-s SYSROOT] [-o DIR] [-j JOBS] ARCH
#     ARCH        armhf, amd64 or arm64
#     -b BINARY   the fdtp to package. Default: build-deb-TRIPLET/fdtp, made
#                 (or brought up to date) here first with
#                 make CROSS=TRIPLET- SYSROOT=SYSROOT FDTP_RPATH= BUILD=build-deb-TRIPLET
#                 (no RPATH: the libraries are Debian's, not ~/.fdtp/lib)
#     -s SYSROOT  made by tools/make_sysroot.sh -a ARCH -d (default
#                 ~/work/sysroot-ARCH-deb): for the build, and its debs/ for
#                 finding the packages of libraries not installed here
#     -o DIR      where the .deb goes (default build/deb)
#
# The version is FDTP_VERSION of src/version.h; the Debian revision and the
# Maintainer come from the top entry of packaging/debian/changelog, whose
# version must be FDTP_VERSION-N (add an entry when the version changes).
# fdtp-localsend beside BINARY is packaged too when present (OpenSSL is
# optional at build time). Depends covers both programs' NEEDED libraries.
# Depends: one package for each NEEDED library of the ELF, found with
# dpkg -S (installed here for ARCH) or in SYSROOT/debs; unversioned (the
# .deb is for Debian 13 only). It stops when a library has no package, when
# the binary has an RPATH, or when it has one of the libraries linked in
# statically (giflib and libwebp are, in a build against a sysroot made
# without -d).
#
# Installs: /usr/bin/fdtp; /usr/share/applications/fdtp.desktop;
# /usr/share/icons/hicolor/NxN/apps/fdtp.png; /usr/share/doc/fdtp/ (README,
# NOTICE, copyright with the AGPL text, changelog.Debian.gz, the manual's
# PDFs when there are any); /usr/share/fdtp/pomera/ (the Pomera scripts) and
# /usr/sbin/fdtp-pomera-setup (pomera/pomera_setup.sh).
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail
OUTDIR= BINARY= SYSROOT= JOBS=4
while getopts b:s:o:j:h opt; do
	case $opt in
	b) BINARY=$OPTARG ;; s) SYSROOT=$OPTARG ;; o) OUTDIR=$OPTARG ;; j) JOBS=$OPTARG ;;
	*) sed -n '2,30p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# = 1 ] || { sed -n '2,30p' "$0" >&2; exit 2; }
ARCH=$1
case $ARCH in
armhf) TRIPLET=arm-linux-gnueabihf MACHINE=ARM CLASS=ELF32 ;;
amd64) TRIPLET=x86_64-linux-gnu MACHINE='X86-64' CLASS=ELF64 ;;
arm64) TRIPLET=aarch64-linux-gnu MACHINE=AArch64 CLASS=ELF64 ;;
*) echo "make_deb: ARCH is armhf, amd64 or arm64" >&2; exit 2 ;;
esac
die() { echo "make_deb: $*" >&2; exit 1; }
for t in dpkg-deb dpkg gzip md5sum "$TRIPLET-readelf" "$TRIPLET-nm" "$TRIPLET-strip"; do
	command -v "$t" >/dev/null || die "$t is missing (binutils for $TRIPLET, dpkg)"
done

# The private tree (fdtp/ with pomera/, icons/, public/) or the public copy
# (scripts/pomera, data/icons, README.md at the top).
APP=$(cd "$(dirname "$0")/.." && pwd)
# The first of the paths that is there (nothing, and status 0, if none).
first() { for p; do if [ -e "$p" ]; then printf '%s\n' "$p"; return 0; fi; done; return 0; }
PDIR=$(first "$APP/scripts/pomera" "$APP/pomera")
ICONS=$(first "$APP/data/icons" "$APP/icons")
PKGDIR=$APP/packaging/debian
[ -n "$PDIR" ] && [ -n "$ICONS" ] && [ -f "$PKGDIR/control.in" ] || die "$APP: pomera/, icons/ or packaging/debian/ missing"
SYSROOT=${SYSROOT:-$HOME/work/sysroot-$ARCH-deb}
OUTDIR=${OUTDIR:-$APP/build/deb}

# ---- version --------------------------------------------------------------------
VERSION=$(sed -n 's/^#define FDTP_VERSION "\(.*\)"/\1/p' "$APP/src/version.h")
[ -n "$VERSION" ] || die "no FDTP_VERSION in src/version.h"
DEBVER=$(sed -n '1s/^fdtp (\([^)]*\)).*/\1/p' "$PKGDIR/changelog")
[ "${DEBVER%-*}" = "$VERSION" ] && [ "$DEBVER" != "$VERSION" ] ||
	die "packaging/debian/changelog's top entry is ${DEBVER:-none}, not $VERSION-N: add an entry for $VERSION"
MAINTAINER=$(sed -n 's/^ -- \(.*>\)  .*/\1/p' "$PKGDIR/changelog" | head -1)
[ -n "$MAINTAINER" ] || die "no ' -- Name <email>  date' line in packaging/debian/changelog"

# ---- the binary ---------------------------------------------------------------------
if [ -z "$BINARY" ]; then
	[ -d "$SYSROOT/usr/lib/$TRIPLET" ] || die "no sysroot $SYSROOT: run tools/make_sysroot.sh -a $ARCH -d"
	BUILD=$APP/build-deb-$TRIPLET
	echo "== build $BUILD/fdtp"
	mkdir -p "$BUILD"
	make -C "$APP" -j"$JOBS" CROSS="$TRIPLET-" SYSROOT="$SYSROOT" FDTP_RPATH= BUILD="$BUILD" all >"$BUILD/build.log" 2>&1 ||
		{ grep -E 'error|warning' "$BUILD/build.log" | head -20; die "the build failed (see $BUILD/build.log)"; }
	echo "  warnings: $(grep -c 'warning:' "$BUILD/build.log" || true)"
	BINARY=$BUILD/fdtp
fi
[ -f "$BINARY" ] || die "no $BINARY"
LOCALSEND_BINARY=$(dirname "$BINARY")/fdtp-localsend
BINARIES=("$BINARY")
[ ! -f "$LOCALSEND_BINARY" ] || BINARIES+=("$LOCALSEND_BINARY")
NEEDED=
for elf in "${BINARIES[@]}"; do
head=$(LC_ALL=C "$TRIPLET-readelf" -h "$elf") || die "$elf is not an ELF file"
printf '%s\n' "$head" | grep -q "Class:.*$CLASS" && printf '%s\n' "$head" | grep -q "Machine:.*$MACHINE" ||
	die "$elf is not a $ARCH program"
dynamic=$(LC_ALL=C "$TRIPLET-readelf" -d "$elf")
! printf '%s\n' "$dynamic" | grep -q -E '\((RPATH|RUNPATH)\)' ||
	die "$elf has an RPATH (build it with FDTP_RPATH=)"
# Libraries linked in statically (a .so link of the sysroot left dangling):
# Depends and copyright would miss them.
static=
for s in DGifOpenFileName:giflib WebPDecode:libwebp png_create_read_struct:libpng jpeg_CreateDecompress:libjpeg \
	inflate:zlib XML_ParserCreate:expat fz_new_context_imp:mupdf uim_init:uim drmModeGetResources:libdrm \
	SSL_new:openssl EVP_DigestInit_ex:openssl; do
	"$TRIPLET-nm" --defined-only "$elf" 2>/dev/null | grep -q " [TtWw] ${s%%:*}$" && static="$static ${s#*:}"
done
[ -z "$static" ] || die "$elf has these linked in statically:$static (make the sysroot with make_sysroot.sh -d)"
needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')
[ -n "$needed" ] || die "$elf has no NEEDED libraries"
NEEDED="$NEEDED $needed"
done

# ---- Depends from NEEDED ---------------------------------------------------------------
NEEDED=$(printf '%s\n' $NEEDED | sort -u)
INDEX=
if [ -d "$SYSROOT/debs" ]; then
	# path<TAB>package for the libraries in the sysroot's .debs of ARCH.
	INDEX=$(for d in "$SYSROOT"/debs/*_"$ARCH".deb; do
		[ -f "$d" ] || continue
		p=$(dpkg-deb -f "$d" Package)
		dpkg-deb -c "$d" | awk -v p="$p" '{ f = $6; sub(/^\./, "", f); print f "\t" p }'
	done)
fi
package_of() { # SONAME -> the Debian package of ARCH that has it
	local so=$1 hit
	hit=$(dpkg -S "*/$TRIPLET/$so" "/usr/lib/$so" "/lib/$so" "/usr/lib64/$so" "/lib64/$so" 2>/dev/null |
		sed -n "s|^\([^:, ]*\):$ARCH: \(.*\)$|\1 \2|p" |
		awk -v so="$so" -v t="$TRIPLET" '$2 ~ ("/" t "/" so "$") || $2 == "/usr/lib/" so || $2 == "/lib/" so || $2 == "/usr/lib64/" so || $2 == "/lib64/" so { print $1 }' | sort -u)
	if [ -z "$hit" ] && [ -n "$INDEX" ]; then
		hit=$(printf '%s\n' "$INDEX" | awk -F'\t' -v so="$so" -v t="$TRIPLET" \
			'$1 == "/usr/lib/" t "/" so || $1 == "/lib/" t "/" so || $1 == "/usr/lib/" so || $1 == "/lib/" so { print $2 }' | sort -u)
	fi
	[ "$(printf '%s\n' "$hit" | grep -c .)" = 1 ] || { echo "make_deb: the package of $so: ${hit:-not found}" >&2; return 1; }
	printf '%s\n' "$hit"
}
echo "== Depends (NEEDED -> package)"
PKGS=
for so in $NEEDED; do
	p=$(package_of "$so") || die "cannot tell which package has $so (install it for $ARCH, or add its package to the sysroot)"
	printf '  %-24s %s\n' "$so" "$p"
	PKGS="$PKGS $p"
done
DEPENDS=$(printf '%s\n' $PKGS | sort -u | paste -sd, - | sed 's/,/, /g')
# Each must be a package of Debian 13 for ARCH (apt's lists; not installed).
if dpkg --print-architecture | grep -qx "$ARCH" || dpkg --print-foreign-architectures | grep -qx "$ARCH"; then
	for p in $(printf '%s\n' $PKGS | sort -u) zip unzip lhasa; do
		apt-cache show "$p:$ARCH" >/dev/null 2>&1 || die "apt does not know $p for $ARCH"
	done
	echo "  apt knows all of them for $ARCH"
else
	echo "  (apt has no lists for $ARCH here: the packages were not checked against them)"
fi

# ---- the tree ---------------------------------------------------------------------------
NAME=fdtp_${DEBVER}_$ARCH
STAGE=$OUTDIR/stage/$NAME
umask 022
rm -rf "$STAGE" && mkdir -p "$STAGE/DEBIAN" && chmod 755 "$STAGE"
inst() { install -D -m "$1" "$2" "$STAGE$3"; }
inst 755 "$BINARY" /usr/bin/fdtp
"$TRIPLET-strip" --strip-unneeded --remove-section=.comment --remove-section=.note "$STAGE/usr/bin/fdtp"
if [ -f "$LOCALSEND_BINARY" ]; then
	inst 755 "$LOCALSEND_BINARY" /usr/bin/fdtp-localsend
	"$TRIPLET-strip" --strip-unneeded --remove-section=.comment --remove-section=.note "$STAGE/usr/bin/fdtp-localsend"
fi
mkdir -p "$STAGE/usr/share/applications"
sed 's|^Exec=.*|Exec=/usr/bin/fdtp|' "$ICONS/fdtp.desktop" > "$STAGE/usr/share/applications/fdtp.desktop"
chmod 644 "$STAGE/usr/share/applications/fdtp.desktop"
for s in 16 32 48 64 128; do inst 644 "$ICONS/fdtp-$s.png" "/usr/share/icons/hicolor/${s}x${s}/apps/fdtp.png"; done
DOC=/usr/share/doc/fdtp
for n in README.md README.en.md NOTICE.md; do
	f=$(first "$APP/public/$n" "$APP/$n")
	[ -n "$f" ] && inst 644 "$f" "$DOC/$n"
done
MANUAL=$(first "$APP/docs/manual" "$APP/public/manual")
if [ -n "$MANUAL" ]; then
	for f in "$MANUAL"/*/*.pdf; do [ -f "$f" ] && inst 644 "$f" "$DOC/$(basename "$(dirname "$f")")/$(basename "$f")"; done
fi
LICENSE=$(first "$APP/public/LICENSE" "$APP/LICENSE")
[ -n "$LICENSE" ] || die "no LICENSE"
# The AGPL text as the body of the License paragraph (DEP-5: a space before
# each line, " ." for an empty one).
awk -v lic="$LICENSE" '
	$0 == "@AGPL_TEXT@" { while ((getline l < lic) > 0) { sub(/[ \t\r]+$/, "", l); print (l == "" ? " ." : " " l) } next }
	{ print }' "$PKGDIR/copyright" > "$STAGE$DOC/copyright"
gzip -9n -c "$PKGDIR/changelog" > "$STAGE$DOC/changelog.Debian.gz"
chmod 644 "$STAGE$DOC/copyright" "$STAGE$DOC/changelog.Debian.gz"
# The Pomera scripts; those starting with #! executable.
for f in "$PDIR"/*.sh "$PDIR"/*.py; do
	b=$(basename "$f")
	[ "$b" = pomera_setup.sh ] && continue
	if [ "$(head -c 2 "$f")" = '#!' ]; then m=755; else m=644; fi
	inst "$m" "$f" "/usr/share/fdtp/pomera/$b"
done
inst 755 "$PDIR/pomera_setup.sh" /usr/sbin/fdtp-pomera-setup
for s in postinst prerm; do install -m 755 "$PKGDIR/$s" "$STAGE/DEBIAN/$s"; done
SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
sed -e "s|@VERSION@|$DEBVER|" -e "s|@ARCH@|$ARCH|" -e "s|@MAINTAINER@|$MAINTAINER|" \
	-e "s|@INSTALLED_SIZE@|$SIZE|" -e "s|@DEPENDS@|$DEPENDS|" "$PKGDIR/control.in" > "$STAGE/DEBIAN/control"
grep -q '@[A-Z_]*@' "$STAGE/DEBIAN/control" && die "control: a @NAME@ left"
(cd "$STAGE" && find usr -type f | LC_ALL=C sort | xargs md5sum) > "$STAGE/DEBIAN/md5sums"
chmod 644 "$STAGE/DEBIAN/control" "$STAGE/DEBIAN/md5sums"

mkdir -p "$OUTDIR"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUTDIR/$NAME.deb" >/dev/null
echo "== $OUTDIR/$NAME.deb ($(du -k "$OUTDIR/$NAME.deb" | cut -f1) KB)"
dpkg-deb -f "$OUTDIR/$NAME.deb" Package Version Architecture Installed-Size Depends Recommends Suggests | sed 's/^/  /'
