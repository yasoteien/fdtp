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
#   POMERA_PDCHARGE=yes|no|keep  install / remove / leave pdcharge unchanged
#   POMERA_CVE=yes|no|keep  install / remove / leave the CVE mitigation unchanged
#   POMERA_WIFI_PS=off|on|keep  install-ps / remove-ps / leave unchanged
#   POMERA_CLEAN_OLD=yes    remove udev/watchdog, pdcharge and CVE workarounds
#   POMERA_RTC=yes          timedatectl set-local-rtc 0 (no RTC clock write)
#   POMERA_KERNEL_APT=yes|source|no|keep  install / source / leave unchanged
# Own kernels skip legacy installs, even with stale answers.
# FDTP_CONFIGURE_OWNKERNEL=yes|no overrides kernel detection for tests.
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
own_kernel() {
	case ${FDTP_CONFIGURE_OWNKERNEL:-} in yes) return 0 ;; no) return 1 ;; esac
	[ -d "/usr/lib/pomera-kernel/$(uname -r)" ]
}
STABILITY=${POMERA_STABILITY:-no}
WIFI_PS=${POMERA_WIFI_PS:-keep}
CLEAN_OLD=${POMERA_CLEAN_OLD:-keep}
RTC=${POMERA_RTC:-keep}
KERNEL_APT=${POMERA_KERNEL_APT:-keep}
PDCHARGE=${POMERA_PDCHARGE:-keep}
CVE=${POMERA_CVE:-keep}
INTERNAL=${POMERA_INTERNAL:-}
if [ -z "$INTERNAL" ]; then
	if [ "${POMERA_INTERNAL_RO:-no}" = yes ]; then INTERNAL=ro; else INTERNAL=keep; fi
fi
DT=${FDTP_CONFIGURE_DT:-/proc/device-tree}
msg_lang=${FDTP_MSG_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}
case $msg_lang in auto) msg_lang=${LC_ALL:-${LC_MESSAGES:-${LANG:-}}} ;; esac
say() { case $msg_lang in ja*) printf '%s\n' "$1" ;; *) printf '%s\n' "$2" ;; esac; }

[ "$KERNEL_APT" = yes ] || [ "$KERNEL_APT" = source ] || [ "$WIFI_PS" = off ] || [ "$WIFI_PS" = on ] || [ "$CLEAN_OLD" = yes ] || [ "$RTC" = yes ] || [ "$STABILITY" = yes ] || [ "$INTERNAL" = ro ] || [ "$INTERNAL" = rw ] || [ "$INTERNAL" = no ] || [ "$PDCHARGE" = yes ] || [ "$PDCHARGE" = no ] || [ "$CVE" = yes ] || [ "$CVE" = no ] || exit 0
model=$(cat "$DT/compatible" "$DT/model" 2>/dev/null | tr '\0' ' ' || true)
case $(printf '%s' "$model" | tr 'A-Z' 'a-z') in
*dm250*) ;;
*) say "Pomera DM250 ではないので、Pomera の設定は飛ばします。" "Not a Pomera DM250: the Pomera settings are skipped."; exit 0 ;;
esac
[ "$(id -u)" = 0 ] || { say "Pomera の設定には root が要ります（sudo make install）" "The Pomera settings need root (sudo make install)" >&2; exit 1; }

# Repeat the kernel check: config.mk may come from before a kernel upgrade.
if own_kernel; then STABILITY=no PDCHARGE=keep CVE=keep; fi
case $WIFI_PS in
 off) sh "$DIR/pomera_stability.sh" install-ps ;;
 on) sh "$DIR/pomera_stability.sh" remove-ps ;;
esac
if [ "$CLEAN_OLD" = yes ]; then
	sh "$DIR/pomera_stability.sh" remove-old
	sh "$DIR/pomera_pdcharge.sh" remove
	sh "$DIR/pomera_cve_2026_98432.sh" remove
fi
if [ "$RTC" = yes ]; then timedatectl set-local-rtc 0; fi
reboot=no
if [ "$STABILITY" = yes ]; then
	say "== Wi-Fi の安定化 1/2: 省電力オフ・停止時の自動再起動（watchdog） ==" "== Wi-Fi stability 1/2: power saving off, automatic restart on a freeze (watchdog) =="
	if sh "$DIR/pomera_stability.sh" install; then reboot=yes
	else say "省電力・watchdog の設定は行いませんでした（上の理由）。" "Power saving and the watchdog were not set up (see above)."; fi
	say "== Wi-Fi の安定化 2/2: Wi-Fi を転送用回路（DMA）から CPU が転送する方式（PIO）に変える ==" "== Wi-Fi stability 2/2: change Wi-Fi from a transfer circuit (DMA) to CPU-driven transfers (PIO) =="
	say "SD 内の機器設定データ（デバイスツリー）を、先頭から 24 MiB の位置で 1 MiB 書き換えます。" \
		"This rewrites 1 MiB of hardware settings (device tree) on the SD, starting 24 MiB from its beginning."
	say "  - ekesete.net の Debian 13 イメージ（D250 用）の SD に限ります。中身が違えば何もしません。" \
		"  - Only for an SD with the Debian 13 image for the D250 (ekesete.net); with anything else nothing is done."
	backup=$(sh "$DIR/pomera_dma_pio.sh" backup-dir)
	say "  - 書き換える前に元の 1 MiB を $backup/rsce-backup-*.bin に保存します。" \
		"  - The original 1 MiB is saved to $backup/rsce-backup-*.bin first."
	say "    戻すには: sudo sh $DIR/pomera_dma_pio.sh restore" \
		"    To undo: sudo sh $DIR/pomera_dma_pio.sh restore"
	say "  - 起動できなくなった場合は、SD を別の Linux 機につなぎ、rescue_rsce.sh と保存したバックアップをその機械で使って復旧します。手順は $DIR/rescue_rsce.sh 冒頭の説明を参照してください（台本も別の機械へコピーしてください）。" \
		"  - If the Pomera no longer boots, connect the SD to another Linux machine and use rescue_rsce.sh and the saved backup there. See the instructions at the start of $DIR/rescue_rsce.sh (copy the script to the other machine too)."
	answer=
	# Already in PIO (an earlier install): nothing to ask (user, 2026-11-04:
	# the bare question could not be judged on its own).
	pio_now=$(sh "$DIR/pomera_dma_pio.sh" state 2>/dev/null || true)
	if [ "$pio_now" = wifi-pio ] || [ "$pio_now" = pio ]; then
		say "Wi-Fi はもう PIO です（SD の 24 MiB の所は書き換え済み）。何もしません。" \
			"The Wi-Fi is already in PIO (the SD at 24 MiB was rewritten before): nothing to do."
		answer=skip
	elif [ -t 0 ]; then
		if [ "${FDTP_QUESTION_HINT_SHOWN:-no}" != yes ]; then
			say "はいは y、いいえは n。何も入力せず Enter を押すと、[Y/n]・[y/N] の大文字の方を選びます。" \
				"Enter y for yes or n for no. Press Enter without typing to choose the uppercase answer in [Y/n] or [y/N]."
		fi
		say "Wi-Fi が固まる不具合への対策として、SD 内の機器設定データを書き換え、Wi-Fi だけ CPU が転送する方式（PIO）にしますか？
はいの場合：SD を書き換える
いいえの場合：SD は書き換えず、省電力オフ・停止時の自動再起動の設定は残す
入力: [y/N]" \
			"To prevent Wi-Fi freezes, rewrite the SD hardware settings for Wi-Fi-only CPU-driven transfers (PIO)?
If yes: rewrite SD
If no: leave SD unchanged; keep power-saving-off and automatic restart settings
Answer: [y/N]"
		read -r answer || answer=
	else
		say "端末ではないので、書き換えは行いません。" "Not on a terminal: the SD is not rewritten."
	fi
	case $answer in
	y|Y|yes|Yes|YES|はい) sh "$DIR/pomera_dma_pio.sh" wifi || say "PIO の書き換えは行いませんでした（上の理由）。" "The PIO change was not made (see above)." ;;
	skip) ;;
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
case $PDCHARGE in
 yes) sh "$DIR/pomera_pdcharge.sh" install || say "追加充電プログラムの設定は行いませんでした（上の理由）。" "The additional charging program was not set up (see above)." ;;
 no) sh "$DIR/pomera_pdcharge.sh" remove || true ;;
esac
case $CVE in
 yes) sh "$DIR/pomera_cve_2026_98432.sh" install || say "CVE-2026-98432 の設定は行いませんでした（上の理由）。" "CVE-2026-98432 was not set up (see above)." ;;
 no) sh "$DIR/pomera_cve_2026_98432.sh" remove || true ;;
esac
case $KERNEL_APT in
 yes)
	newf=$(mktemp)
	FDTP_KERNEL_APT_NEW_FILE=$newf FDTP_MSG_LANG=$msg_lang sh "$DIR/pomera_kernel_apt.sh" install
	[ -s "$newf" ] && reboot=yes
	rm -f "$newf" ;;
 source) FDTP_MSG_LANG=$msg_lang sh "$DIR/pomera_kernel_apt.sh" source ;;
esac
[ "$reboot" = no ] || say "再起動してください（sudo reboot）。" "Please reboot (sudo reboot)."
