#!/bin/sh
# Install/remove/status the IPv4 TCP options mitigation for CVE-2026-98432.
# probe only reads the DM250 DT and kernel version. Tests replace paths with
# FDTP_CVE_ROOT, commands with FDTP_CVE_NFT / FDTP_CVE_SYSTEMCTL /
# FDTP_CONFIGURE_APT, and uname -r with FDTP_CVE_UNAME_R.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/pomera_questions.sh"
msg_init
ROOT=${FDTP_CVE_ROOT:-}
CONF=$ROOT/etc/nftables.conf
RULE=$ROOT/etc/nftables.d/fdtp-cve-2026-98432.nft
FLAG=$ROOT/var/lib/fdtp/cve-2026-98432-enabled
INCLUDE='include "/etc/nftables.d/fdtp-cve-2026-98432.nft"'
nft=${FDTP_CVE_NFT:-nft}
systemctl=${FDTP_CVE_SYSTEMCTL:-systemctl}
mode=${1:-install}
case $mode in install|remove|status|probe) ;; *) echo "usage: sh $0 install|remove|status|probe" >&2; exit 2 ;; esac
# A new image must still be able to inspect and remove an earlier setting.
if [ "$mode" = install ] || [ "$mode" = probe ]; then
	is_dm250 "${FDTP_CONFIGURE_DT:-/proc/device-tree}" || {
		say "CVE-2026-98432: Pomera DM250 ではありません。" "CVE-2026-98432: not a Pomera DM250."; exit 1;
	}
	kernel=${FDTP_CVE_UNAME_R:-$(uname -r)}
	if printf '%s\n' "$kernel" | awk -F. '
		$1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ { exit 1 }
		{
			major = $1 + 0; minor = $2 + 0
			patch = $3; sub(/[^0-9].*$/, "", patch)
			if (NF >= 3 && patch == "") exit 1
			if (patch == "") patch = 0
			if (major < 4 || (major == 4 && minor < 4)) exit 2
			if (major > 7 || (major == 7 && minor >= 3)) exit 3
			series = major "." minor
			fixed["5.10"] = 272; fixed["5.15"] = 223; fixed["6.1"] = 190
			fixed["6.6"] = 159; fixed["6.12"] = 113; fixed["6.18"] = 56; fixed["7.2"] = 10
			if (series in fixed && patch + 0 >= fixed[series]) exit 3
		}'
	then :
	else
		case $? in
		2) say "CVE-2026-98432: カーネル $kernel は影響を受けません。" "CVE-2026-98432: kernel $kernel is unaffected." ;;
		3) say "CVE-2026-98432: カーネル $kernel は修正済みです。" "CVE-2026-98432: kernel $kernel is fixed." ;;
		*) say "CVE-2026-98432: カーネル $kernel の版を判定できません。" "CVE-2026-98432: unrecognized kernel version $kernel." ;;
		esac
		exit 1
	fi
fi
[ "$mode" != probe ] || exit 0
if [ "$mode" = status ]; then
	if [ -f "$RULE" ] && [ -f "$CONF" ] && grep -Fqx "$INCLUDE" "$CONF"; then
		say "CVE-2026-98432: IPv4 TCP の IP オプションを破棄します（$RULE）。" \
			"CVE-2026-98432: drop IPv4 TCP packets with IP options ($RULE)."
		exit 0
	fi
	say "CVE-2026-98432: 設定されていません。" "CVE-2026-98432: not installed."
	exit 1
fi
[ "${FDTP_CONFIGURE_UID:-$(id -u)}" = 0 ] || { say "CVE-2026-98432: root が要ります。" "CVE-2026-98432: root is required." >&2; exit 1; }
if [ "$mode" = remove ]; then
	[ -f "$RULE" ] || [ -f "$FLAG" ] || { [ -f "$CONF" ] && grep -Fqx "$INCLUDE" "$CONF"; } || exit 0
	if [ -f "$CONF" ]; then
		awk -v line="$INCLUDE" '$0 != line' "$CONF" > "$CONF.tmp"
		cat "$CONF.tmp" > "$CONF"
		rm -f "$CONF.tmp"
	fi
	rm -f "$RULE"
	"$nft" delete table ip pomera_cve 2>/dev/null || true
	if [ -f "$FLAG" ]; then
		"$systemctl" disable nftables.service
		rm -f "$FLAG"
	else
		"$systemctl" restart nftables.service
	fi
	say "CVE-2026-98432 の設定を外しました。" "CVE-2026-98432 mitigation removed."
	exit 0
fi
enabled_before=no
if "$systemctl" is-enabled --quiet nftables.service; then enabled_before=yes; fi
if ! command -v "$nft" >/dev/null 2>&1; then
	apt=${FDTP_CONFIGURE_APT:-apt-get}
	$apt install -y nftables
fi
install -d "$ROOT/etc/nftables.d" "$ROOT/var/lib/fdtp"
if [ -f "$CONF" ] && [ ! -e "$CONF.bak-fdtp-cve" ]; then cp -p "$CONF" "$CONF.bak-fdtp-cve"; fi
# The hand-made script wrote a shebang before its marker. Accept that form
# too, and restore its original rules before adding our separate include.
if [ -f "$CONF" ] && sed -n '1,2p' "$CONF" | grep -q '^# pomera_cve_2026_98432: written by pomera_cve_2026_98432.sh'; then
	if [ -f "$CONF.orig" ]; then cp -p "$CONF.orig" "$CONF"; else : > "$CONF"; fi
fi
[ -f "$CONF" ] || printf '%s\n' '#!/usr/sbin/nft -f' > "$CONF"
cat > "$RULE" <<'RULES'
table ip pomera_cve
delete table ip pomera_cve
table ip pomera_cve {
	chain pre {
		type filter hook prerouting priority raw; policy accept;
		ip protocol tcp ip hdrlength != 5 counter drop
	}
}
RULES
if ! grep -Fqx "$INCLUDE" "$CONF"; then printf '\n%s\n' "$INCLUDE" >> "$CONF"; fi
"$nft" -c -f "$CONF"
# Keep ownership of enablement across repeated installs.
if [ "$enabled_before" = no ]; then : > "$FLAG"; fi
"$systemctl" enable nftables.service
"$systemctl" restart nftables.service
"$nft" list table ip pomera_cve
