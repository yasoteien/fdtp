#!/bin/sh
# Pomera DM250 (Debian 13 on the SD): mount the stock OS's user storage on
# the internal eMMC, read-only, to read the texts written in the stock mode.
#
# The 6.18 kernel does not see the stock eMMC's partitions (no
# /dev/mmcblk0pN: they are in Rockchip's own table). The offsets are those
# of the Japanese DM250 from mah-jp/pomera-dm250-backup-restore-tool
# (common.sh): "User Storage" at 866 MiB, 1280 MiB long. Whether the US
# model (DM250US) has it at the same place is not known: every command
# below stops when no FAT file system is found there. A read-only loop
# device over that range is used; nothing is written to the eMMC.
#
#   sudo sh pomera_mount_internal.sh mount     -> /mnt/pomera-internal (read-only)
#   sudo sh pomera_mount_internal.sh umount
#   sudo sh pomera_mount_internal.sh probe     only show what the range holds
#   sudo sh pomera_mount_internal.sh fstype    only print its type (vfat, exfat; nothing
#                                              if none) and leave no loop device behind
#   sudo sh pomera_mount_internal.sh persist   mount it (read-only) at every boot: a line in /etc/fstab
#   sudo sh pomera_mount_internal.sh unpersist remove that line
#   sudo sh pomera_mount_internal.sh persisted exit 0 when that line is there
# The fstab line lets mount(8) set up the read-only loop itself (loop,offset,
# sizelimit); nofail so a missing or changed eMMC never stops the boot.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DEV=/dev/mmcblk0
OFFSET=$((866 * 1024 * 1024))
SIZE=$((1280 * 1024 * 1024))
MNT=/mnt/pomera-internal
OWNER=${SUDO_USER:-root}

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -b $DEV ] || { echo "$DEV is missing: no internal eMMC here" >&2; exit 1; }
loop() { losetup -j $DEV -o $OFFSET | cut -d: -f1 | head -1; }
attach() {
	l=$(loop)
	[ -n "$l" ] || l=$(losetup -r -o $OFFSET --sizelimit $SIZE -f --show $DEV)
	echo "$l"
}
fstype() { # the FAT type at the offset, or nothing; the loop device is left as it was
	had=$(loop)
	l=$(attach)
	t=$(blkid -p -o value -s TYPE "$l" 2>/dev/null || true)
	[ -n "$had" ] || losetup -d "$l"
	case $t in vfat|exfat) echo "$t" ;; esac
}

case ${1:-probe} in
probe)
	l=$(attach)
	echo "loop: $l (read-only: $(cat /sys/block/${l#/dev/}/ro))"
	blkid -p "$l" || echo "no file system found at 866 MiB"
	;;
fstype) fstype ;;
mount)
	l=$(attach)
	type=$(blkid -p -o value -s TYPE "$l" || true)
	case $type in
	vfat|exfat) ;;
	*) echo "no FAT file system found at 866 MiB: stop" >&2; losetup -d "$l"; exit 1 ;;
	esac
	mkdir -p $MNT
	mount -o ro,uid=$(id -u "$OWNER"),gid=$(id -g "$OWNER"),iocharset=utf8 "$l" $MNT
	echo "mounted $type read-only at $MNT"
	;;
persist)
	if grep -q "$MNT" /etc/fstab; then echo "already in /etc/fstab"; exit 0; fi
	type=$(fstype)
	[ -n "$type" ] || { echo "no FAT file system found at 866 MiB: nothing done" >&2; exit 1; }
	cp -p /etc/fstab /etc/fstab.bak-internal
	mkdir -p $MNT
	# fstab has no comments after the fields: the note goes on its own line.
	printf '%s\n%s\n' "# pomera_mount_internal.sh: the stock OS's user storage (eMMC), read-only" \
		"$DEV $MNT $type ro,loop,offset=$OFFSET,sizelimit=$SIZE,uid=$(id -u "$OWNER"),gid=$(id -g "$OWNER"),iocharset=utf8,nofail,x-systemd.device-timeout=10s 0 0" >> /etc/fstab
	systemctl daemon-reload
	findmnt $MNT >/dev/null || mount $MNT
	echo "in /etc/fstab (old one: /etc/fstab.bak-internal); mounted: $(findmnt -n -o OPTIONS $MNT | cut -c1-40)"
	;;
unpersist)
	grep -q "$MNT" /etc/fstab || { echo "not in /etc/fstab: nothing done"; exit 0; }
	sed -i -e "\|$MNT|d" -e '/^# pomera_mount_internal.sh:/d' /etc/fstab
	systemctl daemon-reload
	echo "removed from /etc/fstab (still mounted until umount or reboot)"
	;;
persisted) grep -q "$MNT" /etc/fstab ;;
umount)
	umount $MNT 2>/dev/null || true
	l=$(loop); [ -z "$l" ] || losetup -d "$l"
	echo "unmounted"
	;;
*) sed -n '2,22p' "$0"; exit 2 ;;
esac
