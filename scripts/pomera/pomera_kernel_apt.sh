#!/bin/sh
# Install the signed Pomera kernel apt source, optionally install the kernel,
# or inspect/remove only the source and public key (never remove the kernel).
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# FDTP_KERNEL_APT_ROOT prefixes system paths for tests; POMERA_KERNEL_APT_URL
# replaces the repository URL and FDTP_CONFIGURE_APT replaces apt-get.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/pomera_questions.sh"
msg_init
ROOT=${FDTP_KERNEL_APT_ROOT:-}
KEY=$ROOT/usr/share/keyrings/pomera-kernel.asc
SOURCE=$ROOT/etc/apt/sources.list.d/pomera-kernel.sources
URL=${POMERA_KERNEL_APT_URL:-https://yasoteien.github.io/pomera-kernel-apt/}
apt=${FDTP_CONFIGURE_APT:-apt-get}
mode=${1:-install}
case $mode in
status) [ -e "$KEY" ] || [ -e "$SOURCE" ]; exit $? ;;
install|source|remove) ;;
*) echo "usage: sh $0 install|source|remove|status" >&2; exit 2 ;;
esac
[ "${FDTP_CONFIGURE_UID:-$(id -u)}" = 0 ] || {
	say "カーネルの apt の設定には root が要ります。" "The kernel apt settings need root." >&2; exit 1;
}
if [ "$mode" = remove ]; then
	rm -f "$KEY" "$SOURCE"
	exit 0
fi
command -v gpg >/dev/null 2>&1 || {
	say "公開鍵の指紋を確かめるには gpg が要ります。" "Checking the public key fingerprint needs gpg." >&2; exit 1;
}
# Read the bundled key without importing keys or using the user's keyring.
# An empty, throwaway home: $DIR is the package's own (read-only under /usr).
gpghome=$(mktemp -d)
key_info=$(gpg --batch --no-options --no-keyring --trust-model always --no-auto-check-trustdb --lock-never \
	--homedir "$gpghome" --show-keys --with-colons "$DIR/pomera-kernel.asc") || { rm -rf "$gpghome"; exit 1; }
rm -rf "$gpghome"
fingerprint=$(printf '%s\n' "$key_info" | awk -F: '$1 == "pub" { primary=1; next } $1 == "fpr" && primary { print $10; primary=0 }')
[ "$fingerprint" = E9B307FAF435923A22E2A7109110B3116ECA5467 ] || {
	say "公開鍵の指紋が一致しません。何も設定しません。" "Public key fingerprint mismatch: nothing was set up." >&2; exit 1;
}
install -d "$(dirname "$KEY")" "$(dirname "$SOURCE")"
install -m 644 "$DIR/pomera-kernel.asc" "$KEY"
cat > "$SOURCE" <<EOF
Types: deb
URIs: $URL
Suites: stable
Components: main
Architectures: armhf
Signed-By: /usr/share/keyrings/pomera-kernel.asc
EOF
chmod 644 "$SOURCE"
if [ "$mode" = install ]; then
	before=$(dpkg-query -W -f='${Version}' pomera-kernel 2>/dev/null || true)
	$apt update
	$apt install -y pomera-kernel
	after=$(dpkg-query -W -f='${Version}' pomera-kernel 2>/dev/null || true)
	# Nothing new was installed (the newest is already there): no trial waits.
	if [ -n "$before" ] && [ "$before" = "$after" ]; then
		say "pomera-kernel（試験版）はすでに最新（$after）です。新しく試すカーネルはありません。" \
			"pomera-kernel (test release) is already the newest ($after): there is no new kernel to try."
		exit 0
	fi
	say "次回起動で pomera-kernel（試験版）を試し、正常と判定されたら以後も使います。試し起動に失敗した場合は自動で元に戻ります。使い始めた後に手動で戻すには sudo pomera-kernel rollback" \
		"On reboot, pomera-kernel (test release) is tried and kept for later boots if judged successful. A failed trial boot rolls back automatically. To roll back manually after starting to use it: sudo pomera-kernel rollback"
	say "新しいカーネルで起動した後、ソース版では ./configure で旧対策を解除すると答え、sudo make install を実行してください。.deb 版では sudo fdtp-pomera-setup を実行してください。" \
		"After booting the new kernel, for the source version answer yes to removing old workarounds in ./configure, then run sudo make install. For the .deb version, run sudo fdtp-pomera-setup."
	# What happens after the reboot, step by step (user, 2026-11-04).
	say "再起動後の流れです。" "What happens after reboot:"
	say "  1. まず、現在のカーネル（配布元: https://www.ekesete.net/log/?p=9565）で起動します。" "  1. The Pomera first starts with the current kernel (distributed at https://www.ekesete.net/log/?p=9565)."
	say "  2. 起動の途中で pomera-kernel（試験版）に移ります。画面が一度消えて、また出ます。" \
		"  2. During that start it switches to pomera-kernel (test release); the screen goes dark once and comes back."
	say "  正常に起動した場合：約 2 分後に自動で確定し、U-Boot も書き換えます。以後はカーネルと U-Boot の組で起動します。" \
		"  If it starts normally: it is confirmed by itself about 2 minutes later and U-Boot is rewritten too; from then on the Pomera starts with the kernel and U-Boot as a set."
	say "    確定したかは sudo pomera-kernel status で確認できます。確定した後に、上の手順で、要らなくなった追加対策を解除してください。" \
		"    Check confirmation with sudo pomera-kernel status. Once confirmed, remove the workarounds it no longer needs as described above."
	say "  試し起動に失敗した場合：約 5 分以内に自動で現在のカーネル（配布元: https://www.ekesete.net/log/?p=9565）に戻ります（U-Boot は書き換えません）。" \
		"  If the trial boot fails: the Pomera returns automatically to the current kernel (distributed at https://www.ekesete.net/log/?p=9565) within about 5 minutes (U-Boot is not rewritten)."
	say "再起動してください（sudo reboot）。" "Please reboot (sudo reboot)."
fi
