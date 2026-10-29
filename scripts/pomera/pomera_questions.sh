# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# The Pomera DM250 questions, in one place: sourced (". FILE") by
# ../configure (before make install) and by pomera_setup.sh (the .deb's
# fdtp-pomera-setup, after apt install). Not run by itself. Each question
# comes after what it does and how to undo it. Plain sh (dash). The libheif
# question (HEIC) is asked on any Debian, not only on a DM250.
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
# ask_internal             the internal memory: INTERNAL=ro|rw|no (asked only
#                          when root and a FAT is found there; keep when the
#                          question was skipped: the setting is left as it is)
# ask_heif                 libheif for HEIC (iPhone photos), which FDTP reads
#                          with dlopen when one is opened: when it is missing,
#                          root is asked to install it with apt; others are
#                          told the apt line. HEIF=yes|no (yes: there now)
# The caller sets PDIR (this directory), UID_NOW (id -u), and PQ_AGAIN:
# make-install (configure: make install asks again before the PIO change)
# or now (fdtp-pomera-setup: asked again right before it).
# FDTP_CONFIGURE_PROBE replaces the internal memory probe (the tests);
# FDTP_CONFIGURE_HEIF=yes|no whether libheif is installed, and
# FDTP_CONFIGURE_APT the apt-get command (the tests).

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
	INTERNAL=keep
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
標準 OS（ポメラのモード）の文書がある内蔵メモリ（${found_fs}）を
/mnt/pomera-internal にマウントします。FDTP から利用できます。
  1. 読み取り専用（既定）: /etc/fstab に 1 行追加します。
     元の fstab は /etc/fstab.bak-internal に保存します。
  2. 読み書き: 起動後に pomera-internal.service が点検してマウントします。
  3. マウントしない
設定をやめるには: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
	else cat <<EOF
Mount the internal memory containing documents from the stock OS (Pomera mode;
${found_fs}) at /mnt/pomera-internal for use by FDTP.
  1. Read-only (default): adds a line to /etc/fstab and saves the original
     as /etc/fstab.bak-internal.
  2. Read-write: pomera-internal.service checks and mounts it after boot.
  3. Do not mount
To stop either setting: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
	fi
	if [ "$TTY" != yes ]; then
		INTERNAL=ro
		printf '%s\n' "$(pick "選択 [1/2/3] → 1（既定の答え、${why}）" "Choice [1/2/3] -> 1 (the default, $why)")"
		return
	fi
	while :; do
		printf '%s ' "$(pick '選択 [1/2/3]:' 'Choice [1/2/3]:')"
		if ! read -r a; then a=; echo; fi
		case $a in
		''|1) INTERNAL=ro; return ;;
		3) INTERNAL=no; return ;;
		2)
			if [ "$MSG" = ja ]; then cat <<EOF
読み書きを選ぶと Linux から内蔵メモリに書けます。純正のポメラのモードでも同じファイルが見えます。
Pomera が止まった時に書いていたファイルは、戻らないことがあります。
起動時は「使用中」の印だけを見ます。印が残っていた時（前回止まった時）だけ、
起動後に fsck.fat -n（読むだけ）で点検します。毎回は流さないので起動は遅くなりません。
dosfstools が純正 OS の Shift_JIS の短い名前を誤って改名するため、自動修復はしません。
既知の指摘だけなら読み書きで、それ以外なら読み取り専用でマウントし、
ログイン時と FDTP 起動時に結果を知らせます。
点検には dosfstools（fsck.fat）が要ります。無い時は sudo apt install dosfstools
やめるには: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
			else cat <<EOF
Read-write lets Linux write to the internal memory. The same files are visible
in the stock Pomera mode. A file being written when the Pomera stops may be lost.
At boot only the in-use flag is checked. If it remains set (the Pomera stopped
last time), fsck.fat -n checks the memory after boot without writing to it.
It does not run every time, so it does not slow down boot.
There is no automatic repair: dosfstools can incorrectly rename Shift_JIS short
names used by the stock OS. With only known findings it mounts read-write; otherwise
it mounts read-only. The result is shown at login and when FDTP starts.
The check needs dosfstools (fsck.fat): sudo apt install dosfstools
To stop it: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
			fi
			ask_yn n "それでも読み書きにしますか" "Mount it read-write anyway?"
			if [ "$ANSWER" = yes ]; then INTERNAL=rw; return; fi ;;
		*) say "1・2・3 のどれかで答えてください。" "Please answer 1, 2 or 3." ;;
		esac
	done
}

HEIF_PKGS='libheif1 libheif-plugin-libde265'
heif_have() {
	case ${FDTP_CONFIGURE_HEIF:-} in yes) return 0 ;; no) return 1 ;; esac
	if command -v dpkg-query >/dev/null 2>&1; then
		for p in $HEIF_PKGS; do
			case $(dpkg-query -W -f='${Status}' "$p" 2>/dev/null) in *" installed") ;; *) return 1 ;; esac
		done
		return 0
	fi
	{ /sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null; } | grep -q 'libheif\.so\.1 '
}

ask_heif() {
	say "== HEIC（iPhone の写真） ==" "== HEIC (iPhone photos) =="
	if heif_have; then
		HEIF=yes
		say "libheif があるので、HEIC の写真を画像の表示で開けます。" "libheif is installed: the image viewer opens HEIC photos."
		return
	fi
	HEIF=no
	apt=${FDTP_CONFIGURE_APT:-apt-get}
	if [ "$UID_NOW" != 0 ] || ! command -v "${apt%% *}" >/dev/null 2>&1; then
		say "HEIC を開くには libheif が要ります。入れるには: sudo apt install $HEIF_PKGS" \
			"Opening HEIC needs libheif. To install it: sudo apt install $HEIF_PKGS"
		return
	fi
	if [ "$MSG" = ja ]; then cat <<EOF
iPhone のカメラの写真（HEIC）を FDTP の画像の表示で開くための部品
（$HEIF_PKGS）を apt で入れます。
FDTP は libheif を写真を開く時に読むので、入れなくても FDTP は動きます（HEIC だけ開けません）。
あとで入れても FDTP の作り直しは要りません。
外すには: sudo apt remove $HEIF_PKGS
EOF
	else cat <<EOF
Installs $HEIF_PKGS with apt, for opening iPhone camera photos (HEIC)
in FDTP's image viewer. FDTP reads libheif when a photo is opened: without it
FDTP still works (only HEIC does not open), and installing it later needs no rebuild.
To remove it: sudo apt remove $HEIF_PKGS
EOF
	fi
	ask_yn y "HEIC を開くには libheif が要ります。入れますか" "Opening HEIC needs libheif. Install it?"
	if [ "$ANSWER" != yes ]; then
		say "入れませんでした。あとで入れるには: sudo apt install $HEIF_PKGS" \
			"Not installed. To install it later: sudo apt install $HEIF_PKGS"
	elif $apt install -y $HEIF_PKGS; then
		HEIF=yes
	else
		say "入りませんでした（上の理由）。あとで入れるには: sudo apt install $HEIF_PKGS" \
			"It was not installed (see above). To install it later: sudo apt install $HEIF_PKGS"
	fi
}
