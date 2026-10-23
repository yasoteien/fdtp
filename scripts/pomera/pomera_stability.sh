#!/bin/sh
# Pomera DM250 (Debian 13, kernel 6.18): keeps the SD, the eMMC and the
# Wi-Fi from freezing together, and reboots by itself if they do.
#
# What was seen on the author's DM250: after many ssh connections and a
# fast large transfer over the Wi-Fi, the kernel logged many lines of
#   dma-pl330 20078000.dma-controller: Runtime PM usage count underflow!
# and nothing after. The one PL330 DMA controller serves all three MMC
# hosts (device tree: mmc@10214000 = the SD with /, mmc@10218000 = the
# SDIO Wi-Fi brcmfmac, mmc@1021c000 = the eMMC), so when its runtime power
# management miscounts and it stops, the network, / and the journal stop at
# once; `sudo reboot` hangs on the disk too.
#
# Measures (all can be undone with `remove`):
#   1. The PL330 and the SD/eMMC hosts stay powered (runtime PM off,
#      power/control=on): the counter that underflows is no longer used.
#      Costs a little battery.
#   2. Wi-Fi power saving off (brcmfmac with power save is known to stall
#      on SDIO): NetworkManager wifi.powersave=2, and iw now.
#   3. The hardware watchdog (2004c000.watchdog): systemd pings it every
#      RuntimeWatchdogSec/2; if PID 1 stops, the board resets. A reboot that
#      hangs is forced after RebootWatchdogSec.
# Not done: the kernel has no hung-task detector, so a freeze that leaves
# PID 1 running is not caught by 3. pomera_dma_pio.sh wifi (the Wi-Fi
# without the DMA) is the other half.
#
#   sudo sh pomera_stability.sh install   write the files and apply now
#   sh pomera_stability.sh check          show the state (no root needed)
#   sudo sh pomera_stability.sh remove    delete the files (takes effect at the next boot)
#   sh pomera_stability.sh present        print the files that are there; exit 1 if none
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DMA=/sys/devices/platform/20078000.dma-controller
HOSTS="1021c000.mmc 10214000.mmc 10218000.mmc"
UDEV=/etc/udev/rules.d/92-pomera-dma.rules
NM=/etc/NetworkManager/conf.d/90-pomera-wifi-powersave.conf
WD=/etc/systemd/system.conf.d/90-pomera-watchdog.conf

check() {
	echo "PL330 power/control: $(cat $DMA/power/control) ($(cat $DMA/power/runtime_status))"
	for h in $HOSTS; do echo "$h power/control: $(cat /sys/devices/platform/$h/power/control)"; done
	echo "Wi-Fi: $(/usr/sbin/iw dev wlan0 get power_save 2>&1)"
	echo "watchdog: $(systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec | tr '\n' ' ')"
	echo "underflow since boot: $(journalctl -b 0 -k -q --no-pager 2>/dev/null | grep -c 'usage count underflow' || true)"
	for f in $UDEV $NM $WD; do if [ -f $f ]; then echo "file: $f"; else echo "missing: $f"; fi; done
}
pomera() { # the RK3128's DMA and MMC hosts are where this expects them
	[ -d $DMA ] && for h in $HOSTS; do [ -d /sys/devices/platform/$h ] || return 1; done
}

case ${1:-check} in
install)
	[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
	pomera || { echo "$DMA or the MMC hosts are missing: not a Pomera DM250 on Debian 13; nothing done" >&2; exit 1; }
	cat > $UDEV <<'EOF'
# pomera_stability.sh: the PL330 DMA and the SD/eMMC hosts stay powered
# (runtime PM usage count underflow froze the SD, the eMMC and the Wi-Fi).
ACTION=="add", SUBSYSTEM=="amba", KERNEL=="20078000.dma-controller", ATTR{power/control}="on"
ACTION=="add", SUBSYSTEM=="platform", KERNEL=="1021c000.mmc|10214000.mmc|10218000.mmc", ATTR{power/control}="on"
EOF
	mkdir -p /etc/NetworkManager/conf.d /etc/systemd/system.conf.d
	cat > $NM <<'EOF'
# pomera_stability.sh: brcmfmac's power saving off (2 = disable).
[connection]
wifi.powersave = 2
EOF
	cat > $WD <<'EOF'
# pomera_stability.sh: reset the board when PID 1 stops, and force a
# reboot that hangs.
[Manager]
RuntimeWatchdogSec=60s
RebootWatchdogSec=3min
EOF
	# Now, without a reboot.
	echo on > $DMA/power/control
	for h in $HOSTS; do echo on > /sys/devices/platform/$h/power/control; done
	/usr/sbin/iw dev wlan0 set power_save off 2>/dev/null || true
	systemctl reload NetworkManager 2>/dev/null || true
	systemctl daemon-reexec
	check
	;;
remove)
	[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
	if [ ! -e $UDEV ] && [ ! -e $NM ] && [ ! -e $WD ]; then echo "none of the files are there: nothing to remove"; exit 0; fi
	rm -f $UDEV $NM $WD
	echo "removed; reboot to go back to the old behaviour"
	;;
check) check ;;
present)
	found=1
	for f in $UDEV $NM $WD; do if [ -e $f ]; then echo $f; found=0; fi; done
	exit $found
	;;
*) sed -n '2,30p' "$0"; exit 2 ;;
esac
