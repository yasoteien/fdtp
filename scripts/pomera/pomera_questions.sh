# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# The Pomera DM250 questions, in one place: sourced (". FILE") by
# ../configure (before make install) and by pomera_setup.sh (the .deb's
# fdtp-pomera-setup, after apt install). Not run by itself. Each question
# comes after what it does and how to undo it. Plain sh (dash).
#
# msg_init                 MSG=ja|en from LC_ALL, LC_MESSAGES, LANG (the first
#                          set; ja* is Japanese, but not on the Linux console,
#                          which cannot show it: then CONSOLE_NOTE=1);
#                          locale_now is that value
# pick JA EN, say JA EN, warn JA EN
# tty_init YES             TTY=yes|no (whether to ask: stdin is a terminal and
#                          YES is not 1; FDTP_CONFIGURE_TTY=yes|no replaces it,
#                          for the tests) and why (said when not asking)
# ask_yn y|n JA EN         ANSWER=yes|no; without a terminal the default, said so
# is_dm250 DT              true when DT/compatible or DT/model has "dm250"
# ask_wifi                 the Wi-Fi notes and question: STABILITY=yes|no
# ask_internal             the internal memory: INTERNAL=yes|no (asked only
#                          when root and a FAT is found there)
# The caller sets PDIR (this directory), UID_NOW (id -u), and PQ_AGAIN:
# make-install (configure: make install asks again before the PIO change)
# or now (fdtp-pomera-setup: asked again right before it).
# FDTP_CONFIGURE_PROBE replaces the internal memory probe (the tests).

msg_init() {
	locale_now=${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}
	case $locale_now in ja*) MSG=ja ;; *) MSG=en ;; esac
	CONSOLE_NOTE=
	if [ "$MSG" = ja ] && [ "${TERM:-}" = linux ]; then MSG=en; CONSOLE_NOTE=1; fi
}
pick() { if [ "$MSG" = ja ]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
say() { printf '%s\n' "$(pick "$1" "$2")"; }
warn() { printf '%s\n' "$(pick "注意: $1" "warning: $2")"; }

tty_init() {
	if [ -n "${FDTP_CONFIGURE_TTY:-}" ]; then TTY=$FDTP_CONFIGURE_TTY
	elif [ -t 0 ]; then TTY=yes
	else TTY=no
	fi
	[ "$1" = 1 ] && TTY=no
	why=$(if [ "$1" = 1 ]; then echo "--yes"; else pick "端末ではない" "not a terminal"; fi)
}

ask_yn() {
	q=$(pick "$2" "$3")
	if [ "$1" = y ]; then hint='[Y/n]'; else hint='[y/N]'; fi
	if [ "$TTY" != yes ]; then
		if [ "$1" = y ]; then ANSWER=yes; else ANSWER=no; fi
		printf '%s %s %s\n' "$q" "$hint" "$(pick "→ ${ANSWER}（既定の答え、${why}）" "-> $ANSWER (the default, $why)")"
		return
	fi
	while :; do
		printf '%s %s ' "$q" "$hint"
		if ! read -r a; then a=; echo; fi
		case $a in
		'') if [ "$1" = y ]; then ANSWER=yes; else ANSWER=no; fi; return ;;
		y|Y|yes|Yes|YES|はい) ANSWER=yes; return ;;
		n|N|no|No|NO|いいえ) ANSWER=no; return ;;
		*) say "y か n で答えてください。" "Please answer y or n." ;;
		esac
	done
}

is_dm250() {
	case $(cat "$1/compatible" "$1/model" 2>/dev/null | tr '\0' ' ' | tr 'A-Z' 'a-z') in
	*dm250*) return 0 ;;
	*) return 1 ;;
	esac
}

ask_wifi() {
	backup=$(sh "$PDIR/pomera_dma_pio.sh" backup-dir)
	if [ "$MSG" = ja ]; then cat <<EOF
== Wi-Fi の安定化（Pomera DM250 だけ） ==
DM250 の Debian 13 では、SD・内蔵メモリ・Wi-Fi が 1 つの DMA を共用していて、
その不具合で 3 つが同時に止まる（固まる）事があります。次の 2 つを行います。
  1. Wi-Fi の省電力を切り、ハードウェアの watchdog を使う（止まったら自動で再起動）
  2. Wi-Fi だけ DMA をやめて PIO にする（SD の 24 MiB の所の 1 MiB を書き換える）
注意:
  - 2 は ekesete.net の Debian 13 イメージ（D250 用）の SD に限ります。中身が違えば何もしません。
  - 書き換える前に元の 1 MiB を $backup/rsce-backup-*.bin に保存します。
    戻すには: sudo sh $PDIR/pomera_dma_pio.sh restore
  - 電池の持ちが少し悪くなります。Wi-Fi の速さが落ちる事があります。
  - 終わったら再起動が要ります。
EOF
		case ${PQ_AGAIN:-make-install} in
		now) echo "  - 2 は書き換える直前にもう一度聞きます（既定は「いいえ」）。"
			echo "  - この設定を全部戻すには: sudo fdtp-pomera-setup --undo" ;;
		*) echo "  - 2 は sudo make install の時にもう一度聞きます（既定は「いいえ」）。" ;;
		esac
	else cat <<EOF
== Wi-Fi stability (Pomera DM250 only) ==
On the DM250's Debian 13 the SD, the internal memory and the Wi-Fi share one
DMA controller, and a bug in it can stop (freeze) all three at once. This does two things:
  1. Turns the Wi-Fi's power saving off and uses the hardware watchdog
     (the Pomera restarts by itself when it freezes)
  2. Lets only the Wi-Fi stop using the DMA (PIO instead): rewrites the 1 MiB at 24 MiB into the SD
Notes:
  - 2 is only for an SD with the Debian 13 image for the D250 (ekesete.net). With anything else nothing is done.
  - The original 1 MiB is saved to $backup/rsce-backup-*.bin first.
    To undo: sudo sh $PDIR/pomera_dma_pio.sh restore
  - The battery lasts a little less. The Wi-Fi may be slower.
  - A reboot is needed afterwards.
EOF
		case ${PQ_AGAIN:-make-install} in
		now) echo "  - Right before 2 you are asked once more (the default there is no)."
			echo "  - To undo all of these settings: sudo fdtp-pomera-setup --undo" ;;
		*) echo "  - sudo make install asks once more before 2 (the default there is no)." ;;
		esac
	fi
	ask_yn y "Wi-Fi を安定させますか" "Make the Wi-Fi stable?"
	STABILITY=$ANSWER
}

ask_internal() {
	INTERNAL=no
	say "== 内蔵メモリのマウント（Pomera DM250 だけ） ==" "== Mounting the internal memory (Pomera DM250 only) =="
	found_fs=
	if [ "$UID_NOW" != 0 ]; then
		say "内蔵メモリの位置を確かめるには root が要ります（sudo ./configure なら確かめられます）。今は飛ばします。" \
			"Finding the internal memory needs root (sudo ./configure can do it). Skipped for now."
		return
	fi
	if [ -n "${FDTP_CONFIGURE_PROBE:-}" ]; then found_fs=$(eval "$FDTP_CONFIGURE_PROBE" 2>/dev/null | head -1)
	else found_fs=$(sh "$PDIR/pomera_mount_internal.sh" fstype 2>/dev/null | head -1)
	fi
	case $found_fs in vfat|exfat) ;; *)
		say "この Pomera では内蔵メモリの位置が分からないので飛ばします。" \
			"The internal memory was not found where it is on the Japanese DM250: skipped."
		return ;;
	esac
	if [ "$MSG" = ja ]; then cat <<EOF
標準の OS（ポメラのモード）で書いた文書がある内蔵メモリの領域（${found_fs}）を、
起動のたびに /mnt/pomera-internal に読み取り専用でつなぎます。FDTP から読めます。
注意:
  - 読み取り専用です。内蔵メモリには何も書きません。
  - /etc/fstab に 1 行足します（元は /etc/fstab.bak-internal に保存）。
    やめるには: sudo sh $PDIR/pomera_mount_internal.sh unpersist
  - 位置は日本語版の DM250 の値です。見つからない時はこの質問を飛ばします。
EOF
	else cat <<EOF
The area of the internal memory with the documents written in the stock OS (the
Pomera mode; $found_fs) is attached read-only at /mnt/pomera-internal at every boot.
FDTP can read them there.
Notes:
  - Read-only: nothing is written to the internal memory.
  - Adds one line to /etc/fstab (the old one is kept as /etc/fstab.bak-internal).
    To stop it: sudo sh $PDIR/pomera_mount_internal.sh unpersist
  - The place is that of the Japanese DM250. Where nothing is found there, this question is skipped.
EOF
	fi
	ask_yn y "内蔵メモリを読み取り専用でマウントしますか" "Mount the internal memory read-only?"
	INTERNAL=$ANSWER
}
