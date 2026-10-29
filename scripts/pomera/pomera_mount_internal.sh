#!/bin/sh
# Pomera DM250 (Debian 13 on the SD): mount the stock OS's user storage on
# the internal eMMC, read-only (or, when asked, read-write) to read and write
# the texts of the stock mode.
#
# The 6.18 kernel does not see the stock eMMC's partitions (no
# /dev/mmcblk0pN: they are in Rockchip's own table). The offsets are those
# of the Japanese DM250 from mah-jp/pomera-dm250-backup-restore-tool
# (common.sh): "User Storage" at 866 MiB, 1280 MiB long. Whether the US
# model (DM250US) has it at the same place is not known: every command
# below stops when no FAT file system is found there. In the read-only modes
# a read-only loop device over that range is used; nothing is written to the
# eMMC.
#
#   sudo sh pomera_mount_internal.sh mount     -> /mnt/pomera-internal (read-only)
#   sudo sh pomera_mount_internal.sh umount
#   sudo sh pomera_mount_internal.sh probe     only show what the range holds
#   sudo sh pomera_mount_internal.sh fstype    only print its type (vfat, exfat; nothing
#                                              if none) and leave no loop device behind
#   sudo sh pomera_mount_internal.sh persist   mount it (read-only) at every boot: a line in /etc/fstab
#   sudo sh pomera_mount_internal.sh persist rw  mount it read-write at every boot,
#                                              after a check (pomera-internal.service)
#   sudo sh pomera_mount_internal.sh unpersist remove either of them
#   sudo sh pomera_mount_internal.sh boot      what pomera-internal.service runs
#   sh pomera_mount_internal.sh classify < out  known_only or damaged, from fsck.fat -n
#
# Read-write: fsck.fat -a must never run on this file system. The stock OS
# writes short names in Shift_JIS, and dosfstools takes a trail byte equal to
# '|' or '\' (as in ポ = 83 7C) for a bad character and renames the file
# (tried on a copy, 2026-10-29). So at boot only the dirty bit (boot sector
# byte 0x41, set by Linux while mounted read-write) is read. When it is set,
# fsck.fat -n runs; if it reports only those short names, the empty label and
# the dirty bit itself, the mount is read-write; otherwise read-only. The
# result goes to $STATE (key=value) for the login message
# (/etc/profile.d/pomera-internal.sh) and for FDTP. Nothing is repaired.
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
STATE_DIR=/var/lib/pomera-internal
STATE=$STATE_DIR/check
CONF=/etc/default/pomera-internal
UNIT=/etc/systemd/system/pomera-internal.service
PROFILE=/etc/profile.d/pomera-internal.sh
LIBDIR=/usr/local/lib/pomera-internal
OWNER=${SUDO_USER:-root}
[ -r $CONF ] && . $CONF

# The fsck.fat -n output on stdin: known_only when every finding is one of the
# known harmless ones, damaged otherwise.
classify() {
	awk '
	/^fsck\.fat [0-9]/ || /^Leaving filesystem unchanged/ || /: [0-9]+ files, [0-9]+\/[0-9]+ clusters$/ { next }
	/^\// { next }                                   # the path a finding is about
	/^  Bad short file name \(.*\\[23][0-7][0-7]/ { sjis = 1; next }
	sjis && (/^  Auto-renaming it\./ || /^  Renamed to FSCK[0-9]+\.[0-9]+/) { next }
	/^Label .. stored in boot sector is not valid\./ { label = 1; next }
	label && /^  Auto-removing label from boot sector\./ { next }
	/Dirty bit is set\. Fs was not properly unmounted/ { dirty = 1; next }
	dirty && /^ ?Automatically removing dirty bit\./ { next }
	/^There are differences between boot sector and its backup\./ { backup = 1; next }
	backup && /^This is mostly harmless\./ { next }
	backup && /^Differences: \(offset:original\/backup\)/ { next }
	backup && /^  65:0[01]\/0[01] *$/ { next }      # the dirty bit, which Linux sets only in the main one
	backup && /^  Not automatically fixing this\./ { next }
	/^Free cluster summary (wrong|uninitialized)/ { fsinfo = 1; next }  # only a hint, stale after a stop
	fsinfo && /^  Auto-correcting\./ { next }
	/^[ \t]*$/ { next }
	{ bad = 1; print "unknown: " $0 > "/dev/stderr" }
	END { print bad ? "damaged" : "known_only" }'
}
case ${1:-} in classify) classify; exit 0 ;; esac

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
write_state() { # state= mount= and the fsck output file
	mkdir -p $STATE_DIR
	{
		echo "state=$1"
		echo "mount=$2"
		echo "boot_id=$(cat /proc/sys/kernel/random/boot_id)"
		echo "time=$(date -Iseconds)"
		echo "fsck_output=$3"
	} > $STATE.new
	mv $STATE.new $STATE
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
	if [ "${2:-ro}" = rw ]; then
		[ -f $UNIT ] && { echo "already set up read-write"; exit 0; }
		type=$(fstype)
		[ "$type" = vfat ] || { echo "no FAT32 found at 866 MiB: nothing done" >&2; exit 1; }
		[ -x /usr/sbin/fsck.fat ] || { echo "fsck.fat is missing: sudo apt install dosfstools" >&2; exit 1; }
		if grep -q "$MNT" /etc/fstab; then
			findmnt $MNT >/dev/null && umount $MNT
			l=$(loop); [ -z "$l" ] || losetup -d "$l"
			sed -i -e "\|$MNT|d" -e '/^# pomera_mount_internal.sh:/d' /etc/fstab
		fi
		mkdir -p $LIBDIR
		cp "$0" $LIBDIR/pomera_mount_internal.sh
		echo "OWNER=$OWNER" > $CONF
		cat > $UNIT <<UNIT_END
[Unit]
Description=Check (read only) and mount the Pomera's internal memory at $MNT
After=local-fs.target systemd-udev-settle.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh $LIBDIR/pomera_mount_internal.sh boot
ExecStop=/bin/umount $MNT

[Install]
WantedBy=multi-user.target
UNIT_END
		cat > $PROFILE <<'PROFILE_END'
# pomera_mount_internal.sh: say so at login when the internal memory looked damaged
if [ -r /var/lib/pomera-internal/check ] && grep -q '^state=damaged' /var/lib/pomera-internal/check; then
	case ${LANG:-} in
	ja*) echo "注意: 起動時の点検で、内蔵メモリ（/mnt/pomera-internal）に破損が見つかりました。読み取り専用でつないでいます。"
	     echo "      点検の結果: /var/lib/pomera-internal/fsck.txt（自動修復はしていません）" ;;
	*) echo "Note: the boot check found damage on the internal memory (/mnt/pomera-internal): it is mounted read-only."
	   echo "      The check's output: /var/lib/pomera-internal/fsck.txt (nothing was repaired)" ;;
	esac
fi
PROFILE_END
		systemctl daemon-reload
		systemctl enable --now pomera-internal.service
		cat $STATE
		exit 0
	fi
	if grep -q "$MNT" /etc/fstab; then echo "already in /etc/fstab"; exit 0; fi
	# From read-write back to read-only: the service goes first (both would
	# mount the same place).
	if [ -f $UNIT ]; then
		findmnt $MNT >/dev/null && umount $MNT
		sh "$0" unpersist
	fi
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
	done=
	if grep -q "$MNT" /etc/fstab; then
		sed -i -e "\|$MNT|d" -e '/^# pomera_mount_internal.sh:/d' /etc/fstab
		done="removed from /etc/fstab"
	fi
	if [ -f $UNIT ]; then
		systemctl disable pomera-internal.service >/dev/null 2>&1 || true
		rm -f $UNIT $PROFILE $CONF
		rm -rf $LIBDIR
		done="${done:+$done; }removed pomera-internal.service and the login message"
	fi
	[ -n "$done" ] || { echo "not set up: nothing done"; exit 0; }
	systemctl daemon-reload
	echo "$done (still mounted until umount or reboot)"
	;;
persisted) grep -q "$MNT" /etc/fstab || [ -f $UNIT ] ;;
boot)
	# What pomera-internal.service runs: check (read only), then mount.
	findmnt $MNT >/dev/null && { echo "already mounted"; exit 0; }
	l=$(losetup -r -o $OFFSET --sizelimit $SIZE -f --show $DEV)
	type=$(blkid -p -o value -s TYPE "$l" 2>/dev/null || true)
	out=$STATE_DIR/fsck.txt
	mkdir -p $STATE_DIR
	if [ "$type" != vfat ]; then
		losetup -d "$l"; echo "no FAT file system found at 866 MiB" > $out
		write_state damaged none $out; echo "no FAT file system: not mounted" >&2; exit 1
	fi
	dirty=$(( $(dd if="$l" bs=1 skip=65 count=1 2>/dev/null | od -An -tu1) & 1 ))
	if [ $dirty = 0 ]; then
		state=clean; : > $out
	elif command -v fsck.fat >/dev/null 2>&1 || [ -x /usr/sbin/fsck.fat ]; then
		PATH=$PATH:/usr/sbin fsck.fat -n -c 932 "$l" > $out 2>&1 || true
		state=$(sh "$0" classify < $out 2>>$out)
	else
		echo "fsck.fat is missing (apt install dosfstools)" > $out; state=damaged
	fi
	losetup -d "$l"
	mkdir -p $MNT
	opts=uid=$(id -u "$OWNER"),gid=$(id -g "$OWNER"),codepage=932,iocharset=utf8,shortname=mixed
	if [ $state = damaged ]; then
		mount -t vfat -o ro,loop,offset=$OFFSET,sizelimit=$SIZE,$opts $DEV $MNT; m=ro
	else
		mount -t vfat -o rw,loop,offset=$OFFSET,sizelimit=$SIZE,$opts,flush,errors=remount-ro $DEV $MNT; m=rw
	fi
	write_state $state $m $out
	echo "dirty=$dirty state=$state mounted $m at $MNT"
	;;
umount)
	umount $MNT 2>/dev/null || true
	l=$(loop); [ -z "$l" ] || losetup -d "$l"
	echo "unmounted"
	;;
*) sed -n '2,38p' "$0"; exit 2 ;;
esac
