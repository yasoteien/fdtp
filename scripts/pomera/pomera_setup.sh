#!/bin/sh
# fdtp-pomera-setup: the Pomera DM250 settings that ./configure asks about
# and sudo make install makes, for FDTP installed from the .deb (it puts
# this file at /usr/sbin/fdtp-pomera-setup and the rest of this directory
# at /usr/share/fdtp/pomera). From the source tree: sudo sh pomera/pomera_setup.sh.
#
#   sudo fdtp-pomera-setup [--yes]          the questions of ./configure (Wi-Fi
#                                           stability, the read-only mount of the
#                                           internal memory), each after what it
#                                           does and how to undo it; then the
#                                           settings answered yes (the PIO change
#                                           is asked once more, default no)
#   sudo fdtp-pomera-setup --undo [--yes]   undo what is set (the PIO change is
#                                           asked once more, default no)
#   sudo fdtp-pomera-setup --status         list what is set; exit 0 when
#                                           something is (the .deb's prerm)
# --yes, or no terminal, takes every default: then the SD is never rewritten.
# Does nothing on anything but a DM250. The language of the messages is
# that of ./configure (LC_ALL, LC_MESSAGES, LANG; English on the Linux console).
# For the tests: FDTP_CONFIGURE_DT, FDTP_CONFIGURE_UID, FDTP_CONFIGURE_PROBE
# and FDTP_CONFIGURE_TTY as in ./configure; FDTP_POMERA_DIR, where the scripts are.
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
	-h|--help) sed -n '2,21p' "$0"; exit 0 ;;
	*) echo "fdtp-pomera-setup: unknown option $a" >&2; sed -n '7,17p' "$0" >&2; exit 2 ;;
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
	SET= STAB_SET= INT_SET= PIO_SET=
	if files=$(sh "$PDIR/pomera_stability.sh" present 2>/dev/null); then
		STAB_SET=1
		SET="$SET$(pick "Wi-Fi の安定化（省電力・watchdog）:" "Wi-Fi stability (power saving, watchdog):") $(echo $files)
"
	fi
	if sh "$PDIR/pomera_mount_internal.sh" persisted >/dev/null 2>&1; then
		INT_SET=1
		SET="$SET$(pick "内蔵メモリの読み取り専用マウント: /etc/fstab" "read-only mount of the internal memory: /etc/fstab")
"
	fi
	pio=$(sh "$PDIR/pomera_dma_pio.sh" state 2>/dev/null) || pio=
	case $pio in wifi-pio|pio)
		PIO_SET=1
		SET="$SET$(pick "SD の 24 MiB の所:" "the SD at 24 MiB:") $pio
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
	if [ -z "$SET" ]; then say "戻す物はありません。" "Nothing to undo."; exit 0; fi
	say "今あるもの:" "Set now:"
	printf '%s' "$SET" | sed 's/^/  /'
	if [ -n "$STAB_SET$INT_SET" ]; then
		say "上の設定のファイルを消します（/etc/fstab の行も）。効くのは再起動の後です。" \
			"The files of these settings are deleted (and the line in /etc/fstab). It takes effect after a reboot."
		ask_yn y "戻しますか" "Undo them?"
		if [ "$ANSWER" = yes ]; then
			[ -z "$STAB_SET" ] || sh "$PDIR/pomera_stability.sh" remove
			[ -z "$INT_SET" ] || sh "$PDIR/pomera_mount_internal.sh" unpersist
		fi
	fi
	if [ -n "$PIO_SET" ]; then
		backup=$(sh "$PDIR/pomera_dma_pio.sh" backup-dir)
		say "SD の 24 MiB の所に、$backup/rsce-backup-*.bin に保存した元の 1 MiB を書き戻します。" \
			"The original 1 MiB saved in $backup/rsce-backup-*.bin is written back at 24 MiB into the SD."
		ask_yn n "SD の 24 MiB の所を書き戻しますか" "Write the SD at 24 MiB back?"
		if [ "$ANSWER" = yes ]; then
			FDTP_MSG_LANG=$MSG sh "$PDIR/pomera_dma_pio.sh" restore ||
				say "書き戻しは行いませんでした（上の理由）。" "Nothing was written back (see above)."
		else
			say "書き戻しは行いませんでした。あとで行うには: sudo sh $PDIR/pomera_dma_pio.sh restore" \
				"Nothing was written back. To do it later: sudo sh $PDIR/pomera_dma_pio.sh restore"
		fi
	fi
	say "再起動してください（sudo reboot）。" "Please reboot (sudo reboot)."
	exit 0
	;;
esac

[ -z "$CONSOLE_NOTE" ] || echo "(LANG is ja*, but the Linux console cannot show Japanese: the messages are in English.)"
say "FDTP の Pomera DM250 の設定（${PDIR}）" "FDTP's Pomera DM250 settings ($PDIR)"
echo
ask_wifi
echo
ask_internal
echo
if [ "$STABILITY" = no ] && [ "$INTERNAL" = no ]; then
	say "何も変えませんでした。" "Nothing was changed."
	exit 0
fi
# install_pomera.sh asks before the PIO change only on a terminal.
if [ "$TTY" = yes ]; then
	POMERA_STABILITY=$STABILITY POMERA_INTERNAL_RO=$INTERNAL FDTP_MSG_LANG=$MSG FDTP_CONFIGURE_DT=$DT sh "$PDIR/install_pomera.sh"
else
	POMERA_STABILITY=$STABILITY POMERA_INTERNAL_RO=$INTERNAL FDTP_MSG_LANG=$MSG FDTP_CONFIGURE_DT=$DT sh "$PDIR/install_pomera.sh" </dev/null
fi
