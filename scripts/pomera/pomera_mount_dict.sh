#!/bin/sh
# Pomera DM250 (Debian 13 on the SD): mount the stock OS's electronic
# dictionaries on the internal eMMC, read-only. No dictionary is copied.
#
# The kernel does not see the stock eMMC's Rockchip partitions. The Japanese
# DM250's p6 is ext4 at 610 MiB, 256 MiB long (mah-jp's common.sh).
# The US model may differ: probe must find ext4 and JDS/*.jds with SDSJ
# headers before persist writes anything. Every probe uses its own read-only
# loop and removes it, including on failure. noload prevents journal replay;
# there is no read-write mode and nothing is written to the eMMC.
#
#   sudo sh pomera_mount_dict.sh probe       check only; leave no loop behind
#   sudo sh pomera_mount_dict.sh mount       -> /mnt/pomera-dict (read-only)
#   sudo sh pomera_mount_dict.sh umount
#   sudo sh pomera_mount_dict.sh persist     read-only at every boot, in /etc/fstab
#   sudo sh pomera_mount_dict.sh unpersist   remove that line; still mounted
#   sh pomera_mount_dict.sh persisted        exit 0 when that line is there
# The fstab line lets mount(8) set up the read-only loop itself (loop,offset,
# sizelimit); nofail so a missing or changed eMMC never stops the boot.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DEV=/dev/mmcblk0
OFFSET=$((610 * 1024 * 1024))
SIZE=$((256 * 1024 * 1024))
MNT=/mnt/pomera-dict
[ "$#" -le 1 ] || { echo "no extra arguments (read-only only)" >&2; exit 2; }
persisted() { awk -v m="$MNT" '$1 !~ /^#/ && $2 == m { found=1 } END { exit !found }' /etc/fstab; }
probe() (
	# A private mount: do not reuse an existing loop, which might be writable.
	l= tmp= mounted=no
	cleanup() {
		if [ "$mounted" = yes ]; then umount "$tmp" || return 1; fi
		[ -z "$l" ] || losetup -d "$l" || return 1
		[ -z "$tmp" ] || rmdir "$tmp"
	}
	trap 'cleanup' EXIT
	trap 'exit 1' HUP INT TERM
	l=$(losetup -r -o "$OFFSET" --sizelimit "$SIZE" -f --show "$DEV") || exit 1
	[ "$(blkid -p -o value -s TYPE "$l")" = ext4 ] || exit 1
	tmp=$(mktemp -d /tmp/pomera-dict.XXXXXX) || exit 1
	mount -t ext4 -o ro,noload "$l" "$tmp" || exit 1
	mounted=yes
	[ -d "$tmp/JDS" ] && [ ! -L "$tmp/JDS" ] || exit 1
	found=no
	for jds in "$tmp"/JDS/*.jds; do
		[ -f "$jds" ] && [ ! -L "$jds" ] || continue
		[ "$(LC_ALL=C head -c 4 "$jds")" = SDSJ ] || exit 1
		found=yes
	done
	[ "$found" = yes ]
)
case ${1:-probe} in
persisted) persisted; exit ;; # Can read or remove the setting without an eMMC.
unpersist)
	[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
	if persisted; then
		sed -i -e "\\|^[^#][^ ]* $MNT |d" -e '/^# pomera_mount_dict.sh:/d' /etc/fstab
		systemctl daemon-reload
		echo "removed from /etc/fstab (still mounted until umount or reboot)"
	fi
	exit ;;
probe|mount|persist|umount) ;;
*) sed -n '2,23p' "$0"; exit 2 ;;
esac
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
case ${1:-probe} in
umount) umount "$MNT"; exit ;; # mount(8)'s loop is autocleared on unmount.
esac
[ -b "$DEV" ] || { echo "$DEV is missing: no internal eMMC here" >&2; exit 1; }
probe || { echo "no stock dictionaries found at 610 MiB: nothing done" >&2; exit 1; }
case ${1:-probe} in
probe) echo "ext4 and SDSJ dictionaries found at 610 MiB (read-only)" ;;
mount)
	mkdir -p "$MNT"
	mount -t ext4 -o loop,offset=$OFFSET,sizelimit=$SIZE,ro,noload "$DEV" "$MNT"
	;;
persist)
	if persisted; then echo "already in /etc/fstab"; exit 0; fi
	cp -p /etc/fstab /etc/fstab.bak-dict
	mkdir -p "$MNT"
	# fstab has no comments after the fields: the note goes on its own line.
	printf '%s\n%s\n' "# pomera_mount_dict.sh: the stock dictionaries (eMMC), read-only" \
		"$DEV $MNT ext4 loop,offset=$OFFSET,sizelimit=$SIZE,ro,noload,nofail 0 0" >> /etc/fstab
	systemctl daemon-reload
	findmnt -M "$MNT" >/dev/null || mount "$MNT"
	echo "in /etc/fstab (old one: /etc/fstab.bak-dict)"
	;;
esac
