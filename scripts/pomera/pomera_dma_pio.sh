#!/bin/sh
# Pomera DM250 with the Debian 13 image for the D250 (ekesete.net): let the
# SDIO Wi-Fi stop using the PL330 DMA (PIO instead), so the DMA's runtime
# PM bug ("dma-pl330 ... Runtime PM usage count underflow", after which the
# SD, the eMMC and the Wi-Fi stop together) is hit far less. The SD and the
# eMMC keep the DMA. dw_mmc then says "Using PIO mode." for the Wi-Fi host.
#
# The kernel's device tree is not a file: U-Boot reads it from the Rockchip
# resource image at 24 MiB into the SD (sector 49152). make_pio.py turns
# the Wi-Fi host's "dmas" and "dma-names" there into NOP tokens (same size).
# It only does so when that 1 MiB is exactly the one of the image (sha256
# ORIG) and the result exactly the known one (WIFI); otherwise nothing is
# written. Nothing else on the SD changes.
#
#   sudo sh pomera_dma_pio.sh check           which version the SD has now
#   sudo sh pomera_dma_pio.sh state           the same as one word: original,
#                                             wifi-pio, pio or unknown
#   sudo sh pomera_dma_pio.sh wifi            save the 1 MiB to the backup directory
#                                             (rsce-backup-*.bin), write the Wi-Fi
#                                             PIO version, then reboot
#   sudo sh pomera_dma_pio.sh restore [FILE]  write the saved 1 MiB back, then reboot
#                                             (FILE: a saved rsce-backup-*.bin;
#                                             default the newest one there)
#   sh pomera_dma_pio.sh backup-dir           print the backup directory
# The backup directory is this script's own, or /var/lib/fdtp when the
# script is under /usr (the .deb puts it in /usr/share/fdtp/pomera, which
# is the package's and not to be written); FDTP_POMERA_BACKUP_DIR replaces it.
# If the Pomera does not boot afterwards: rescue_rsce.sh on another Linux machine.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
DEV=/dev/mmcblk1
SEEK=49152
ORIG=bc0798fb393803cb4368134f121aa6f47ae4d541eb0b85ff03a7d7b6f6770573
WIFI=1a73651d8b786cf347d69e1ec20f661e8b13c396de6de92672bcc7decc541fb2
# The SDIO Wi-Fi, the SD, the eMMC and the UART all in PIO (an earlier
# variant): only recognised, so that restore can undo it.
PIO=c9c32fc1b1f1d88929cfd38049a27e06a385beacfbb5e666b396e59d5f008e90

# Messages: Japanese when the language (FDTP_MSG_LANG, else LC_ALL,
# LC_MESSAGES, LANG) is ja*.
msg_lang=${FDTP_MSG_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}
say() { case $msg_lang in ja*) printf '%s\n' "$1" ;; *) printf '%s\n' "$2" ;; esac; }
die() { say "$1" "$2" >&2; exit 1; }
case $DIR in /usr/*) BACKUP_DIR=/var/lib/fdtp ;; *) BACKUP_DIR=$DIR ;; esac
BACKUP_DIR=${FDTP_POMERA_BACKUP_DIR:-$BACKUP_DIR}
[ "${1:-}" != backup-dir ] || { printf '%s\n' "$BACKUP_DIR"; exit 0; }

[ "$(id -u)" = 0 ] || die "sudo を付けて流してください" "run with sudo"
[ "$(findmnt -n -o SOURCE /)" = ${DEV}p2 ] ||
	die "/ が ${DEV}p2 にありません（DM250 の SD から起動した Debian ではない）: 何もしません" \
		"/ is not on ${DEV}p2 (not Debian booted from the DM250's SD): nothing done"
sum() { sha256sum "$1" | cut -d' ' -f1; }
now() { dd if=$DEV bs=512 skip=$SEEK count=2048 iflag=direct 2>/dev/null | sha256sum | cut -d' ' -f1; }
state() { case $(now) in $ORIG) echo original ;; $WIFI) echo wifi-pio ;; $PIO) echo pio ;; *) echo unknown ;; esac; }
write() { # FILE SHA256
	dd if="$1" of=$DEV bs=512 seek=$SEEK count=2048 conv=fsync,notrunc 2>/dev/null
	sync
	[ "$(now)" = "$2" ] || die "書いた後の読み直しが合いません。再起動せずに、すぐ restore を流してください" \
		"read back differs: run restore now, do not reboot"
}

case ${1:-check} in
check)
	say "SD の 24 MiB の所: $(state)" "SD region at 24 MiB: $(state)"
	say "元の 1 MiB の保存先: $BACKUP_DIR" "Backup directory: $BACKUP_DIR"
	echo "kernel: $(journalctl -k -b 0 -q --no-pager 2>/dev/null | grep -E 'dwmmc_rockchip.*(PIO|DMA)' | sed 's/.*dwmmc_rockchip //' | tr '\n' ' ')"
	echo "underflow since boot: $(journalctl -k -b 0 -q --no-pager 2>/dev/null | grep -c 'usage count underflow' || true)"
	;;
state) state ;;
wifi)
	case $(state) in
	wifi-pio) say "既に Wi-Fi の PIO 版です。何もしません。" "Already the Wi-Fi PIO version; nothing done."; exit 0 ;;
	pio) die "全部を PIO にした版です。先に restore で元に戻してください: 何もしません" \
		"This is the all-PIO version; run restore first: nothing done" ;;
	unknown) die "SD の 24 MiB の所が、既知の Debian 13 イメージ（D250 用）の物ではありません: 何もしません" \
		"The SD's region at 24 MiB is not the known Debian 13 image (D250): nothing done" ;;
	esac
	command -v python3 >/dev/null || die "python3 がありません: 何もしません" "python3 is missing: nothing done"
	mkdir -p "$BACKUP_DIR"
	backup=$BACKUP_DIR/rsce-backup-$(date +%Y%m%d%H%M%S).bin
	dd if=$DEV bs=512 skip=$SEEK count=2048 iflag=direct of="$backup.tmp" 2>/dev/null
	[ "$(sum "$backup.tmp")" = "$ORIG" ] || { rm -f "$backup.tmp"; die "保存した 1 MiB が元の物と合いません: 何もしません" \
		"the saved 1 MiB is not the original: nothing done"; }
	mv "$backup.tmp" "$backup"
	say "元の 1 MiB を保存しました: $backup" "Saved the original 1 MiB: $backup"
	out=$BACKUP_DIR/rsce-wifi-pio.bin
	python3 "$DIR/make_pio.py" "$backup" "$out" || die "Wi-Fi の PIO 版を作れませんでした: SD には何も書いていません" \
		"could not make the Wi-Fi PIO version: nothing written to the SD"
	[ "$(sum "$out")" = "$WIFI" ] || die "作った物が既知の Wi-Fi の PIO 版と合いません: SD には何も書いていません" \
		"the result is not the known Wi-Fi PIO version: nothing written to the SD"
	write "$out" "$WIFI"
	rm -f "$out"
	say "書いて読み直しました。再起動すると Wi-Fi が PIO になります。戻すには: sudo sh $DIR/pomera_dma_pio.sh restore" \
		"Written and read back. Reboot to use it. To undo: sudo sh $DIR/pomera_dma_pio.sh restore"
	;;
restore)
	src=${2:-}
	if [ -z "$src" ]; then
		# The names sort by time: the last original one is the newest.
		for f in "$BACKUP_DIR"/rsce-backup-*.bin; do
			if [ -f "$f" ] && [ "$(sum "$f")" = "$ORIG" ]; then src=$f; fi
		done
		[ -n "$src" ] || die "$BACKUP_DIR に元の 1 MiB（rsce-backup-*.bin）がありません: 何もしません" \
			"no saved original (rsce-backup-*.bin) in $BACKUP_DIR: nothing done"
	fi
	{ [ -f "$src" ] && [ "$(sum "$src")" = "$ORIG" ]; } || die "$src は元の 1 MiB ではありません: 何もしません" \
		"$src is not the original 1 MiB: nothing done"
	case $(state) in
	original) say "既に元のままです。何もしません。" "Already the original; nothing done."; exit 0 ;;
	unknown) die "SD の 24 MiB の所が既知の物ではありません: 何もしません" \
		"The SD's region at 24 MiB is not a known one: nothing done" ;;
	esac
	write "$src" "$ORIG"
	say "元に戻して読み直しました（${src}）。再起動してください。" "Restored and read back ($src). Reboot to use it."
	;;
*) sed -n '2,28p' "$0"; exit 2 ;;
esac
