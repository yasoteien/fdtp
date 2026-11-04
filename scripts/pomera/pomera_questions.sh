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
# own_kernel               directory /usr/lib/pomera-kernel/$(uname -r),
#                          FDTP_CONFIGURE_OWNKERNEL=yes|no overrides it
# ask_pomera               kernel-specific questions, then ask_rtc (LOCAL only)
# ask_wifi_ps              WIFI_PS=on (keep the kernel default, power saving on)|off;
#                          POMERA_WIFI_PS_DEFAULT=y keeps it
# ask_leftovers            CLEAN_OLD=yes|no|keep; only udev/watchdog, PD and CVE
# ask_kernel_apt           KERNEL_APT=yes|no|source|keep; default n, own kernels
#                          add only the source when missing (otherwise keep)
# ask_rtc                  RTC=yes|keep; FDTP_CONFIGURE_ADJTIME replaces /etc/adjtime
# ask_wifi                 the Wi-Fi notes and question: STABILITY=yes|no
# ask_internal             the internal memory: INTERNAL=ro|rw|no (asked only
#                          when root and a FAT is found there; keep when the
#                          question was skipped: the setting is left as it is)
# ask_pdcharge             PD hub charging: PDCHARGE=yes|no|keep; skipped
#                          unless the read-only hardware probe passes
# ask_cve                  CVE-2026-98432: CVE=yes|no|keep; skipped unless
#                          the DM250 kernel probe passes
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
	case ${FDTP_MSG_LANG:-auto} in auto) ;; *) locale_now=$FDTP_MSG_LANG ;; esac
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

kernel_intro() {
	[ "${KERNEL_INTRO_SHOWN:-no}" = yes ] && return 0
	say "pomera-kernel（試験版）は、Linux 6.18 系の長期保守版に、DM250 用の直しを足したカーネルです。カーネルは機器を動かす Linux の中核部分です。" \
		"pomera-kernel (test release) is a kernel based on the Linux 6.18 longterm series with fixes for the DM250. A kernel is the core of Linux that operates the hardware."
	KERNEL_INTRO_SHOWN=yes
}
question_hint() {
	[ "${QUESTION_HINT_SHOWN:-no}" = yes ] && return 0
	say "はいは y、いいえは n。何も入力せず Enter を押すと、[Y/n]・[y/N] の大文字の方を選びます。" \
		"Enter y for yes or n for no. Press Enter without typing to choose the uppercase answer in [Y/n] or [y/N]."
	QUESTION_HINT_SHOWN=yes
}

ask_yn() {
	question_hint
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

own_kernel() {
	case ${FDTP_CONFIGURE_OWNKERNEL:-} in yes) return 0 ;; no) return 1 ;; esac
	[ -d "/usr/lib/pomera-kernel/$(uname -r)" ]
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
DM250 の Debian 13 では、データ転送を受け持つ回路（DMA）を SD・内蔵メモリ・Wi-Fi が共用しています。
その不具合で 3 つが同時に止まることがあります。次の 2 つを行います。
  1. Wi-Fi の省電力を切り、ハードウェアの watchdog を使う（止まったら自動で再起動）
  2. Wi-Fi だけ CPU が転送する方式（PIO）に変える
     （SD 内の機器設定データ〈デバイスツリー〉を、先頭から 24 MiB の位置で 1 MiB 書き換える）
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
On the DM250's Debian 13, the SD, internal memory and Wi-Fi share a data transfer
circuit (DMA controller). A bug can freeze all three at once. This does two things:
  1. Turns the Wi-Fi's power saving off and uses the hardware watchdog
     (the Pomera restarts by itself when it freezes)
  2. Changes only the Wi-Fi to CPU-driven transfers (PIO): rewrites 1 MiB of
     hardware settings (device tree) on the SD, starting 24 MiB from its beginning
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
	ask_yn y "Wi-Fi が固まる不具合への対策を設定しますか？
はいの場合：省電力オフ・停止時の自動再起動を設定、SD の書き換えは別途確認
いいえの場合：既存設定を変更しない
入力:" "Set up Wi-Fi freeze workarounds?
If yes: disable power saving and enable automatic restart on a freeze; SD rewriting is confirmed separately
If no: keep existing settings
Answer:"
	STABILITY=$ANSWER
}

ask_internal() {
	INTERNAL=keep
	say "== 内蔵文書を Linux で使う設定（Pomera DM250 だけ） ==" "== Using internal documents in Linux (Pomera DM250 only) =="
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
/mnt/pomera-internal で使えるようにつなぎます（マウント）。FDTP から利用できます。
  1. 読み取り専用（既定）: /etc/fstab に 1 行追加します。
     元の fstab は /etc/fstab.bak-internal に保存します。
  2. 読み書き: 起動後に pomera-internal.service が点検してマウントします。
  3. 起動時に自動で使う設定を解除する
     現在利用中の状態は、再起動または手動で切り離すまで残ります。
設定をやめるには: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
	else cat <<EOF
Connect the internal memory containing stock OS documents (Pomera mode;
${found_fs}) at /mnt/pomera-internal for use by FDTP (mounting).
  1. Read-only (default): adds a line to /etc/fstab and saves the original
     as /etc/fstab.bak-internal.
  2. Read-write: pomera-internal.service checks and mounts it after boot.
  3. Remove automatic access at boot
     Current access remains until reboot or manual unmounting.
To stop either setting: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
	fi
	question_hint
	if [ "$TTY" != yes ]; then
		INTERNAL=ro
		printf '%s\n' "$(pick "ポメラの内蔵文書を Linux でどう使いますか（1 読むだけ〈Enter 時の既定〉／2 編集・保存も行う／3 起動時に自動で使う設定を解除する）[1/2/3] → 1（既定の答え、${why}）" "How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3] -> 1 (the default, $why)")"
		return
	fi
	while :; do
		printf '%s ' "$(pick 'ポメラの内蔵文書を Linux でどう使いますか（1 読むだけ〈Enter 時の既定〉／2 編集・保存も行う／3 起動時に自動で使う設定を解除する）[1/2/3]:' 'How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]:')"
		if ! read -r a; then a=; echo; fi
		case $a in
		''|1) INTERNAL=ro; return ;;
		3) INTERNAL=no; return ;;
		2)
			# Already read-write: no need to explain and confirm the risk again.
			if [ "$(sh "$PDIR/pomera_mount_internal.sh" persisted-mode 2>/dev/null)" = rw ]; then
				say "今も読み書きの設定です。そのまま続けます。" "Read-write is already set; keeping it."
				INTERNAL=rw; return
			fi
			if [ "$MSG" = ja ]; then cat <<EOF
読み書きを選ぶと Linux から内蔵メモリに書けます。純正のポメラのモードでも同じファイルが見えます。
Pomera が止まった時に書いていたファイルは、戻らないことがあります。
起動時は「使用中」の印だけを見ます。前回正常に終了しなかった印がある場合だけ、
起動後に fsck.fat -n（読むだけ）で点検します。
点検中は内蔵文書を使えるようになるまで待つ場合があります。
dosfstools が純正 OS の Shift_JIS の短い名前を誤って改名するため、自動修復はしません。
純正 OS の名前の付け方など、問題ないと確認済みの指摘だけなら読み書きを許可し、
それ以外は読むだけにして、
ログイン時と FDTP 起動時に結果を知らせます。
点検には dosfstools（fsck.fat）が要ります。無い時は sudo apt install dosfstools
やめるには: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
			else cat <<EOF
Read-write lets Linux write to the internal memory. The same files are visible
in the stock Pomera mode. A file being written when the Pomera stops may be lost.
At boot only the in-use flag is checked. Only if it indicates an unclean shutdown,
fsck.fat -n checks the memory after boot without writing to it.
You may need to wait for the check before the internal documents become available.
There is no automatic repair: dosfstools can incorrectly rename Shift_JIS short
names used by the stock OS. Read-write access is allowed only for findings known
to be harmless, such as stock OS naming conventions; otherwise access is read-only. The result is shown at login and when FDTP starts.
The check needs dosfstools (fsck.fat): sudo apt install dosfstools
To stop it: sudo sh $PDIR/pomera_mount_internal.sh unpersist
EOF
			fi
			ask_yn n "内蔵メモリの文書を Linux から編集・保存できる設定にしますか？
はいの場合：読み書きを許可
いいえの場合：使い方の選択に戻る
入力:" "Allow Linux to edit and save internal documents?
If yes: allow read-write access
If no: return to the access choices
Answer:"
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
	ask_yn y "HEIC の写真を開く部品 libheif を入れますか？
はいの場合：apt で導入
いいえの場合：導入せず HEIC は開けない
入力:" "Install libheif to open HEIC photos?
If yes: install through apt
If no: skip installation; HEIC cannot be opened
Answer:"
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

# Default remains configurable while the release choice is being confirmed.
POMERA_PDCHARGE_DEFAULT=${POMERA_PDCHARGE_DEFAULT:-y}
# The configure probe can be replaced in tests; the installer and daemon
# always repeat their own hardware checks before making any change.
pdcharge_probe() {
	if [ -n "${FDTP_CONFIGURE_PDCHARGE_PROBE:-}" ]; then
		(eval "$FDTP_CONFIGURE_PDCHARGE_PROBE")
	else
		sh "$PDIR/pomera_pdcharge.sh" probe
	fi
}
ask_pdcharge() {
	PDCHARGE=keep
	say "== PD ハブ経由の充電（Pomera DM250 / RK818 だけ） ==" "== Charging through a PD hub (Pomera DM250 / RK818 only) =="
	if ! pdcharge_probe; then
		say "安全の点検を通らないため飛ばします。前の設定は変えません。" "Safety check did not pass: skipped; earlier settings are kept."
		return
	fi
	if [ "$MSG" = ja ]; then cat <<EOF
USB 接続中、充電に使う入力電流の上限が 2.0 A 未満なら 2.0 A に引き上げます。
すでに 2.0 A 以上なら変えません。ハブの有無は判別しません。
有効な間は、PC の USB ポートなど給電能力の低い接続先では使わないでください。
充電 LED: 赤＝充電中、緑＝満充電、赤の点滅＝つないでいるのに放電。
制御プログラムが裏で動き続け、CPU を約 4% 使います。
充電ランプ（LED）の制御には追加部品 python3-libgpiod が必要です。無ければランプだけ使いません。
やめるには: sudo sh $PDIR/pomera_pdcharge.sh remove
確かめたのは ekesete.net の D250 用 Debian 13.7（カーネル 6.18.41-g653d8276acb7）です。
イメージ（カーネル）が新しくなると、要らなくなったり合わなくなったりするかもしれません。
「いいえ」は前の設定を外します。
EOF
	else cat <<EOF
While USB is connected, raise the charging input current limit to 2.0 A if it is below 2.0 A.
If it is already 2.0 A or higher, leave it unchanged. The program does not detect whether a hub is present.
While enabled, do not use connections with low power capacity, such as a PC USB port.
Charge LEDs: red = charging, green = full, blinking red = plugged in but discharging.
A control program keeps running in the background, using about 4% CPU.
Charge lamp (LED) control needs the additional component python3-libgpiod; without it only the lamps are disabled.
To stop: sudo sh $PDIR/pomera_pdcharge.sh remove
Checked on the ekesete.net D250 Debian 13.7 image (kernel 6.18.41-g653d8276acb7).
A newer image (kernel) may no longer need this, or it may not fit.
Answering no removes an earlier setting.
EOF
	fi
	pd_led=yes
	python3 -c 'import gpiod; from gpiod.line import Direction, Value' 2>/dev/null || {
		pd_led=no
		warn "python3-libgpiod がありません。LED だけ使えません。" "python3-libgpiod missing: only LEDs are unavailable."
	}
	if [ "$pd_led" = yes ]; then
		ask_yn "$POMERA_PDCHARGE_DEFAULT" "FDTP の PD ハブ充電・充電 LED の設定を有効にしますか？
はいの場合：有効
いいえの場合：以前の設定も解除
入力:" "Enable FDTP PD hub charging and charge LEDs?
If yes: enable
If no: remove earlier settings too
Answer:"
	else
		ask_yn "$POMERA_PDCHARGE_DEFAULT" "FDTP の PD ハブ充電を有効にしますか？
はいの場合：有効、充電 LED は部品不足で使えない
いいえの場合：以前の充電・LED 設定も解除
入力:" "Enable FDTP PD hub charging?
If yes: enable; charge LEDs unavailable without the component
If no: remove earlier charging and LED settings too
Answer:"
	fi
	PDCHARGE=$ANSWER
}

POMERA_CVE_DEFAULT=${POMERA_CVE_DEFAULT:-y}
cve_probe() {
	if [ -n "${FDTP_CONFIGURE_CVE_PROBE:-}" ]; then
		(eval "$FDTP_CONFIGURE_CVE_PROBE")
	else
		sh "$PDIR/pomera_cve_2026_98432.sh" probe
	fi
}
ask_cve() {
	CVE=keep
	say "== CVE-2026-98432 の対策（Pomera DM250 だけ） ==" "== CVE-2026-98432 mitigation (Pomera DM250 only) =="
	if ! cve_probe; then
		say "対象の機種・カーネルではないため飛ばします。前の設定は変えません。" "Machine or kernel is outside the affected versions: skipped; earlier settings are kept."
		return
	fi
	say "このカーネルには、特定の形式の通信データを受け取ると不具合が起きる脆弱性があります。該当するデータを受信時に遮断して対策します。" \
		"This kernel has a vulnerability triggered by receiving specially formed network data. The mitigation blocks that data on receipt."
	say "IPv4 TCP の IP オプション付きパケットを破棄します。通常の通信には IP オプションはありません。" \
		"Drop IPv4 TCP packets carrying IP options. Normal traffic never has IP options."
	say "RTP-MIDI・mDNS・LocalSend・ssh には影響しません。IGMP は通します。" \
		"RTP-MIDI, mDNS, LocalSend and ssh are unaffected. IGMP is allowed."
	say "通信の通過・遮断を設定する部品 nftables が必要です。無ければ入れます。既存のルールは残します。" \
		"Needs nftables, a component for allowing or blocking network traffic (installed if missing). Existing rules are kept."
	say "やめるには: sudo sh $PDIR/pomera_cve_2026_98432.sh remove" \
		"To stop: sudo sh $PDIR/pomera_cve_2026_98432.sh remove"
	say "このイメージのカーネルが 6.18.56 以降になれば不要です。Debian 純正カーネルは apt upgrade で直ります。" \
		"No longer needed once this image's kernel reaches 6.18.56 or newer. Debian-distributed kernels are fixed by apt upgrade."
	say "「いいえ」は前の設定を外します。" "Answering no removes an earlier setting."
	ask_yn "$POMERA_CVE_DEFAULT" "通信の脆弱性 CVE-2026-98432 への対策を有効にしますか？
はいの場合：対象データを遮断
いいえの場合：以前の対策も解除
入力:" "Enable the network vulnerability CVE-2026-98432 mitigation?
If yes: block affected data
If no: remove earlier mitigation too
Answer:"
	CVE=$ANSWER
}

# Own kernels contain the DMA, charging and CVE fixes. Only power saving
# is optional; remove old workarounds without touching the paired RSCE.
POMERA_WIFI_PS_DEFAULT=${POMERA_WIFI_PS_DEFAULT:-y}
ask_wifi_ps() {
	ask_yn "$POMERA_WIFI_PS_DEFAULT" "Wi-Fi の省電力をオンにしますか？
はいの場合：以前の省電力オフ設定も解除
いいえの場合：省電力をオフ
入力:" "Enable Wi-Fi power saving?
If yes: also remove earlier power-saving-off settings
If no: turn power saving off
Answer:"
	if [ "$ANSWER" = yes ]; then WIFI_PS=on; else WIFI_PS=off; fi
}
old_workaround_labels() {
	printf '%s\n' "$old_files" | while IFS= read -r old; do
		case $old in
		*/92-pomera-dma.rules) label=$(pick "機器の省電力に関する旧対策" "Old device power-saving workaround") ;;
		*/90-pomera-watchdog.conf) label=$(pick "停止時の自動再起動" "Automatic restart on a freeze") ;;
		pdcharge.service) label=$(pick "PD ハブ充電の旧対策" "Old PD hub charging workaround") ;;
		CVE-*) label=$(pick "通信の脆弱性への旧対策（通信を遮断する部品 nftables）" "Old network vulnerability workaround (nftables traffic filter)") ;;
		*) continue ;;
		esac
		printf '  %s: %s\n' "$label" "$old"
	done
}
ask_leftovers() {
	CLEAN_OLD=keep
	old_files=$(sh "$PDIR/pomera_stability.sh" present 2>/dev/null | \
		grep -E '^/etc/(udev/rules.d/92-pomera-dma.rules|systemd/system.conf.d/90-pomera-watchdog.conf)$' || true)
	if sh "$PDIR/pomera_pdcharge.sh" status >/dev/null 2>&1; then old_files="$old_files
pdcharge.service"; fi
	if sh "$PDIR/pomera_cve_2026_98432.sh" status >/dev/null 2>&1; then old_files="$old_files
CVE-2026-98432 (nftables)"; fi
	[ -n "$old_files" ] || return 0
	say "現在のカーネル（配布元: https://www.ekesete.net/log/?p=9565）向けの回避策が残っています:" "Workarounds for the current kernel (distributed at https://www.ekesete.net/log/?p=9565) remain:"
	old_labels=$(old_workaround_labels)
	ask_yn y "現在のカーネル（配布元: https://www.ekesete.net/log/?p=9565）向けの次の追加対策は、pomera-kernel（試験版）では要りません。
$old_labels
これらを解除しますか？
はいの場合：一覧の追加対策を解除します。
いいえの場合：一覧の追加対策を残します。
どちらの場合も、直前に選んだ Wi-Fi の省電力設定は残します。
入力:" "pomera-kernel (test release) does not need these additional workarounds for the current kernel (distributed at https://www.ekesete.net/log/?p=9565):
$old_labels
Remove these workarounds?
If yes: remove the listed additional workarounds.
If no: keep the listed additional workarounds.
Either way, keep the Wi-Fi power-saving setting just selected.
Answer:"
	CLEAN_OLD=$ANSWER
}
ask_rtc() {
	RTC=keep
	[ "$(sed -n '3p' "${FDTP_CONFIGURE_ADJTIME:-/etc/adjtime}" 2>/dev/null)" = LOCAL ] || return 0
	say "ポメラ内蔵の時計（RTC）は協定世界時（UTC）を保存しますが、/etc/adjtime は現地時間として読む設定（LOCAL）です。ネットにつながずに起動すると時刻がずれる原因になります（日本時間の場合は 9 時間）。timedatectl set-local-rtc 0 で読み取り方を UTC に変えます。戻すには: sudo timedatectl set-local-rtc 1" \
		"The Pomera hardware clock (RTC) stores Coordinated Universal Time (UTC), but /etc/adjtime is set to read local time (LOCAL). This can shift the time when booting without a network (9 hours for Japan time). timedatectl set-local-rtc 0 changes the interpretation to UTC. To undo: sudo timedatectl set-local-rtc 1"
	ask_yn y "時刻のずれを直すため、内蔵時計を UTC として読み取る設定に変えますか？
はいの場合：変更
いいえの場合：現在の設定を残す
入力:" "Read the hardware clock as UTC to correct the time shift?
If yes: change
If no: keep the current setting
Answer:"
	[ "$ANSWER" != yes ] || RTC=yes
}

POMERA_KERNEL_APT_DEFAULT=${POMERA_KERNEL_APT_DEFAULT:-n}
ask_kernel_apt() {
	KERNEL_APT=keep
	if own_kernel; then
		if ! sh "$PDIR/pomera_kernel_apt.sh" status >/dev/null 2>&1; then KERNEL_APT=source; fi
		return
	fi
	kernel_intro
	say "== pomera-kernel（試験版、カーネルと U-Boot の組）の apt 導入 ==" "== Install pomera-kernel (test release, the kernel and U-Boot as a set) through apt =="
	say "pomera-kernel（試験版）は、カーネルと U-Boot（電源を入れて最初に動く起動用のプログラム）を組で入れ替えます。組で試験した物なので、片方だけは入れ替えません。" \
		"pomera-kernel (test release) replaces the kernel and U-Boot (the boot program that runs first at power-on) as one set. They were tested together, so one is never replaced without the other."
	say "apt でソフトを入れるための配布元と、配布物の署名を確認する公開鍵を登録し、pomera-kernel（試験版）を入れます。" \
		"Register a software distribution source for apt and a public key to verify package signatures, then install pomera-kernel (test release)."
	say "次回起動で新しいカーネルを試し、正常と判定されたら U-Boot も書き換えて、以後は組で使います。試し起動に失敗した場合は自動で元に戻ります（U-Boot は書き換えません）。使い始めた後に手動で組ごと戻すには: sudo pomera-kernel rollback" \
		"The new kernel is tried on the next boot; if judged successful, U-Boot is also rewritten and the set is used from then on. If the trial boot fails, it rolls back automatically (U-Boot is not rewritten). To roll back the whole set manually after starting to use it: sudo pomera-kernel rollback"
	say "Linux 6.18 系の長期保守版の修正を取り込み、配布元では週 1 回、新版を公開します。ポメラへの更新は apt で行います。自動で再起動はしません。" \
		"It incorporates fixes from the Linux 6.18 longterm series. The distribution source publishes a new version weekly; updates on your Pomera are installed through apt. It does not reboot automatically."
	say "保証はありません。試験版なので、自己責任でお使いください。本体・SD・データに不具合や損失が起きても責任を負いません。キングジムや配布元イメージとは関係のない非公式の物です。入れる前に、起動用部分も含む SD カード全体のバックアップを取ってください。" \
		"No warranty. This is a test release; use it at your own risk. We accept no responsibility for any trouble or loss to the device, the SD card or your data. It is unofficial and not related to King Jim or the image's distributor. Before installing, back up the entire SD card, including its boot areas."
	say "配布元の登録と公開鍵を外すには: sudo sh $PDIR/pomera_kernel_apt.sh remove（カーネルは外しません）" \
		"To remove the source registration and public key: sudo sh $PDIR/pomera_kernel_apt.sh remove (keeps the kernel installed)"
	ask_yn "$POMERA_KERNEL_APT_DEFAULT" "pomera-kernel（試験版、カーネルと U-Boot の組）を apt で入れ、次回起動で試しますか？
はいの場合：導入
いいえの場合：導入しない、既存設定を残す
入力:" "Install pomera-kernel (test release, the kernel and U-Boot as a set) through apt and try it on the next boot?
If yes: install
If no: do not install; keep existing settings
Answer:"
	KERNEL_APT=$ANSWER
}

# Both entry points keep the author's question order and messages.
ask_pomera() {
	WIFI_PS=keep CLEAN_OLD=keep RTC=keep
	if own_kernel; then
		kernel_intro
		STABILITY=no PDCHARGE=keep CVE=keep
		say "pomera-kernel（試験版）には Wi-Fi の不具合への対策が含まれるため、FDTP 側の追加対策と CPU が転送する方式（PIO）への変更は行いません。" "pomera-kernel (test release) includes Wi-Fi fixes, so FDTP does not add workarounds or switch to CPU-driven transfers (PIO)."
		say "PD ハブ経由の充電も pomera-kernel（試験版）側で対応するため、FDTP の追加充電プログラムは設定しません。" "pomera-kernel (test release) also handles PD hub charging, so FDTP does not set up its additional charging program."
		say "pomera-kernel（試験版）には通信の脆弱性 CVE-2026-98432 の修正が含まれるため、FDTP の追加対策は設定しません。" "pomera-kernel (test release) includes the fix for network vulnerability CVE-2026-98432, so FDTP does not set up its additional mitigation."
		ask_wifi_ps
		echo
		ask_leftovers
		echo
		ask_internal
	else
		ask_wifi
		echo
		ask_internal
		echo
		ask_pdcharge
		echo
		ask_cve
	fi
	echo
	ask_rtc
	echo
	ask_kernel_apt
}
