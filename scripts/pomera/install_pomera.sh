#!/bin/sh
# The Pomera DM250 part of `sudo make install` (mk/install.mk runs it with
# the answers of ./configure) and of fdtp-pomera-setup (pomera_setup.sh):
#   POMERA_STABILITY=yes    pomera_stability.sh install, then (asked again,
#                           default no, only on a terminal) pomera_dma_pio.sh wifi
#   POMERA_INTERNAL=ro     pomera_mount_internal.sh persist
#   POMERA_INTERNAL=rw     pomera_mount_internal.sh persist rw
#   POMERA_INTERNAL=no      pomera_mount_internal.sh unpersist (a mount set up
#                           by an earlier install is removed)
#   POMERA_INTERNAL=keep    nothing (the question was skipped)
#   POMERA_INTERNAL_RO=yes is accepted as the legacy equivalent of ro.
# FDTP_MSG_LANG=ja|en picks the messages (default: LC_ALL, LC_MESSAGES, LANG).
# Does nothing when the machine is not a DM250 (/proc/device-tree's
# compatible or model has no "dm250"; FDTP_CONFIGURE_DT replaces
# /proc/device-tree, for the tests).
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
STABILITY=${POMERA_STABILITY:-no}
INTERNAL=${POMERA_INTERNAL:-}
if [ -z "$INTERNAL" ]; then
	if [ "${POMERA_INTERNAL_RO:-no}" = yes ]; then INTERNAL=ro; else INTERNAL=keep; fi
fi
DT=${FDTP_CONFIGURE_DT:-/proc/device-tree}
msg_lang=${FDTP_MSG_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}
case $msg_lang in auto) msg_lang=${LC_ALL:-${LC_MESSAGES:-${LANG:-}}} ;; esac
say() { case $msg_lang in ja*) printf '%s\n' "$1" ;; *) printf '%s\n' "$2" ;; esac; }

[ "$STABILITY" = yes ] || [ "$INTERNAL" = ro ] || [ "$INTERNAL" = rw ] || [ "$INTERNAL" = no ] || exit 0
model=$(cat "$DT/compatible" "$DT/model" 2>/dev/null | tr '\0' ' ' || true)
case $(printf '%s' "$model" | tr 'A-Z' 'a-z') in
*dm250*) ;;
*) say "Pomera DM250 ではないので、Pomera の設定は飛ばします。" "Not a Pomera DM250: the Pomera settings are skipped."; exit 0 ;;
esac
[ "$(id -u)" = 0 ] || { say "Pomera の設定には root が要ります（sudo make install）" "The Pomera settings need root (sudo make install)" >&2; exit 1; }

reboot=no
if [ "$STABILITY" = yes ]; then
	say "== Wi-Fi の安定化 1/2: 省電力を切る・watchdog ==" "== Wi-Fi stability 1/2: power saving off, watchdog =="
	if sh "$DIR/pomera_stability.sh" install; then reboot=yes
	else say "省電力・watchdog の設定は行いませんでした（上の理由）。" "Power saving and the watchdog were not set up (see above)."; fi
	say "== Wi-Fi の安定化 2/2: Wi-Fi だけ DMA をやめて PIO にする ==" "== Wi-Fi stability 2/2: the Wi-Fi in PIO instead of DMA =="
	say "SD の 24 MiB の所の 1 MiB（カーネルのデバイスツリー）を書き換えます。" \
		"This rewrites the 1 MiB at 24 MiB into the SD (the kernel's device tree)."
	say "  - ekesete.net の Debian 13 イメージ（D250 用）の SD に限ります。中身が違えば何もしません。" \
		"  - Only for an SD with the Debian 13 image for the D250 (ekesete.net); with anything else nothing is done."
	backup=$(sh "$DIR/pomera_dma_pio.sh" backup-dir)
	say "  - 書き換える前に元の 1 MiB を $backup/rsce-backup-*.bin に保存します。" \
		"  - The original 1 MiB is saved to $backup/rsce-backup-*.bin first."
	say "    戻すには: sudo sh $DIR/pomera_dma_pio.sh restore" \
		"    To undo: sudo sh $DIR/pomera_dma_pio.sh restore"
	say "  - 起動しなくなった時は、別の Linux 機で $DIR/rescue_rsce.sh を使います。" \
		"  - If the Pomera no longer boots, use $DIR/rescue_rsce.sh on another Linux machine."
	answer=
	if [ -t 0 ]; then
		say "SD の 24 MiB の所を書き換えます。よろしいですか [y/N]" "Rewrite the SD at 24 MiB? [y/N]"
		read -r answer || answer=
	else
		say "端末ではないので、書き換えは行いません。" "Not on a terminal: the SD is not rewritten."
	fi
	case $answer in
	y|Y|yes|Yes|YES|はい) sh "$DIR/pomera_dma_pio.sh" wifi || say "PIO の書き換えは行いませんでした（上の理由）。" "The PIO change was not made (see above)." ;;
	*) say "書き換えは行いませんでした。あとで行うには: sudo sh $DIR/pomera_dma_pio.sh wifi" \
		"The SD was not rewritten. To do it later: sudo sh $DIR/pomera_dma_pio.sh wifi" ;;
	esac
fi
case $INTERNAL in
ro)
	say "== 内蔵メモリの読み取り専用マウント ==" "== Read-only mount of the internal memory =="
	sh "$DIR/pomera_mount_internal.sh" persist || say "マウントの設定は行いませんでした（上の理由）。" "The mount was not set up (see above)."
	;;
rw)
	say "== 内蔵メモリの読み書きマウント ==" "== Read-write mount of the internal memory =="
	sh "$DIR/pomera_mount_internal.sh" persist rw || say "マウントの設定は行いませんでした（上の理由）。" "The mount was not set up (see above)."
	;;
no)
	# "Do not mount" this time: a mount an earlier install set up is removed.
	if sh "$DIR/pomera_mount_internal.sh" persisted 2>/dev/null; then
		say "== 内蔵メモリのマウントをやめる ==" "== Removing the internal memory mount =="
		sh "$DIR/pomera_mount_internal.sh" unpersist || true
	fi
	;;
esac
[ "$reboot" = no ] || say "再起動してください（sudo reboot）。" "Please reboot (sudo reboot)."
