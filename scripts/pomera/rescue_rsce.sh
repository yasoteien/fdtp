#!/bin/sh
# When the Pomera DM250 does not boot after pomera_dma_pio.sh wifi: put its
# SD in a card reader of another Linux machine and write the original
# resource image (the kernel's device tree) back at 24 MiB. Only that 1 MiB
# changes, and only when it is now one of the versions pomera_dma_pio.sh
# writes.
#
# The original 1 MiB is the rsce-backup-*.bin that pomera_dma_pio.sh wifi
# saved on the SD's second partition (ext4, the Pomera's /): next to
# itself, or in /var/lib/fdtp with the .deb (fdtp-pomera-setup).
# Copy it out first, for example:
#   sudo mount -o ro /dev/sdX2 /mnt        (the SD's 2nd partition)
#   cp /mnt/var/lib/fdtp/rsce-backup-*.bin .   (or /mnt/<the directory of pomera_dma_pio.sh>/)
#   sudo umount /mnt
# then, with the SD's whole device (not a partition) and that file:
#   sudo sh rescue_rsce.sh /dev/sdX rsce-backup-YYYYMMDDhhmmss.bin
# lsblk shows which /dev/sdX the card reader is. Nothing on the SD may be
# mounted while this runs.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
SEEK=49152
ORIG=bc0798fb393803cb4368134f121aa6f47ae4d541eb0b85ff03a7d7b6f6770573
WIFI=1a73651d8b786cf347d69e1ec20f661e8b13c396de6de92672bcc7decc541fb2
PIO=c9c32fc1b1f1d88929cfd38049a27e06a385beacfbb5e666b396e59d5f008e90

[ $# = 2 ] || { sed -n '2,18p' "$0"; exit 2; }
DEV=$1 SRC=$2
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device: stop" >&2; exit 1; }
[ "$(lsblk -dn -o TYPE "$DEV")" = disk ] || { echo "$DEV is not a whole disk (give /dev/sdX, not /dev/sdX1): stop" >&2; exit 1; }
if lsblk -rn -o MOUNTPOINT "$DEV" | grep -q .; then echo "something on $DEV is mounted: unmount it first. stop" >&2; exit 1; fi
[ "$(sha256sum "$SRC" | cut -d' ' -f1)" = "$ORIG" ] || { echo "$SRC is not the original 1 MiB: stop" >&2; exit 1; }
now() { dd if="$DEV" bs=512 skip=$SEEK count=2048 iflag=direct 2>/dev/null | sha256sum | cut -d' ' -f1; }
case $(now) in
"$ORIG") echo "$DEV already has the original; nothing to do"; exit 0 ;;
"$WIFI"|"$PIO") ;;
*) echo "$DEV's region at 24 MiB is not one that pomera_dma_pio.sh writes (wrong device?): stop" >&2; exit 1 ;;
esac
lsblk -o NAME,SIZE,MODEL,FSTYPE "$DEV"
printf 'Write %s at 24 MiB into %s? [y/N] ' "$SRC" "$DEV"
read -r answer || answer=
case $answer in y|Y|yes|YES) ;; *) echo "nothing done"; exit 1 ;; esac
dd if="$SRC" of="$DEV" bs=512 seek=$SEEK count=2048 conv=fsync,notrunc 2>/dev/null
sync
if [ "$(now)" = "$ORIG" ]; then echo "restored. Put the SD back in the Pomera."; else echo "read back differs" >&2; exit 1; fi
