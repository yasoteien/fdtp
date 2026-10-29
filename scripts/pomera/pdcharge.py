#!/usr/bin/python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Keep RK818 USB input at 2 A on a powered PD hub; optional charge LEDs.

Why: the image's rk818-charger driver (rk818_monitor_work) looks at the USB
type through extcon about every 1.5 seconds (DCP, CDP or other) and writes the
input limit back. Through a PD hub the type is not detected, so the limit goes
back to 450 mA every time. This polls more often and raises it again.

- 0xA1 low 4 bits (input limit): 0=450mA, 1=80mA, 2=850mA, 3=1.0A, then
  0.25 A steps, 7=2.0A, 11 and above=3.0A. The high 4 bits are CHRG_CT_EN and
  the charge stop voltage (3.26 V): never touched. A limit of 2.0 A or more is
  not lowered.
- Nothing is done while USB/online is 0: plugged in directly and detected as
  DCP, the driver sets AC online and the limit stays at 3.0 A.
- On stop (SIGTERM, Ctrl-C), the low bits go back to 0 only if they are still
  the 2.0 A this process set, the same as the driver's default.
- Use only with a PD hub or a large charger. Do not force 2 A on a weak supply
  such as a PC's USB port (RK818 lowers its own input when the voltage drops,
  but do not rely on it).
- LEDs, every 0.5 seconds: not plugged in = off, charging (battery current
  positive) = red, full (status Full or capacity 100) = green, plugged in but
  discharging = blinking red.

Run as root: sudo python3 pdcharge.py [-v]
"""
import ctypes
import fcntl
import os
import signal
import sys
import time

BUS = "/dev/i2c-0"
ADDR = 0x1C          # RK818
# The ekesete.net D250 Debian 13.7 image this was checked on (its driver writes 450 mA back)
KNOWN_KERNEL = "6.18.41-g653d8276acb7"
REG = 0xA1           # USB_CTRL_REG
ILIM_MASK = 0x0F
ILIM_WANT = 7        # 2.0A
ILIM_DEFAULT = 0     # 450mA
INTERVAL = 0.05      # seconds
SYSFS = os.environ.get("FDTP_PDCHARGE_SYSFS", "/sys")
ONLINE = SYSFS + "/class/power_supply/USB/online"

I2C_SLAVE_FORCE = 0x0706
I2C_SMBUS = 0x0720
I2C_SMBUS_READ = 1
I2C_SMBUS_WRITE = 0
I2C_SMBUS_BYTE_DATA = 2


class SmbusData(ctypes.Union):
    _fields_ = [("byte", ctypes.c_uint8), ("block", ctypes.c_uint8 * 34)]


class SmbusIoctl(ctypes.Structure):
    _fields_ = [("read_write", ctypes.c_uint8), ("command", ctypes.c_uint8),
                ("size", ctypes.c_uint32), ("data", ctypes.POINTER(SmbusData))]


class Rk818:
    def __init__(self):
        self.fd = os.open(BUS, os.O_RDWR)
        # The charger driver owns this address (same FORCE as i2cget -f).
        try:
            fcntl.ioctl(self.fd, I2C_SLAVE_FORCE, ADDR)
        except OSError:
            self.close()
            raise

    def _xfer(self, rw, value=0, reg=REG):
        data = SmbusData()
        data.byte = value
        arg = SmbusIoctl(rw, reg, I2C_SMBUS_BYTE_DATA, ctypes.pointer(data))
        fcntl.ioctl(self.fd, I2C_SMBUS, arg)
        return data.byte

    def close(self):
        os.close(self.fd)

    def read(self):
        return self._xfer(I2C_SMBUS_READ)

    def write(self, value):
        self._xfer(I2C_SMBUS_WRITE, value)


def ilim_ma(nibble):
    return {0: 450, 1: 80, 2: 850}.get(nibble, 1000 + (nibble - 3) * 250)


def usb_online():
    try:
        with open(ONLINE) as f:
            return f.read().strip() == "1"
    except OSError:
        return False


def read_ps(name, prop):
    try:
        with open(f"{SYSFS}/class/power_supply/{name}/{prop}") as f:
            return f.read().strip()
    except OSError:
        return ""


class Leds:
    """GPIO LEDs from jcs/linux-dm250 pomera-dm250-led.dtsi; gpiod v2 optional."""
    CHIP = "/dev/gpiochip1"   # GPIO1 (0x20080000)
    RED = 8                   # GPIO1_B0
    GREEN = 12                # GPIO1_B4

    def __init__(self):
        self.req = None
        self.blink = False
        try:
            import gpiod
            from gpiod.line import Direction, Value
        except ImportError:
            log("python3-libgpiod が無いので LED は使いません", "python3-libgpiod missing: LEDs disabled")
            return
        self.on, self.off = Value.ACTIVE, Value.INACTIVE
        try:
            self.req = gpiod.request_lines(
                self.CHIP, consumer="pdcharge",
                config={(self.RED, self.GREEN): gpiod.LineSettings(
                    direction=Direction.OUTPUT, output_value=Value.INACTIVE)})
        except (OSError, AttributeError) as exc:
            log(f"LED は使えません: {exc}", f"LEDs disabled: {exc}")

    def _set(self, red, green):
        self.req.set_values({self.RED: self.on if red else self.off,
                             self.GREEN: self.on if green else self.off})

    def update(self):
        if not self.req:
            return
        plugged = read_ps("USB", "online") == "1" or read_ps("AC", "online") == "1"
        if not plugged:
            self._set(False, False)
        elif read_ps("BATTERY", "status") == "Full" or read_ps("BATTERY", "capacity") == "100":
            self._set(False, True)
        elif int(read_ps("BATTERY", "current_now") or 0) > 0:
            self._set(True, False)
        else:
            self.blink = not self.blink
            self._set(self.blink, False)

    def close(self):
        if self.req:
            self._set(False, False)
            self.req.release()


def log(ja, en):
    lang = os.environ.get("FDTP_MSG_LANG", "auto")
    if lang == "auto":
        lang = os.environ.get("LC_ALL") or os.environ.get("LC_MESSAGES") or os.environ.get("LANG", "")
    print("pdcharge: " + (ja if lang.startswith("ja") else en), flush=True)


class UnsafeHardware(Exception):
    def __init__(self, ja, en=None):
        self.ja, self.en = ja, en or ja
        super().__init__(self.en)


def open_checked():
    """Read-only gates before any register write or GPIO request.

    Linux rk8xx uses CHIP_NAME 0x17/0x18 and masks revision nibble.
    RK818 ID 0x8181: https://lkml.indiana.edu/1607.0/02713.html
    Revision mask: https://lists.openwall.net/linux-kernel/2019/09/17/221
    The RK818 Rev 1.1 datasheet lists these two addresses as reserved.
    """
    dt = os.environ.get("FDTP_CONFIGURE_DT", "/proc/device-tree")
    model = b""
    for name in ("compatible", "model"):
        try:
            with open(os.path.join(dt, name), "rb") as f:
                model += f.read()
        except OSError:
            pass
    if b"dm250" not in model.lower():
        raise UnsafeHardware("Pomera DM250 ではありません", "not a Pomera DM250")
    if not os.path.isdir(SYSFS + "/bus/platform/drivers/rk818-charger"):
        raise UnsafeHardware("rk818-charger ドライバがありません", "rk818-charger driver missing")
    pmic = None
    try:
        pmic = Rk818()
        msb = pmic._xfer(I2C_SMBUS_READ, reg=0x17)
        lsb = pmic._xfer(I2C_SMBUS_READ, reg=0x18)
        if msb != 0x81 or lsb & 0xF0 != 0x80:
            raise UnsafeHardware(f"RK818 識別が不一致: {msb:02x} {lsb:02x}",
                                 f"RK818 identity mismatch: {msb:02x} {lsb:02x}")
        pmic.read()
        return pmic
    except (OSError, UnsafeHardware) as exc:
        if pmic is not None:
            pmic.close()
        if isinstance(exc, UnsafeHardware):
            raise
        raise UnsafeHardware(f"I2C 点検失敗: {exc}", f"I2C check failed: {exc}") from exc


def main():
    verbose = "-v" in sys.argv[1:]
    try:
        pmic = open_checked()
    except UnsafeHardware as exc:
        log(exc.ja, exc.en)
        return 1 if "--probe" in sys.argv[1:] else 0
    if "--probe" in sys.argv[1:]:
        pmic.close()
        return 0
    if os.uname().release != KNOWN_KERNEL:
        log(f"カーネル {os.uname().release} は未確認です（確かめたのは {KNOWN_KERNEL}）",
            f"kernel {os.uname().release} is untested (checked: {KNOWN_KERNEL})")
    leds = Leds()
    led_tick = 0
    fixes = 0
    running = True
    raised = False

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    log(f"開始、0x{REG:02x} = 0x{pmic.read():02x}", f"start, 0x{REG:02x} = 0x{pmic.read():02x}")
    last_report = time.monotonic()
    while running:
        if usb_online():
            v = pmic.read()
            # Raise only below 2 A; leave direct DCP at 3 A.
            if ilim_ma(v & ILIM_MASK) < ilim_ma(ILIM_WANT):
                pmic.write((v & ~ILIM_MASK & 0xFF) | ILIM_WANT)
                raised = True
                fixes += 1
        led_tick += 1
        if led_tick >= 10:   # every 0.5 seconds
            led_tick = 0
            leds.update()
        now = time.monotonic()
        if verbose and now - last_report >= 10:
            log(f"直近 10 秒に {fixes} 回補正", f"{fixes} fixes in last 10s")
            fixes = 0
            last_report = now
        time.sleep(INTERVAL)
    leds.close()
    v = pmic.read()
    # Restore only a value this process raised; leave direct 3 A alone.
    if raised and v & ILIM_MASK == ILIM_WANT:
        pmic.write((v & ~ILIM_MASK & 0xFF) | ILIM_DEFAULT)
    log(f"停止、0x{REG:02x} = 0x{pmic.read():02x}", f"stop, 0x{REG:02x} = 0x{pmic.read():02x}")
    pmic.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        log(f"失敗: {exc}", f"failed: {exc}")
        sys.exit(1)
