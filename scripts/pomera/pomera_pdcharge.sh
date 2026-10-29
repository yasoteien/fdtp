#!/bin/sh
# Install/remove/status the RK818 PD hub charger. probe only reads hardware.
# FDTP_PDCHARGE_SYSFS and FDTP_CONFIGURE_DT replace hardware paths in tests.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/pomera_questions.sh"
msg_init
LIB=/usr/local/lib/pdcharge
UNIT=/etc/systemd/system/pdcharge.service
mode=${1:-install}
case $mode in install|remove|status|probe) ;; *) echo "usage: sh $0 install|remove|status|probe" >&2; exit 2 ;; esac
# remove and status only touch what install put there, so they skip the
# hardware checks: after a new image changes the driver it can still be removed.
if [ "$mode" = install ] || [ "$mode" = probe ]; then
is_dm250 "${FDTP_CONFIGURE_DT:-/proc/device-tree}" || {
	say "pdcharge: Pomera DM250 ではありません。" "pdcharge: not a Pomera DM250."; exit 1;
}
[ -d "${FDTP_PDCHARGE_SYSFS:-/sys}/bus/platform/drivers/rk818-charger" ] || {
	say "pdcharge: rk818-charger ドライバがありません。" "pdcharge: rk818-charger driver missing."; exit 1;
}
# Checked on the ekesete.net D250 Debian 13.7 image; the 450 mA write-back is
# its rk818-charger driver. Another kernel may not need this or may differ.
KNOWN_KERNEL=6.18.41-g653d8276acb7
[ "$(uname -r)" = "$KNOWN_KERNEL" ] || warn \
	"カーネル $(uname -r) は未確認です（確かめたのは $KNOWN_KERNEL）。まず pdcharge 無しで充電の様子を見てください。" \
	"kernel $(uname -r) is untested (checked: $KNOWN_KERNEL). First check charging without pdcharge."
# /dev/i2c-0 is root only: ./configure (a user) stops at the checks above;
# install and the daemon (root) also read the RK818 ID.
if [ "$(id -u)" = 0 ]; then
	python3 -B "$DIR/pdcharge.py" --probe || exit 1
fi
fi
[ "$mode" != probe ] || exit 0
if [ "$mode" = status ]; then
	[ -f "$UNIT" ] || exit 1
	systemctl --no-pager status pdcharge.service || true
	exit 0
fi
[ "$(id -u)" = 0 ] || { say "pdcharge: root が要ります。" "pdcharge: root is required." >&2; exit 1; }
if [ "$mode" = remove ]; then
	[ -f "$UNIT" ] || [ -f "$LIB/pdcharge.py" ] || exit 0
	systemctl disable --now pdcharge.service
	rm -f "$UNIT" "$LIB/pdcharge.py"
	[ ! -d "$LIB" ] || rmdir "$LIB"
	systemctl daemon-reload
	say "pdcharge を外しました。上限はドライバが 1.5 秒以内に戻します。" "pdcharge removed; the driver restores the limit within 1.5 seconds."
	exit 0
fi
# A running installed service can be updated; do not compete with a manual copy.
if ! systemctl is-active --quiet pdcharge.service && pgrep -f 'python3 .*pdcharge.py' >/dev/null; then
	say "pdcharge.py が動いています。止めてからもう一度。" "pdcharge.py is running; stop it first." >&2
	exit 1
fi
python3 -c 'import gpiod; from gpiod.line import Direction, Value' 2>/dev/null ||
	warn "python3-libgpiod がありません。LED だけ使いません。" "python3-libgpiod missing: only LEDs are disabled."
install -d "$LIB"
install -m 644 "$DIR/pdcharge.py" "$LIB/pdcharge.py"
install -m 644 "$DIR/pdcharge.service" "$UNIT"
systemctl daemon-reload
systemctl enable pdcharge.service
systemctl restart pdcharge.service
systemctl --no-pager status pdcharge.service
