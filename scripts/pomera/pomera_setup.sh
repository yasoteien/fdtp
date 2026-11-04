#!/bin/sh
# fdtp-pomera-setup: the Pomera DM250 settings that ./configure asks about
# and sudo make install makes, for FDTP installed from the .deb (it puts
# this file at /usr/sbin/fdtp-pomera-setup and the rest of this directory
# at /usr/share/fdtp/pomera). From the source tree: sudo sh pomera/pomera_setup.sh.
#
#   sudo fdtp-pomera-setup [--yes]          the questions of ./configure (Wi-Fi
#                                           stability, the mount mode of the
#                                           internal memory, PD hub charging,
#                                           CVE-2026-98432 mitigation, installing libheif
#                                           for HEIC when it is missing), each
#                                           after what it does and how to undo
#                                           it; then the settings answered yes
#                                           (the PIO change is asked once more,
#                                           default no)
#   sudo fdtp-pomera-setup --undo [--yes]   undo what is set (the PIO change is
#                                           asked once more, default no)
#   sudo fdtp-pomera-setup --status         list what is set; exit 0 when
#                                           something is (the .deb's prerm)
# pomera-kernel (test release) asks about Wi-Fi power saving and old-workaround
# removal instead. LOCAL RTC can be treated as UTC on either kernel.
# With pomera-kernel (test release), --undo never restores boot data separately;
# use sudo pomera-kernel rollback to restore the previous kernel and U-Boot together.
# Reboot afterwards.
# --yes, or no terminal, takes every default: then the SD is never rewritten.
# Does nothing on anything but a DM250. The language of the messages is
# that of ./configure (LC_ALL, LC_MESSAGES, LANG; English on the Linux console).
# For the tests: FDTP_CONFIGURE_DT, FDTP_CONFIGURE_UID, FDTP_CONFIGURE_PROBE,
# FDTP_CONFIGURE_TTY, FDTP_CONFIGURE_HEIF and FDTP_CONFIGURE_APT as in ./configure;
# FDTP_POMERA_DIR, where the scripts are.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
if [ -n "${FDTP_POMERA_DIR:-}" ]; then PDIR=$FDTP_POMERA_DIR
else
	here=$(cd "$(dirname "$0")" && pwd)
	if [ -f "$here/pomera_questions.sh" ]; then PDIR=$here; else PDIR=/usr/share/fdtp/pomera; fi
fi
[ -f "$PDIR/pomera_questions.sh" ] || { echo "fdtp-pomera-setup: $PDIR/pomera_questions.sh is missing" >&2; exit 1; }
DT=${FDTP_CONFIGURE_DT:-/proc/device-tree}
UID_NOW=${FDTP_CONFIGURE_UID:-$(id -u)}
PQ_AGAIN=now
. "$PDIR/pomera_questions.sh"

MODE=setup YES=0
for a; do
	case $a in
	-y|--yes) YES=1 ;;
	--undo) MODE=undo ;;
	--status) MODE=status ;;
	-h|--help) sed -n '2,25p' "$0"; exit 0 ;;
	*) echo "fdtp-pomera-setup: unknown option $a" >&2; sed -n '7,18p' "$0" >&2; exit 2 ;;
	esac
done
msg_init
tty_init "$YES"

if ! is_dm250 "$DT"; then
	[ "$MODE" = status ] || say "Pomera DM250 ではないので、何もしません。" "Not a Pomera DM250: nothing to do."
	exit 1
fi
if [ "$UID_NOW" != 0 ]; then
	say "root が要ります: sudo fdtp-pomera-setup" "This needs root: sudo fdtp-pomera-setup" >&2
	exit 1
fi

# What is set now, one line each in SET (empty: nothing).
status() {
	SET= STAB_SET= INT_SET= PIO_SET= PD_SET= CVE_SET= KERNEL_APT_SET=
	if files=$(sh "$PDIR/pomera_stability.sh" present 2>/dev/null); then
		STAB_SET=1
		for file in $files; do
			case $file in
			*/90-pomera-wifi-powersave.conf) label=$(pick "Wi-Fi の省電力をオフにする設定：あり" "Wi-Fi power-saving-off setting: present") ;;
			*/90-pomera-watchdog.conf) label=$(pick "停止時の自動再起動設定：あり" "Automatic restart on a freeze: configured") ;;
			*) label=$(pick "機器の省電力に関する旧対策：あり" "Old device power-saving workaround: present") ;;
			esac
			SET="$SET$label ($file)
"
		done
	fi
	if sh "$PDIR/pomera_mount_internal.sh" persisted >/dev/null 2>&1; then
		INT_SET=1
		internal_mode=$(sh "$PDIR/pomera_mount_internal.sh" persisted-mode 2>/dev/null)
		case $internal_mode in
		rw) label=$(pick "内蔵文書：読み書きで使う設定（pomera-internal.service）" "Internal documents: read-write access configured (pomera-internal.service)") ;;
		ro) label=$(pick "内蔵文書：読み取り専用で使う設定（/etc/fstab）" "Internal documents: read-only access configured (/etc/fstab)") ;;
		*) label=$(pick "内蔵文書：自動利用の設定あり（方式を判別できません）" "Internal documents: automatic access configured (mode unknown)") ;;
		esac
		SET="$SET$label
"
	fi
	if sh "$PDIR/pomera_pdcharge.sh" status >/dev/null 2>&1; then
		PD_SET=1
		SET="$SET$(pick "PD ハブ充電用の追加プログラム：設定あり（充電 LED の利用可否は未確認）" "Additional PD hub charging program: configured (charge LED availability not checked)")
"
	fi
	if sh "$PDIR/pomera_cve_2026_98432.sh" status >/dev/null 2>&1; then
		CVE_SET=1
		SET="$SET$(pick "通信の脆弱性 CVE-2026-98432 の対策（通信を遮断する部品 nftables）" "Network vulnerability CVE-2026-98432 mitigation (nftables traffic filter)")
"
	fi
	if sh "$PDIR/pomera_kernel_apt.sh" status >/dev/null 2>&1; then
		KERNEL_APT_SET=1
		SET="$SET$(pick "pomera-kernel（試験版）を apt で入れるための配布元の登録・署名確認用の鍵" "apt distribution source registration / signature verification key for pomera-kernel (test release)")
"
	fi
	pio=$(sh "$PDIR/pomera_dma_pio.sh" state 2>/dev/null) || pio=
	case $pio in wifi-pio|pio)
		PIO_SET=1
		case $pio in
		wifi-pio) label=$(pick "Wi-Fi だけ CPU が転送する方式（PIO）に変更済み" "Only Wi-Fi changed to CPU-driven transfers (PIO)") ;;
		pio) label=$(pick "Wi-Fi 以外も CPU が転送する方式（PIO）に変更済み" "Wi-Fi and other devices changed to CPU-driven transfers (PIO)") ;;
		esac
		SET="$SET$label
" ;;
	esac
}

case $MODE in
status)
	status
	[ -n "$SET" ] || exit 1
	printf '%s' "$SET"
	exit 0
	;;
undo)
	[ -z "$CONSOLE_NOTE" ] || echo "(LANG is ja*, but the Linux console cannot show Japanese: the messages are in English.)"
	say "== FDTP の Pomera の設定を戻す ==" "== Undo FDTP's Pomera settings =="
	status
	reboot=no
	say "時計の読み取り設定は --undo の対象外です。SD の CPU 転送方式（PIO）の書き戻しは、この後で別に確認します。" \
		"Hardware clock interpretation is outside --undo. Restoring the SD CPU-transfer (PIO) settings is confirmed separately below."
	if [ -z "$SET" ]; then say "このコマンドで解除できる追加設定はありません。時計の読み取り設定は対象外です。" "There are no additional settings this command can remove. Hardware clock interpretation is outside its scope."; exit 0; fi
	say "今あるもの:" "Set now:"
	printf '%s' "$SET" | sed 's/^/  /'
	if [ -n "$STAB_SET$INT_SET$PD_SET$CVE_SET$KERNEL_APT_SET" ]; then
		# Only the lines about what is set (1.15: with only the kernel apt source,
		# the fstab, pdcharge and CVE lines were shown too).
		if [ -n "$STAB_SET$INT_SET$PD_SET$CVE_SET" ]; then
			say "一覧にある追加設定だけを解除します。充電プログラムがあれば、すぐ停止します。内蔵文書の現在の利用状態や停止時の自動再起動が設定されている場合、その動作は再起動まで残ります。" \
				"Only the listed additional settings are removed. If a charging program is configured, it stops immediately. If internal document access or automatic restart on a freeze is configured, that behavior remains until reboot."
		fi
		[ -z "$CVE_SET" ] || say "CVE-2026-98432 の対策もすぐ外れます。" "The CVE-2026-98432 mitigation is also removed immediately."
		[ -z "$KERNEL_APT_SET" ] || say "pomera-kernel（試験版）を apt で入れるための配布元の登録と署名確認用の鍵を外します。カーネルは外しません。" "Remove the apt distribution source registration and signature verification key for pomera-kernel (test release); the kernel stays installed."
		ask_yn y "一覧のうち、SD の PIO 設定を除く FDTP の追加設定を解除しますか？
はいの場合：解除
いいえの場合：残す
入力:" "Remove the listed FDTP additional settings except SD PIO settings?
If yes: remove
If no: keep
Answer:"
		if [ "$ANSWER" = yes ]; then
			[ -z "$STAB_SET" ] || { sh "$PDIR/pomera_stability.sh" remove && reboot=yes; }
			[ -z "$INT_SET" ] || { sh "$PDIR/pomera_mount_internal.sh" unpersist && reboot=yes; }
			[ -z "$PD_SET" ] || sh "$PDIR/pomera_pdcharge.sh" remove
			[ -z "$KERNEL_APT_SET" ] || sh "$PDIR/pomera_kernel_apt.sh" remove
			[ -z "$CVE_SET" ] || sh "$PDIR/pomera_cve_2026_98432.sh" remove
		fi
	fi
	if own_kernel; then
		say "pomera-kernel（試験版）は、カーネルと U-Boot を組で管理します。組に含まれる起動用データ（RSCE）だけを個別に書き戻すことはしません。前の組に戻すには sudo pomera-kernel rollback を実行し、再起動してください。" \
			"pomera-kernel (test release) manages the kernel and U-Boot as a set. Boot data (RSCE) within the set is not restored separately. To restore the previous set, run sudo pomera-kernel rollback and reboot."
	elif [ -n "$PIO_SET" ]; then
		backup=$(sh "$PDIR/pomera_dma_pio.sh" backup-dir)
		say "SD の 24 MiB の所に、$backup/rsce-backup-*.bin に保存した元の 1 MiB を書き戻します。" \
			"The original 1 MiB saved in $backup/rsce-backup-*.bin is written back at 24 MiB into the SD."
		ask_yn n "PIO 化を取り消すため、SD の起動用設定を変更前のバックアップに戻しますか？
はいの場合：復元
いいえの場合：現在の設定を残す
入力:" "Restore the SD boot settings from the original backup to undo PIO?
If yes: restore
If no: keep the current settings
Answer:"
		if [ "$ANSWER" = yes ]; then
			if FDTP_MSG_LANG=$MSG sh "$PDIR/pomera_dma_pio.sh" restore; then reboot=yes
			else say "書き戻しは行いませんでした（上の理由）。" "Nothing was written back (see above)."; fi
		else
			say "書き戻しは行いませんでした。あとで行うには: sudo sh $PDIR/pomera_dma_pio.sh restore" \
				"Nothing was written back. To do it later: sudo sh $PDIR/pomera_dma_pio.sh restore"
		fi
	fi
	[ "$reboot" = no ] || say "再起動してください（sudo reboot）。" "Please reboot (sudo reboot)."
	exit 0
	;;
esac

[ -z "$CONSOLE_NOTE" ] || echo "(LANG is ja*, but the Linux console cannot show Japanese: the messages are in English.)"
say "FDTP の Pomera DM250 の設定（${PDIR}）" "FDTP's Pomera DM250 settings ($PDIR)"
echo
ask_pomera
echo
ask_heif
echo
# 3 (do not mount) changes something only when an earlier mount is set up.
if [ "$INTERNAL" = no ] && ! sh "$PDIR/pomera_mount_internal.sh" persisted 2>/dev/null; then INTERNAL=keep; fi
if [ "$STABILITY" = no ] && [ "$INTERNAL" = keep ] && [ "$PDCHARGE" = keep ] && [ "$CVE" = keep ] && [ "$WIFI_PS" = keep ] && [ "$CLEAN_OLD" != yes ] && [ "$RTC" != yes ] && [ "$KERNEL_APT" != yes ] && [ "$KERNEL_APT" != source ]; then
	say "Pomera 固有の追加設定は変更しませんでした。" "Pomera-specific additional settings were not changed."
	exit 0
fi
# install_pomera.sh asks before the PIO change only on a terminal.
if [ "$TTY" = yes ]; then
	FDTP_QUESTION_HINT_SHOWN=${QUESTION_HINT_SHOWN:-no} POMERA_KERNEL_APT=$KERNEL_APT POMERA_WIFI_PS=$WIFI_PS POMERA_CLEAN_OLD=$CLEAN_OLD POMERA_RTC=$RTC POMERA_STABILITY=$STABILITY POMERA_INTERNAL=$INTERNAL POMERA_PDCHARGE=$PDCHARGE POMERA_CVE=$CVE FDTP_MSG_LANG=$MSG FDTP_CONFIGURE_DT=$DT sh "$PDIR/install_pomera.sh"
else
	FDTP_QUESTION_HINT_SHOWN=${QUESTION_HINT_SHOWN:-no} POMERA_KERNEL_APT=$KERNEL_APT POMERA_WIFI_PS=$WIFI_PS POMERA_CLEAN_OLD=$CLEAN_OLD POMERA_RTC=$RTC POMERA_STABILITY=$STABILITY POMERA_INTERNAL=$INTERNAL POMERA_PDCHARGE=$PDCHARGE POMERA_CVE=$CVE FDTP_MSG_LANG=$MSG FDTP_CONFIGURE_DT=$DT sh "$PDIR/install_pomera.sh" </dev/null
fi
