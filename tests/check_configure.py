#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Tests of ./configure, mk/install.mk and pomera/ (with pomera_setup.sh, the .deb's
fdtp-pomera-setup; docs/FDTP_CONFIGURE.md).

    python3 -B tests/check_configure.py

Runs on macOS and Linux without root and without touching the machine:
configure's view of the machine (uname, /proc/device-tree, id -u, the
internal memory probe, pkg-config, the compiler) is replaced through its
FDTP_CONFIGURE_* variables, config.mk goes to a temporary directory, and
make install runs with DESTDIR (which also skips the Pomera part).
pomera/make_pio.py is tried on a small device tree made here; the real
sha256 values can only be checked on a DM250's SD.
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest import mock

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIGURE = os.path.join(APP, "configure")
# pomera/ here, scripts/pomera/ in the public copy (tools/make_public.sh).
POMERA = next((d for d in (os.path.join(APP, "scripts", "pomera"), os.path.join(APP, "pomera")) if os.path.isdir(d)),
              os.path.join(APP, "pomera"))
SHELLS = [s for s in ("sh", "dash") if shutil.which(s)]
FAILS = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILS.append(what)
        print("  NG:", what)
    return cond


def run(args, env=None, stdin="", cwd=APP):
    p = subprocess.run(args, input=stdin, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=cwd)
    return p.returncode, p.stdout + p.stderr


def base_env(**extra):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("LC_", "FDTP_", "POMERA_")) and k not in ("LANG", "LANGUAGE", "TERM", "PKG_CONFIG", "CC",
                                                                          "MAKEFLAGS", "MFLAGS", "MAKELEVEL", "PREFIX", "DESTDIR")}
    env.update(FDTP_CONFIGURE_OWNKERNEL="no", FDTP_CONFIGURE_ADJTIME="/nonexistent/fdtp-adjtime")
    env.update(extra)
    return env


def read_mk(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^([A-Z_]+) = (.*)$", line.rstrip("\n"))
            if m:
                out[m.group(1)] = m.group(2)
    return out


class Machine:
    """A fake machine in a temporary directory: device tree, pkg-config, cc."""

    def __init__(self, root):
        self.root = root
        self.dt = os.path.join(root, "dt")
        os.makedirs(self.dt, exist_ok=True)
        self.bin = os.path.join(root, "bin")
        os.makedirs(self.bin, exist_ok=True)
        self.pkg_config = os.path.join(self.bin, "fake-pkg-config")
        self.cc = os.path.join(self.bin, "fake-cc")
        with open(self.pkg_config, "w") as f:
            f.write('#!/bin/sh\n[ "$1" = --exists ] || exit 1\n'
                    'for m in $FAKE_PC; do [ "$m" = "$2" ] && exit 0; done\nexit 1\n')
        with open(self.cc, "w") as f:
            f.write('#!/bin/sh\nfor a; do case $a in -l*) case " $FAKE_LIBS " in *" ${a#-l} "*) ;; *) exit 1 ;; esac ;; esac; done\nexit 0\n')
        os.chmod(self.pkg_config, 0o755)
        os.chmod(self.cc, 0o755)
        self.out = os.path.join(root, "config.mk")

    def device(self, compatible=None, model=None):
        for name, value in (("compatible", compatible), ("model", model)):
            path = os.path.join(self.dt, name)
            if value is None:
                if os.path.exists(path):
                    os.remove(path)
            else:
                with open(path, "wb") as f:
                    f.write(value)

    def configure(self, *args, uname="Linux", uid="1000", probe=None, tty=None, stdin="", lang=None, term=None,
                  pc="libdrm gbm egl glesv2 xcb libjpeg libpng libwebp zlib expat",
                  libs="gif mupdf uim uim-scm", shell="sh", heif="yes", apt=None, brew=None, pdcharge_probe=None, cve_probe="exit 1", ownkernel="no", adjtime=None, wifi_default=None, kernel_root=None):
        if os.path.exists(self.out):
            os.remove(self.out)
        env = base_env(FDTP_CONFIG_MK=self.out, FDTP_CONFIGURE_UNAME=uname, FDTP_CONFIGURE_DT=self.dt,
                       FDTP_CONFIGURE_UID=uid, FDTP_CONFIGURE_OWNKERNEL=ownkernel,
                       FDTP_KERNEL_APT_ROOT=kernel_root or os.path.join(self.root, "no-kernel-source"),
                       FDTP_CONFIGURE_ADJTIME=adjtime or os.path.join(self.root, "no-adjtime"),
                       POMERA_WIFI_PS_DEFAULT=wifi_default or "y", PKG_CONFIG=self.pkg_config, CC=self.cc, FAKE_PC=pc, FAKE_LIBS=libs,
                       FDTP_CONFIGURE_PROBE=probe if probe is not None else "echo should-not-run; exit 1",
                       FDTP_CONFIGURE_CVE_PROBE=cve_probe, FDTP_CONFIGURE_HEIF=heif, FDTP_CONFIGURE_BREW=brew or os.path.join(self.root, "no-brew"),
                       FDTP_CONFIGURE_APT=apt or "echo apt-should-not-run; false")
        if pdcharge_probe is not None:
            env["FDTP_CONFIGURE_PDCHARGE_PROBE"] = pdcharge_probe
        if uname == "Darwin":
            env["CC"] = "cc"
        if tty:
            env["FDTP_CONFIGURE_TTY"] = tty
        if lang:
            env["LANG"] = lang
        if term:
            env["TERM"] = term
        code, text = run([shell, CONFIGURE] + list(args), env=env, stdin=stdin)
        mk = read_mk(self.out) if os.path.exists(self.out) else None
        return code, text, mk


POMERA_DT = b"kingjim,pomera-dm250\0rockchip,rk3128\0"


def test_syntax():
    print("== shell syntax")
    files = [CONFIGURE] + sorted(os.path.join(POMERA, f) for f in os.listdir(POMERA) if f.endswith(".sh"))
    for sh in SHELLS:
        for f in files:
            code, text = run([sh, "-n", f])
            check(code == 0, "%s -n %s: %s" % (sh, os.path.relpath(f, APP), text.strip()))
    check(os.access(CONFIGURE, os.X_OK), "configure is not executable")


def test_deps_cover_makefiles():
    """Every library the Makefiles use is in configure's list."""
    print("== configure's library list against Makefile, mk/*.mk and sources.mk")
    code, text = run(["sh", CONFIGURE, "--list-deps"])
    check(code == 0, "--list-deps failed")
    listed = set()
    for line in text.splitlines():
        kind, name = line.split()[:2]
        listed.add(name.split(":")[0] if kind == "lib" else name)
    files = [os.path.join(APP, "Makefile")] + [os.path.join(APP, "mk", f) for f in sorted(os.listdir(os.path.join(APP, "mk")))]
    # The platform layer: lib/rmp-platform in the public copy, port/app/platform beside fdtp/ here.
    sources = [s for s in (os.path.join(APP, "lib", "rmp-platform", "sources.mk"),
                           os.path.join(APP, "..", "port", "app", "platform", "sources.mk")) if os.path.exists(s)]
    check(sources, "the platform layer's sources.mk not found")
    files += sources[:1]
    used = set()
    for path in files:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for m in re.finditer(r"--(?:cflags|libs|exists)((?:\s+[a-z][\w.+-]*)+)", text):
            used.update(m.group(1).split())
        for m in re.finditer(r"PLATFORM_PKGS\s*:?\+?=([^\n]*)", text):
            used.update(m.group(1).split())
        for m in re.finditer(r"(?<![\w-])-l([a-z](?:[\w-]*\w)?)(?=[\s)]|$)", text):
            used.add("-l" + m.group(1))
    # -lNAME that a pkg-config module stands for (the Mac branches name the
    # libraries directly); libm and libdl are libc's.
    alias = {"-ljpeg": "libjpeg", "-lpng": "libpng", "-lwebp": "libwebp", "-lz": "zlib", "-lexpat": "expat",
             "-lssl": "openssl", "-lcrypto": "openssl"}
    for u in sorted(used):
        if u in ("-lm", "-ldl") or u.startswith("$"):
            continue
        name = alias.get(u, u[2:] if u.startswith("-l") else u)
        check(name in listed, "%s (from the Makefiles) is not in configure's list" % u)


def test_darwin(m):
    print("== macOS")
    code, text, mk = m.configure("--yes", "--no-pomera", "--prefix=/opt/fdtp/", uname="Darwin")
    check(code == 0, "Darwin --yes failed: " + text)
    check(mk == {"FDTP_CONFIGURED": "yes", "PREFIX": "/opt/fdtp", "FDTP_LANGUAGE": "auto", "POMERA_STABILITY": "no",
                 "POMERA_INTERNAL": "keep", "POMERA_PDCHARGE": "keep", "POMERA_CVE": "keep", "POMERA_WIFI_PS": "keep",
                 "POMERA_CLEAN_OLD": "keep", "POMERA_RTC": "keep", "POMERA_KERNEL_APT": "keep", "HAVE_XCB": "no", "HAVE_UIM": "no"}, "Darwin config.mk: %s" % mk)
    check("Wi-Fi" not in text and "pkg-config" not in text, "Darwin asked about the Pomera or pkg-config")
    check("make install is for Linux" in text, "Darwin: no note that make install is for Linux")
    # The Pomera questions never come on the Mac, even with a DM250 tree.
    m.device(POMERA_DT)
    code, text, mk = m.configure("--yes", uname="Darwin")
    check(code == 0 and mk and mk["POMERA_STABILITY"] == "no" and "Wi-Fi" not in text, "Darwin with a DM250 tree")
    m.device()
    env = base_env(FDTP_CONFIG_MK=m.out, FDTP_CONFIGURE_UNAME="Darwin", CC="no-such-cc-fdtp")
    os.path.exists(m.out) and os.remove(m.out)
    code, text = run(["sh", CONFIGURE, "--yes"], env=env)
    check(code == 1 and "no-such-cc-fdtp" in text and not os.path.exists(m.out), "Darwin without a compiler: %s" % text)


def test_linux_deps(m):
    print("== Linux: dependencies")
    m.device(b"raspberrypi,4-model-b\0brcm,bcm2711\0", b"Raspberry Pi 4 Model B Rev 1.4\0")
    code, text, mk = m.configure("--yes", "--prefix=/usr")
    check(code == 0, "all found: " + text)
    check(mk and mk["HAVE_XCB"] == "yes" and mk["HAVE_UIM"] == "yes" and "NOVEL_IME" not in mk and mk["PREFIX"] == "/usr",
          "all found: config.mk %s" % mk)
    check("Wi-Fi" not in text, "not a DM250 but the Pomera questions came")
    code, text, mk = m.configure("--yes", pc="gbm egl glesv2 xcb libjpeg libpng libwebp zlib expat", libs="gif uim uim-scm")
    check(code == 1 and mk is None, "missing libdrm and libmupdf did not stop: %s" % text)
    check("sudo apt install libdrm-dev libmupdf-dev" in text, "missing: apt line wrong: " + text)
    code, text, mk = m.configure("--yes", libs="gif mupdf", pc="libdrm gbm egl glesv2 libjpeg libpng libwebp zlib expat")
    check(code == 0 and mk and mk["HAVE_UIM"] == "no" and mk.get("NOVEL_IME") == "ime_none" and mk["HAVE_XCB"] == "no",
          "optional uim and xcb missing: %s %s" % (mk, text))
    check("libuim-dev" in text and "libxcb1-dev" in text, "optional ones missing: no warning")
    code, text, mk = m.configure("--yes", libs="gif mupdf uim")
    check(mk and mk["HAVE_UIM"] == "no" and mk.get("NOVEL_IME") == "ime_none", "uim without uim-scm counted as found")
    code, text, mk = m.configure("--yes", pc="libdrm gbm egl glesv2 libjpeg libpng libwebp zlib expat")
    check(code == 0, "without xcb: " + text)
    env = base_env(FDTP_CONFIG_MK=m.out, FDTP_CONFIGURE_UNAME="Linux", FDTP_CONFIGURE_DT=m.dt, CC=m.cc,
                   PKG_CONFIG="no-such-pkg-config-fdtp")
    os.path.exists(m.out) and os.remove(m.out)
    code, text = run(["sh", CONFIGURE, "--yes"], env=env)
    check(code == 1 and "sudo apt install pkg-config" in text and not os.path.exists(m.out), "without pkg-config: " + text)


def test_options(m):
    print("== options")
    for args in (["--lang=fr"], ["--prefix=usr/local"], ["--prefix=/opt/my fdtp"], ["--prefix=/opt/$x"], ["--bogus"], ["--lang"]):
        code, text, mk = m.configure(*args)
        check(code == 2 and mk is None, "%s accepted: %s" % (args, text))
    code, text, mk = m.configure("--yes", "--lang", "en", "--prefix", "/opt/f")
    check(mk and mk["FDTP_LANGUAGE"] == "en" and mk["PREFIX"] == "/opt/f" and "Choose a number" not in text,
          "--lang en --prefix /opt/f: %s" % mk)
    code, text = run(["sh", CONFIGURE, "--help"])
    check(code == 0 and "--prefix" in text, "--help")


def test_pomera(m):
    print("== Pomera DM250")
    m.device(POMERA_DT, b"King Jim Pomera DM250\0")
    code, text, mk = m.configure("--yes")
    check(code == 0 and mk and mk["POMERA_STABILITY"] == "yes" and mk["POMERA_INTERNAL"] == "keep" and mk["POMERA_PDCHARGE"] == "keep", "DM250 --yes: %s" % mk)
    check("Finding the internal memory needs root" in text and "should-not-run" not in text, "not root: probe not skipped")
    i_notes, i_ask = text.find("Notes:"), text.find("Set up Wi-Fi freeze workarounds?\nIf yes: disable power saving and enable automatic restart on a freeze; SD rewriting is confirmed separately\nIf no: keep existing settings\nAnswer: [Y/n]")
    check(0 <= i_notes < i_ask, "Wi-Fi: the notes are not before [Y/n]")
    check("pomera/pomera_dma_pio.sh restore" in text and "24 MiB" in text, "Wi-Fi: undo or what is rewritten not told")
    # Only the model, upper case.
    m.device(None, b"KING JIM POMERA DM250\0")
    code, text, mk = m.configure("--yes")
    check(mk and mk["POMERA_STABILITY"] == "yes", "DM250 from the model only")
    m.device(POMERA_DT)
    code, text, mk = m.configure("--yes", "--no-pomera")
    check(mk and mk["POMERA_STABILITY"] == "no" and "Wi-Fi" not in text, "--no-pomera asked")
    # Root: the probe finds the FAT.
    code, text, mk = m.configure("--yes", uid="0", probe="echo vfat")
    check(mk and mk["POMERA_INTERNAL"] == "ro", "root, vfat found: %s" % mk)
    i_notes, i_ask = text.find("To stop either setting:"), text.find("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]")
    check(0 <= i_notes < i_ask and "unpersist" in text and "/etc/fstab" in text, "internal: undo instructions not before choice")
    code, text, mk = m.configure("--yes", uid="0", probe="echo ext4")
    check(mk and mk["POMERA_INTERNAL"] == "keep" and "Mount the internal memory" not in text and "skipped" in text,
          "root, no FAT: %s" % text)
    code, text, mk = m.configure("--yes", uid="0", probe="exit 1")
    check(mk and mk["POMERA_INTERNAL"] == "keep" and "skipped" in text, "root, probe failed")
    # Interactive, in Japanese: language 1 (after a wrong answer), Wi-Fi no, internal default.
    code, text, mk = m.configure(uid="0", probe="echo exfat", tty="yes", stdin="9\n1\nn\n\n", lang="ja_JP.UTF-8")
    check(code == 0 and mk and mk["FDTP_LANGUAGE"] == "ja" and mk["POMERA_STABILITY"] == "no" and mk["POMERA_INTERNAL"] == "ro",
          "interactive ja: %s %s" % (mk, text))
    check("== Wi-Fi の安定化（Pomera DM250 だけ） ==" in text and "1・2・3 のどれかで" in text, "ja messages")
    i_notes, i_ask = text.find("注意:"), text.find("Wi-Fi が固まる不具合への対策を設定しますか？\nはいの場合：省電力オフ・停止時の自動再起動を設定、SD の書き換えは別途確認\nいいえの場合：既存設定を変更しない\n入力: [Y/n]")
    check(0 <= i_notes < i_ask, "ja: notes not before [Y/n]")
    check(0 <= text.find("設定をやめるには:") < text.find("ポメラの内蔵文書を Linux でどう使いますか（1 読むだけ〈Enter 時の既定〉／2 編集・保存も行う／3 起動時に自動で使う設定を解除する）[1/2/3]"), "ja internal: undo instructions after choice")
    # Choose rw, decline its warning, and choose no; confirm rw; reject invalid input.
    for answers, expected in (("2\nn\n3\n", "no"), ("2\ny\n", "rw"), ("x\n1\n", "ro")):
        code, text, mk = m.configure("--lang=en", uid="0", probe="echo vfat", tty="yes", stdin="n\n" + answers)
        check(code == 0 and mk and mk.get("POMERA_INTERNAL") == expected,
              "interactive internal %r: %s %s" % (answers, mk, text))
        if answers.startswith("2"):
            check("fsck.fat -n" in text and "Allow Linux to edit and save internal documents?\nIf yes: allow read-write access\nIf no: return to the access choices\nAnswer: [y/N]" in text,
                  "read-write warning missing: " + text)
            if expected == "no":
                check(text.count("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]") >= 2, "declined read-write did not return to choice: " + text)
        if answers.startswith("x"):
            check("Please answer 1, 2 or 3." in text and text.count("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]") >= 2, "invalid internal choice: " + text)
    # y/n: a wrong answer is asked again; EOF takes the default.
    code, text, mk = m.configure(tty="yes", stdin="2\nmaybe\nn\n")
    check(mk and mk["FDTP_LANGUAGE"] == "en" and mk["POMERA_STABILITY"] == "no" and "Please answer y or n." in text,
          "interactive en: %s" % mk)
    code, text, mk = m.configure(tty="yes", stdin="")
    check(mk and mk["FDTP_LANGUAGE"] == "auto" and mk["POMERA_STABILITY"] == "yes", "interactive EOF: %s" % mk)
    # Not a terminal: the defaults, said so.
    code, text, mk = m.configure(tty="no", lang="ja_JP.UTF-8")
    check(mk and mk["POMERA_STABILITY"] == "yes" and "端末ではない" in text, "not a terminal")
    # The Linux console cannot show Japanese.
    code, text, mk = m.configure("--yes", lang="ja_JP.UTF-8", term="linux")
    check("Set up Wi-Fi freeze workarounds?\nIf yes: disable power saving and enable automatic restart on a freeze; SD rewriting is confirmed separately\nIf no: keep existing settings\nAnswer:" in text and "cannot show Japanese" in text, "TERM=linux not in English")
    for sh in SHELLS:
        code, text, mk = m.configure("--yes", uid="0", probe="echo vfat", lang="ja_JP.UTF-8", shell=sh)
        check(code == 0 and mk and mk["POMERA_INTERNAL"] == "ro", "%s: %s" % (sh, text))
    m.device()


def test_heif(m):
    """libheif (HEIC) is read by FDTP with dlopen: configure only tells or,
    as root, offers to install it with apt (FDTP_CONFIGURE_APT replaces it)."""
    print("== HEIC (libheif)")
    m.device(b"raspberrypi,4-model-b\0")
    log = os.path.join(m.root, "apt.log")
    fake_apt = os.path.join(m.bin, "fake-apt")
    with open(fake_apt, "w") as f:
        f.write('#!/bin/sh\necho "$*" >> "%s"\n[ -z "$FAKE_APT_FAIL" ]\n' % log)
    os.chmod(fake_apt, 0o755)
    code, text, mk = m.configure("--yes")
    check(code == 0 and "libheif is installed" in text and "HEIC (libheif): yes" in text, "heif found: " + text)
    # Not root: the apt line only, no question.
    code, text, mk = m.configure("--yes", heif="no")
    check(code == 0 and "To install it: sudo apt install libheif1 libheif-plugin-libde265" in text
          and "Install it?" not in text and "apt-should-not-run" not in text and "HEIC (libheif): no" in text,
          "heif missing, not root: " + text)
    # Root: what it does first, then [Y/n]; yes installs, no tells the line.
    if os.path.exists(log):
        os.remove(log)
    code, text, mk = m.configure("--yes", heif="no", uid="0", apt=fake_apt)
    i_notes, i_ask = text.find("To remove it: sudo apt remove"), text.find("Install libheif to open HEIC photos?\nIf yes: install through apt\nIf no: skip installation; HEIC cannot be opened\nAnswer: [Y/n]")
    check(code == 0 and 0 <= i_notes < i_ask and "HEIC (libheif): yes" in text, "heif root --yes: " + text)
    check(os.path.exists(log) and open(log).read() == "install -y libheif1 libheif-plugin-libde265\n", "heif: apt not run as expected")
    os.remove(log)
    code, text, mk = m.configure("--lang=en", heif="no", uid="0", apt=fake_apt, tty="yes", stdin="n\n", lang="ja_JP.UTF-8")
    check(code == 0 and "HEIC の写真を開く部品 libheif を入れますか？\nはいの場合：apt で導入\nいいえの場合：導入せず HEIC は開けない\n入力: [Y/n]" in text and "あとで入れるには: sudo apt install" in text
          and not os.path.exists(log), "heif root, answered no (ja): " + text)
    env_fail = dict(FAKE_APT_FAIL="1")
    os.environ.update(env_fail)
    try:
        code, text, mk = m.configure("--yes", heif="no", uid="0", apt=fake_apt)
    finally:
        os.environ.pop("FAKE_APT_FAIL")
    check(code == 0 and "It was not installed" in text and "HEIC (libheif): no" in text, "heif: apt failed: " + text)
    # The Mac: Homebrew's libheif or the brew line.
    code, text, mk = m.configure("--yes", uname="Darwin")
    check(code == 0 and "brew install libheif" in text, "Darwin without libheif: " + text)
    os.makedirs(os.path.join(m.root, "brew", "opt", "libheif"), exist_ok=True)
    code, text, mk = m.configure("--yes", uname="Darwin", brew=os.path.join(m.root, "brew"))
    check(code == 0 and "Homebrew's libheif is there" in text, "Darwin with libheif: " + text)
    m.device()


def make(args, env):
    return run(["make", "-s", "-C", APP] + args, env=env)


def test_install(m):
    print("== make install / uninstall (DESTDIR)")
    m.device()
    code, text, mk = m.configure("--yes", "--lang=ja", "--prefix=/opt/fdtp", "--no-pomera")
    check(code == 0, "configure for install: " + text)
    root = os.path.join(m.root, "dest")
    fake = os.path.join(m.root, "fake-fdtp")
    with open(fake, "w") as f:
        f.write("#!/bin/sh\necho fake\n")
    env = base_env()
    common = ["FDTP_CONFIG_MK=" + m.out, "INSTALL_EXE=" + fake, "DESTDIR=" + root]
    code, text = run(["make", "-n", "-C", APP, "install"] + common, env=env)
    for want in ("/opt/fdtp/bin/fdtp", "/opt/fdtp/share/applications/fdtp.desktop", "Exec=/opt/fdtp/bin/fdtp",
                 "/opt/fdtp/share/icons/hicolor/${s}x${s}/apps/fdtp.png", "/opt/fdtp/share/doc/fdtp/", "LANGUAGE=ja",
                 "/etc/fdtprc", "LICENSE", "README.md"):
        check(want in text, "make -n install lacks %s" % want)
    code, text = make(["install"] + common, env)
    check(code == 0, "make install: " + text)
    p = lambda *a: os.path.join(root, *a)
    check(os.access(p("opt/fdtp/bin/fdtp"), os.X_OK), "bin/fdtp")
    with open(p("opt/fdtp/share/applications/fdtp.desktop")) as f:
        desktop = f.read()
    check("Exec=/opt/fdtp/bin/fdtp\n" in desktop and "@HOME@" not in desktop, "desktop Exec")
    for s in (16, 32, 48, 64, 128):
        check(os.path.isfile(p("opt/fdtp/share/icons/hicolor/%dx%d/apps/fdtp.png" % (s, s))), "icon %d" % s)
    check(os.path.isfile(p("opt/fdtp/share/doc/fdtp/LICENSE")) and os.path.isfile(p("opt/fdtp/share/doc/fdtp/README.md")), "docs")
    rc = p("etc/fdtprc")
    with open(rc) as f:
        first = f.read()
    check(first.splitlines()[1:] == ["LANGUAGE=ja"] and first.startswith("# "), "new fdtprc: %r" % first)
    # An existing fdtprc keeps its other lines; only LANGUAGE changes.
    with open(rc, "w") as f:
        f.write("# mine\nSORTTYPE=2\n  LANGUAGE = en\nAWAKE=5")
    code, text = make(["install"] + common + ["FDTP_LANGUAGE=auto"], env)
    with open(rc) as f:
        check(f.read() == "# mine\nSORTTYPE=2\nLANGUAGE=auto\nAWAKE=5", "fdtprc with LANGUAGE: only that line")
    with open(rc, "w") as f:
        f.write("SORTTYPE=2")
    make(["install"] + common, env)
    with open(rc) as f:
        check(f.read() == "SORTTYPE=2\nLANGUAGE=ja\n", "fdtprc without LANGUAGE: appended")
    code, text = make(["install"] + common + ["FDTP_LANGUAGE=fr"], env)
    check(code != 0, "FDTP_LANGUAGE=fr installed")
    # Uninstall: the files go, an fdtprc with settings of its own stays.
    code, text = make(["uninstall"] + common, env)
    check(code == 0 and not os.path.exists(p("opt/fdtp/bin/fdtp")) and not os.path.exists(p("opt/fdtp/share/doc/fdtp"))
          and not os.path.exists(p("opt/fdtp/share/applications/fdtp.desktop"))
          and not os.path.exists(p("opt/fdtp/share/icons/hicolor/16x16/apps/fdtp.png")), "uninstall: " + text)
    check(os.path.exists(rc) and "kept" in text, "uninstall removed a user's fdtprc")
    os.remove(rc)
    make(["install"] + common, env)
    code, text = make(["uninstall"] + common, env)
    check(not os.path.exists(rc), "uninstall kept the fdtprc it made")
    # Without ./configure: no fdtprc.
    code, text = make(["install", "FDTP_CONFIG_MK=" + os.path.join(m.root, "none.mk"), "INSTALL_EXE=" + fake, "DESTDIR=" + root], env)
    check(code == 0 and not os.path.exists(rc), "install without config.mk wrote fdtprc")
    # Pomera answers: uninstall tells how to undo them.
    with open(m.out, "a") as f:
        f.write("POMERA_STABILITY = yes\nPOMERA_INTERNAL = ro\nPOMERA_PDCHARGE = yes\nPOMERA_CVE = yes\n")
    code, text = make(["uninstall"] + common, env)
    check("pomera_stability.sh remove" in text and "pomera_dma_pio.sh restore" in text and "unpersist" in text,
          "uninstall: how to undo the Pomera settings")
    check("pomera_pdcharge.sh remove" in text, "uninstall: how to undo PD charging")
    check("pomera_cve_2026_98432.sh remove" in text, "uninstall: how to undo CVE mitigation")
    code, text = run(["make", "-n", "-C", APP, "install"] + common, env=env)
    check("install_pomera.sh" in text, "make -n install: no Pomera part")
    check(re.search(r'POMERA_PDCHARGE=["\']?yes["\']?', text), "make install did not pass PD answer: " + text)
    check(re.search(r'POMERA_CVE=["\']?yes["\']?', text), "make install did not pass CVE answer: " + text)
    # A legacy config containing only the old setting is translated for the installer.
    legacy = os.path.join(m.root, "legacy.mk")
    with open(legacy, "w") as f:
        f.write("POMERA_INTERNAL_RO = yes\n")
    code, text = run(["make", "-n", "-C", APP, "install", "FDTP_CONFIG_MK=" + legacy,
                      "INSTALL_EXE=" + fake, "UNAME=Linux"], env=env)
    check(code == 0 and re.search(r'POMERA_INTERNAL=["\']?ro["\']?', text) and "install_pomera.sh" in text,
          "legacy config did not pass ro to installer: " + text)
    # New answers are forwarded even when they are the only requested action.
    for key, value in (("POMERA_WIFI_PS", "off"), ("POMERA_WIFI_PS", "on"),
                       ("POMERA_CLEAN_OLD", "yes"), ("POMERA_RTC", "yes"),
                       ("POMERA_KERNEL_APT", "yes"), ("POMERA_KERNEL_APT", "source")):
        with open(legacy, "w") as f:
            f.write(key + " = " + value + "\n")
        code, text = run(["make", "-n", "-C", APP, "install", "FDTP_CONFIG_MK=" + legacy,
                          "INSTALL_EXE=" + fake, "UNAME=Linux"], env=env)
        check(code == 0 and key + '="' + value + '"' in text,
              "make did not forward %s: %s" % (key, text))
    if sys.platform == "darwin":
        code, text = make(["install", "FDTP_CONFIG_MK=" + m.out, "DESTDIR=" + root], env)
        check(code != 0 and "for Linux" in text, "make install on macOS not refused")
    shutil.rmtree(root, ignore_errors=True)


def test_make_reads_config(m):
    print("== Makefile with config.mk")
    # A makefile that includes ours and shows the variables (make 3.81, as
    # on macOS, has no --eval).
    show = os.path.join(m.root, "show-vars.mk")
    with open(show, "w") as f:
        f.write("include %s\nfdtp-show:\n\t@echo NOVEL_IME=$(NOVEL_IME) MODULES=$(MODULES) PREFIX=$(PREFIX)\n"
                % os.path.join(APP, "Makefile"))
    mk = os.path.join(m.root, "show.mk")
    with open(mk, "w") as f:
        f.write("PREFIX = /opt/q\nNOVEL_IME = ime_none\nFDTP_CONFIGURED = yes\n")
    env = base_env()
    vars_ = lambda *a: run(["make", "-s", "-f", show, "fdtp-show", "UNAME=Linux"] + list(a), env=env)[1]
    text = vars_("FDTP_CONFIG_MK=" + mk)
    check("NOVEL_IME=ime_none" in text and " ime_none" in text and "ime_uim" not in text and "PREFIX=/opt/q" in text,
          "config.mk not read: " + text)
    text = vars_("FDTP_CONFIG_MK=" + mk, "CROSS=arm-linux-gnueabihf-")
    check("NOVEL_IME=ime_uim" in text and "PREFIX=/usr/local" in text, "cross build read config.mk: " + text)
    text = vars_("FDTP_CONFIG_MK=" + os.path.join(m.root, "none.mk"))
    check("NOVEL_IME=ime_uim" in text and " ime_uim" in text, "without config.mk the build changed: " + text)
    code, text = make(["-n", "FDTP_CONFIG_MK=" + mk], env)
    check(code == 0, "make -n (all) with config.mk: " + text[-300:])


# ---- make_pio.py on a small device tree ----------------------------------------------

def fdt(nodes, nop=()):
    """A flattened device tree: nodes is [(path, [(name, value)])] in
    order (one level under /); properties (path, name) in nop are written
    as FDT_NOP words of the same size."""
    strings, offsets, struct_ = b"", {}, b""

    def name_off(n):
        nonlocal strings
        if n not in offsets:
            offsets[n] = len(strings)
            strings += n.encode() + b"\0"
        return offsets[n]

    def pad(b):
        return b + b"\0" * (-len(b) % 4)

    def prop(path, n, v):
        word = struct.pack(">III", 3, len(v), name_off(n)) + pad(v)
        return struct.pack(">I", 4) * (len(word) // 4) if (path, n) in nop else word

    root_props = nodes[0][1]
    struct_ += struct.pack(">I", 1) + pad(b"\0")
    for n, v in root_props:
        struct_ += prop("/", n, v)
    for path, props in nodes[1:]:
        struct_ += struct.pack(">I", 1) + pad(path[1:].encode() + b"\0")
        for n, v in props:
            struct_ += prop(path, n, v)
        struct_ += struct.pack(">I", 2)
    struct_ += struct.pack(">II", 2, 9)
    rsv = b"\0" * 16
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_strings = off_struct + len(struct_)
    total = off_strings + len(strings)
    head = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, off_rsv, 17, 16, 0, len(strings), len(struct_))
    return head + rsv + struct_ + strings


def region(dtb):
    r = bytearray(1 << 20)
    r[0:4] = b"RSCE"
    r[0x400:0x400 + len(dtb)] = dtb
    r[0x400 + len(dtb) + 64:0x400 + len(dtb) + 72] = b"trailing"
    return bytes(r)


def tree():
    return [("/", [("compatible", b"kingjim,pomera-dm250\0rockchip,rk3128\0"), ("#address-cells", b"\0\0\0\1")]),
            ("/mmc@10214000", [("compatible", b"rockchip,rk3128-dw-mshc\0"), ("dmas", b"\0\0\0\x0d\0\0\0\1"),
                               ("dma-names", b"rx-tx\0")]),
            ("/mmc@10218000", [("compatible", b"rockchip,rk3128-dw-mshc\0"), ("dmas", b"\0\0\0\x0d\0\0\0\2"),
                               ("dma-names", b"rx-tx\0"), ("status", b"okay\0")]),
            ("/serial@20064000", [("dmas", b"\0\0\0\x0d\0\0\0\3\0\0\0\x0d\0\0\0\4"), ("dma-names", b"tx\0rx\0")])]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def test_make_pio(tmp):
    print("== make_pio.py on a small device tree")
    sys.path.insert(0, POMERA)
    try:
        import make_pio
    finally:
        sys.path.pop(0)
    src = region(fdt(tree()))
    want = region(fdt(tree(), nop={("/mmc@10218000", "dmas"), ("/mmc@10218000", "dma-names")}))
    check(len(src) == len(want) == 1 << 20, "test regions")
    got = make_pio.wifi_pio(src)
    check(got == want, "wifi_pio: not exactly the two properties as NOP")
    diff = [i for i in range(len(src)) if src[i] != got[i]]
    check(diff and 0x400 <= diff[0] and diff[-1] < 0x400 + len(fdt(tree())), "changes outside the device tree")
    left = {(n, p) for t, _, _, n, p in make_pio.walk(make_pio.dtb_of(got)) if t == make_pio.PROP}
    check(("/mmc@10218000", "dmas") not in left and ("/mmc@10218000", "dma-names") not in left
          and ("/mmc@10214000", "dmas") in left and ("/serial@20064000", "dma-names") in left
          and ("/mmc@10218000", "status") in left, "properties left: %s" % sorted(left))
    # The command: only with the expected input and result.
    i, o = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    with open(i, "wb") as f:
        f.write(src)
    script = os.path.join(POMERA, "make_pio.py")
    code, text = run([sys.executable, "-B", script, i, o, "--expect-in", sha(src), "--expect-out", sha(want)])
    check(code == 0 and open(o, "rb").read() == want, "make_pio.py with the test sha256s: " + text)
    os.remove(o)
    code, text = run([sys.executable, "-B", script, i, o])
    check(code == 3 and not os.path.exists(o) and "not the known original" in text, "ORIG differs: did not stop: " + text)
    code, text = run([sys.executable, "-B", script, i, o, "--expect-in", sha(src), "--expect-out", "0" * 64])
    check(code == 4 and not os.path.exists(o) and not os.path.exists(o + ".tmp"), "WIFI differs: did not stop: " + text)
    with open(i, "wb") as f:
        f.write(src[:-1])
    code, text = run([sys.executable, "-B", script, i, o, "--expect-in", sha(src[:-1])])
    check(code == 3 and not os.path.exists(o), "short input accepted")
    t = tree()
    t[2] = ("/mmc@10218000", [p for p in t[2][1] if p[0] != "dma-names"])
    bad = region(fdt(t))
    with open(i, "wb") as f:
        f.write(bad)
    code, text = run([sys.executable, "-B", script, i, o, "--expect-in", sha(bad)])
    check(code == 5 and not os.path.exists(o), "only dmas: did not stop: " + text)
    nofdt = bytes(1 << 20)
    with open(i, "wb") as f:
        f.write(nofdt)
    code, text = run([sys.executable, "-B", script, i, o, "--expect-in", sha(nofdt)])
    check(code == 5 and not os.path.exists(o), "no device tree: did not stop: " + text)
    # The same ORIG and WIFI everywhere.
    for name in ("pomera_dma_pio.sh", "rescue_rsce.sh"):
        with open(os.path.join(POMERA, name)) as f:
            text = f.read()
        check("ORIG=" + make_pio.ORIG in text and "WIFI=" + make_pio.WIFI in text, "%s: ORIG/WIFI differ from make_pio.py" % name)


def test_install_pomera(m):
    print("== pomera/install_pomera.sh (without root)")
    script = os.path.join(POMERA, "install_pomera.sh")
    m.device(b"raspberrypi,4-model-b\0")
    env = base_env(FDTP_CONFIGURE_DT=m.dt, POMERA_STABILITY="yes", POMERA_INTERNAL="ro", FDTP_MSG_LANG="en")
    code, text = run(["sh", script], env=env)
    check(code == 0 and "Not a Pomera DM250" in text, "not a DM250: " + text)
    code, text = run(["sh", script], env=base_env(FDTP_CONFIGURE_DT=m.dt))
    check(code == 0 and text == "", "both no: did something: " + text)
    if os.geteuid() != 0:   # never run the real thing
        m.device(POMERA_DT)
        code, text = run(["sh", script], env=env)
        check(code == 1 and "need root" in text, "DM250 without root: " + text)
    # Copy the installer beside a harmless mount script and fake root identity.
    fake_dir = os.path.join(m.root, "fake-pomera")
    os.makedirs(fake_dir, exist_ok=True)
    shutil.copy(script, os.path.join(fake_dir, "install_pomera.sh"))
    log = os.path.join(m.root, "persist-args")
    with open(os.path.join(fake_dir, "pomera_mount_internal.sh"), "w") as f:
        f.write('#!/bin/sh\nprintf "%s\\n" "$@" > "$FAKE_PERSIST_LOG"\n')
    with open(os.path.join(m.bin, "id"), "w") as f:
        f.write('#!/bin/sh\necho 0\n')
    os.chmod(os.path.join(m.bin, "id"), 0o755)
    m.device(POMERA_DT)
    env = base_env(FDTP_CONFIGURE_DT=m.dt, POMERA_INTERNAL="rw", FDTP_MSG_LANG="en",
                   FAKE_PERSIST_LOG=log, PATH=m.bin + os.pathsep + os.environ.get("PATH", ""))
    code, text = run(["sh", os.path.join(fake_dir, "install_pomera.sh")], env=env)
    args = open(log).read().splitlines() if os.path.exists(log) else []
    check(code == 0 and args == ["persist", "rw"], "installer rw persist: %s %s" % (args, text))
    # The answer changed since an earlier install (revision 1 §9): 3 removes a
    # mount set up before, and only then; a skipped question (keep) leaves it.
    with open(os.path.join(fake_dir, "pomera_mount_internal.sh"), "w") as f:
        f.write('#!/bin/sh\necho "$*" >> "$FAKE_PERSIST_LOG"\n'
                '[ "$1" != persisted ] || [ "$FAKE_PERSISTED" = yes ]\n')
    for internal, persisted, want in (("no", "yes", ["persisted", "unpersist"]), ("no", "no", ["persisted"]),
                                      ("keep", "yes", [])):
        if os.path.exists(log):
            os.remove(log)
        env.update(POMERA_INTERNAL=internal, FAKE_PERSISTED=persisted)
        code, text = run(["sh", os.path.join(fake_dir, "install_pomera.sh")], env=env)
        args = open(log).read().splitlines() if os.path.exists(log) else []
        check(code == 0 and args == want, "installer %s (persisted %s): %s %s" % (internal, persisted, args, text))
    # Charging has an explicit keep state: declining removes an earlier service.
    charge_log = os.path.join(m.root, "pdcharge-args")
    with open(os.path.join(fake_dir, "pomera_pdcharge.sh"), "w") as f:
        f.write('#!/bin/sh\necho "$*" >> "$FAKE_CHARGE_LOG"\n')
    env.update(POMERA_INTERNAL="keep", FAKE_CHARGE_LOG=charge_log)
    for answer, want in (("yes", ["install"]), ("no", ["remove"]), ("keep", [])):
        if os.path.exists(charge_log):
            os.remove(charge_log)
        env["POMERA_PDCHARGE"] = answer
        code, text = run(["sh", os.path.join(fake_dir, "install_pomera.sh")], env=env)
        got = open(charge_log).read().splitlines() if os.path.exists(charge_log) else []
        check(code == 0 and got == want, "installer PD %s: %s %s" % (answer, got, text))
    m.device()


def test_setup(m):
    """pomera/pomera_setup.sh (the .deb's fdtp-pomera-setup) without root:
    the machine is replaced as for configure; nothing is set up."""
    print("== pomera/pomera_setup.sh (fdtp-pomera-setup; without root)")
    script = os.path.join(POMERA, "pomera_setup.sh")

    def setup(*args, uid="1000", probe="echo should-not-run; exit 1", tty="no", stdin="", lang=None, heif="yes"):
        env = base_env(FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID=uid, FDTP_CONFIGURE_PROBE=probe, FDTP_CONFIGURE_TTY=tty,
                       FDTP_CONFIGURE_HEIF=heif, FDTP_CONFIGURE_CVE_PROBE="exit 1", FDTP_CONFIGURE_APT="echo apt-ran")
        if lang:
            env["LANG"] = lang
        return run(["sh", script] + list(args), env=env, stdin=stdin)

    code, text = setup("--help")
    check(code == 0 and "--undo" in text and "--status" in text, "setup --help: " + text)
    code, text = setup("--bogus")
    check(code == 2 and "unknown option" in text, "setup --bogus: " + text)
    m.device(b"raspberrypi,4-model-b\0")
    code, text = setup()
    check(code == 1 and "Not a Pomera DM250" in text, "setup, not a DM250: " + text)
    code, text = setup("--status")
    check(code == 1 and text == "", "setup --status, not a DM250: %r" % text)
    m.device(POMERA_DT)
    code, text = setup()
    check(code == 1 and "needs root" in text and "Wi-Fi" not in text, "setup, DM250 without root: " + text)
    # (Fake) root, both answered no: instructions precede the questions, nothing run.
    code, text = setup(uid="0", probe="echo vfat", tty="yes", stdin="n\n3\n")
    check(code == 0 and "Pomera-specific additional settings were not changed." in text, "setup n 3: " + text)
    i_notes, i_ask = text.find("Notes:"), text.find("Set up Wi-Fi freeze workarounds?\nIf yes: disable power saving and enable automatic restart on a freeze; SD rewriting is confirmed separately\nIf no: keep existing settings\nAnswer: [Y/n]")
    check(0 <= i_notes < i_ask and "pomera_dma_pio.sh restore" in text and "fdtp-pomera-setup --undo" in text
          and "asked once more" in text and "sudo make install" not in text, "setup: Wi-Fi notes: " + text)
    check(0 <= text.find("To stop either setting:") < text.find("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]") and "unpersist" in text,
          "setup: internal undo instructions not before choice")
    # libheif missing: asked after the internal memory; n keeps it out.
    code, text = setup(uid="0", probe="echo vfat", tty="yes", stdin="n\n3\nn\nn\n", heif="no")
    check(code == 0 and 0 <= text.find("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]") < text.find("Install libheif to open HEIC photos?\nIf yes: install through apt\nIf no: skip installation; HEIC cannot be opened\nAnswer: [Y/n]")
          and "apt-ran" not in text and "sudo apt install libheif1 libheif-plugin-libde265" in text, "setup heif n: " + text)
    code, text = setup(uid="0", probe="echo vfat", tty="no", heif="no")
    check("apt-ran install -y libheif1 libheif-plugin-libde265" in text, "setup heif default yes: " + text)
    code, text = setup(uid="0", probe="echo vfat", tty="yes", stdin="n\n3\n", lang="ja_JP.UTF-8")
    check(code == 0 and "Pomera 固有の追加設定は変更しませんでした。" in text and "書き換える直前にもう一度聞きます" in text, "setup ja: " + text)
    # The backup directory: beside the scripts here, /var/lib/fdtp under /usr.
    code, text = run(["sh", os.path.join(POMERA, "pomera_dma_pio.sh"), "backup-dir"], env=base_env())
    check(code == 0 and text.strip() in (POMERA, os.path.realpath(POMERA)), "backup-dir: " + text)
    code, text = run(["sh", os.path.join(POMERA, "pomera_dma_pio.sh"), "backup-dir"], env=base_env(FDTP_POMERA_BACKUP_DIR="/x/y"))
    check(code == 0 and text.strip() == "/x/y", "backup-dir with FDTP_POMERA_BACKUP_DIR: " + text)
    m.device()


def test_pdcharge_questions(m):
    """Exercise the shared question with a read-only fake hardware probe."""
    print("== PD charging question (fake probe)")
    fake = os.path.join(m.root, "charge-questions")
    os.makedirs(fake, exist_ok=True)
    with open(os.path.join(fake, "pomera_pdcharge.sh"), "w") as f:
        f.write('#!/bin/sh\n[ "$1" = probe ] || exit 9\nexit "$FAKE_CHARGE_PROBE"\n')
    m.device(POMERA_DT)
    for answer in ("y", "n"):
        code, text, mk = m.configure("--lang=en", uid="0", probe="echo vfat", pdcharge_probe="exit 0",
                                     tty="yes", stdin="n\n1\n" + answer + "\n")
        expected = "yes" if answer == "y" else "no"
        check(code == 0 and mk and mk.get("POMERA_PDCHARGE") == expected,
              "configure PD answer %s: %s %s" % (answer, mk, text))
        check(0 <= text.find("How should Linux use the Pomera internal documents (1 read only, default on Enter / 2 edit and save too / 3 remove automatic access at boot)? [1/2/3]") < text.find("Enable FDTP PD hub charging"),
              "configure PD question was not after internal memory: " + text)
    code, text, mk = m.configure("--yes", pdcharge_probe="exit 0")
    check(code == 0 and mk and mk.get("POMERA_PDCHARGE") == "yes", "configure PD default: %s %s" % (mk, text))
    code, text, mk = m.configure("--yes", pdcharge_probe="exit 1")
    check(code == 0 and mk and mk.get("POMERA_PDCHARGE") == "keep", "configure PD skipped: %s %s" % (mk, text))
    code, text, mk = m.configure("--yes", "--no-pomera", pdcharge_probe="echo probe-should-not-run; exit 0")
    check(code == 0 and mk and mk.get("POMERA_PDCHARGE") == "keep" and "probe-should-not-run" not in text,
          "configure --no-pomera probed PD: %s %s" % (mk, text))
    source = os.path.join(POMERA, "pomera_questions.sh")
    body = '. "$QUESTION_SOURCE"; msg_init; tty_init "$TEST_YES"; ask_pdcharge; echo "RESULT=$PDCHARGE"'
    for shell in SHELLS:
        for probe, tty, yes, answers, expected in (
                ("0", "no", "1", "", "yes"),
                ("0", "yes", "0", "n\n", "no"),
                ("0", "yes", "0", "wrong\ny\n", "yes"),
                ("1", "yes", "0", "y\n", "keep")):
            env = base_env(QUESTION_SOURCE=source, PDIR=fake, UID_NOW="0", DT=m.dt,
                           FDTP_CONFIGURE_TTY=tty, FAKE_CHARGE_PROBE=probe, TEST_YES=yes)
            code, text = run([shell, "-c", body], env=env, stdin=answers)
            check(code == 0 and "RESULT=" + expected in text,
                  "%s PD question probe=%s answer=%r: %s" % (shell, probe, answers, text))
            if expected == "yes":
                check("pomera_pdcharge.sh remove" in text and re.search(r"2\.0 ?A", text),
                      "PD question did not explain current and undo: " + text)
            if answers.startswith("wrong"):
                check("Please answer y or n." in text, "PD invalid input was accepted: " + text)
        env = base_env(QUESTION_SOURCE=source, PDIR=fake, UID_NOW="0", DT=m.dt,
                       FDTP_CONFIGURE_TTY="no", FAKE_CHARGE_PROBE="0", TEST_YES="1", POMERA_PDCHARGE_DEFAULT="n")
        code, text = run([shell, "-c", body], env=env)
        check(code == 0 and "RESULT=no" in text and "[y/N]" in text,
              "%s PD configured default no: %s" % (shell, text))
    m.device()


def test_pdcharge_setup(m):
    print("== PD charging setup status and undo (fake helpers)")
    fake = os.path.join(m.root, "charge-setup")
    os.makedirs(fake, exist_ok=True)
    shutil.copy(os.path.join(POMERA, "pomera_questions.sh"), fake)
    log = os.path.join(m.root, "charge-setup-log")
    helpers = {
        "pomera_stability.sh": 'exit 1\n',
        "pomera_mount_internal.sh": 'exit 1\n',
        "pomera_dma_pio.sh": 'case "$1" in state) echo original ;; backup-dir) echo /fake ;; *) exit 8 ;; esac\n',
        "pomera_pdcharge.sh": 'echo "$*" >> "$FAKE_CHARGE_LOG"\ncase "$1" in status) [ "$FAKE_CHARGE_SET" = yes ] ;; probe) exit 0 ;; remove) exit 0 ;; *) exit 8 ;; esac\n',
    }
    for name, body in helpers.items():
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body)
    m.device(POMERA_DT)
    env = base_env(FDTP_POMERA_DIR=fake, FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID="0",
                   FDTP_CONFIGURE_TTY="no", FAKE_CHARGE_LOG=log, FAKE_CHARGE_SET="yes")
    setup = os.path.join(POMERA, "pomera_setup.sh")
    code, text = run(["sh", setup, "--status"], env=env)
    check(code == 0 and "Additional PD hub charging program: configured (charge LED availability not checked)" in text, "setup did not report PD service: " + text)
    code, text = run(["sh", setup, "--undo", "--yes"], env=env)
    calls = open(log).read().splitlines()
    check(code == 0 and calls.count("remove") == 1, "setup did not undo PD service: %s %s" % (calls, text))
    os.remove(log)
    env["FAKE_CHARGE_SET"] = "no"
    code, text = run(["sh", setup, "--status"], env=env)
    check(code == 1, "setup status found absent PD service: " + text)
    code, text = run(["sh", setup, "--undo", "--yes"], env=env)
    calls = open(log).read().splitlines()
    check(code == 0 and "remove" not in calls, "setup removed absent PD service: %s %s" % (calls, text))
    m.device()


def test_pdcharge_safety(m):
    """Unsupported hosts never touch services, I2C or GPIO; IDs are read first."""
    print("== PD charging hardware safety")
    script = os.path.join(POMERA, "pomera_pdcharge.sh")
    sysfs = os.path.join(m.root, "charge-sys")
    os.makedirs(sysfs, exist_ok=True)
    log = os.path.join(m.root, "charge-systemctl")
    fakebin = os.path.join(m.root, "charge-bin")
    os.makedirs(fakebin, exist_ok=True)
    with open(os.path.join(fakebin, "systemctl"), "w") as f:
        f.write('#!/bin/sh\necho "$*" >> "$FAKE_SYSTEMCTL_LOG"\nexit 0\n')
    os.chmod(os.path.join(fakebin, "systemctl"), 0o755)
    env = base_env(FDTP_CONFIGURE_DT=m.dt, FDTP_PDCHARGE_SYSFS=sysfs,
                   FAKE_SYSTEMCTL_LOG=log, PATH=fakebin + os.pathsep + os.environ.get("PATH", ""))
    for device in (b"raspberrypi,4-model-b\0", POMERA_DT):
        m.device(device)
        for command in ("probe", "install", "remove", "status"):
            code, text = run(["sh", script, command], env=env)
            check(code == 1,
                  "unsafe PD %s exit %s: %s" % (command, code, text))
            check(not os.path.exists(log), "unsafe PD %s called systemctl" % command)
    # Load the module without executing main. A supported DT and driver still
    # require the RK818 identity before any charger or GPIO writes.
    spec = importlib.util.spec_from_file_location("test_pdcharge", os.path.join(POMERA, "pdcharge.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    os.makedirs(os.path.join(sysfs, "bus", "platform", "drivers", "rk818-charger"), exist_ok=True)
    with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(mod, "SYSFS", sysfs):
        for device, sysroot in ((b"raspberrypi,4-model-b\0", sysfs),
                                (POMERA_DT, os.path.join(m.root, "missing-driver"))):
            m.device(device)
            with mock.patch.object(mod, "SYSFS", sysroot), mock.patch.object(mod.os, "open") as opened:
                try:
                    mod.open_checked()
                    check(False, "PD accepted unsupported hardware before I2C")
                except mod.UnsafeHardware:
                    pass
                check(not opened.called, "PD opened I2C before DT/driver safety passed")
        m.device(POMERA_DT)
        for id17, id18, read_error, safe in ((0x81, 0x81, False, True),
                                            (0x81, 0x8F, False, True),
                                            (0x00, 0x81, False, False),
                                            (0x81, 0x90, False, False),
                                            (0x81, 0x81, True, False)):
            transfers = []

            def ioctl(fd, request, arg):
                if request == mod.I2C_SMBUS:
                    transfers.append((arg.read_write, arg.command))
                    if read_error:
                        raise OSError("fake I2C read failed")
                    arg.data.contents.byte = {0x17: id17, 0x18: id18}.get(arg.command, 0)
                return 0

            with mock.patch.object(mod.os, "open", return_value=123), \
                    mock.patch.object(mod.os, "close"), \
                    mock.patch.object(mod.fcntl, "ioctl", side_effect=ioctl):
                try:
                    pmic = mod.open_checked()
                    got_safe = True
                except mod.UnsafeHardware:
                    got_safe = False
                check(got_safe == safe, "PD identity %02x/%02x read_error=%s" % (id17, id18, read_error))
                check(transfers and all(rw == mod.I2C_SMBUS_READ for rw, reg in transfers),
                      "PD identity check wrote an I2C register: %s" % transfers)
                check(all(reg in (0x17, 0x18) for rw, reg in transfers[:2])
                      and (safe or all(reg != mod.REG for rw, reg in transfers)),
                      "PD check accessed charger register before identity: %s" % transfers)
        with mock.patch.object(mod, "open_checked", side_effect=mod.UnsafeHardware("fake unsafe")), \
                mock.patch.object(mod, "Leds") as leds, \
                mock.patch.object(sys, "argv", ["pdcharge.py"]), contextlib.redirect_stdout(io.StringIO()):
            result = mod.main()
            check(result in (None, 0), "unsafe normal PD daemon did not exit successfully")
            check(not leds.called, "unsafe PD daemon initialized GPIO")
    # Python's entry point also distinguishes an unsafe probe from a quiet
    # successful daemon skip, on an unsupported DT (no /dev access needed).
    m.device(b"raspberrypi,4-model-b\0")
    for args, expected in (([], 0), (["--probe"], 1)):
        code, text = run([sys.executable, "-B", os.path.join(POMERA, "pdcharge.py")] + args, env=env)
        check(code == expected and len(text.strip().splitlines()) == 1,
              "unsafe Python PD %s: %s %s" % (args, code, text))
    m.device()


def test_pdcharge_loop(m):
    """One daemon iteration plus SIGTERM, with a fake PMIC and no GPIO module."""
    print("== PD charging current and shutdown (fake PMIC)")
    spec = importlib.util.spec_from_file_location("loop_pdcharge", os.path.join(POMERA, "pdcharge.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    class Pmic:
        def __init__(self, value):
            self.value, self.writes, self.closed = value, [], False

        def read(self):
            return self.value

        def write(self, value):
            self.value = value
            self.writes.append(value)

        def close(self):
            self.closed = True

    # 0xF0 also checks that the unrelated upper nibble survives both writes.
    # An initial 2 A limit belongs to somebody else and must not be reset.
    for initial, online, external, want in ((0xF0, True, None, [0xF7, 0xF0]),
                                             (0xF7, True, None, []),
                                             (0xFB, True, None, []),
                                             (0xF0, False, None, []),
                                             (0xF0, True, 0xFB, [0xF7])):
        pmic = Pmic(initial)
        handlers = {}

        def stop_after_one(interval):
            if external is not None:
                pmic.value = external
            handlers[mod.signal.SIGTERM]()

        with mock.patch.object(mod, "open_checked", return_value=pmic), \
                mock.patch.object(mod, "usb_online", return_value=online), \
                mock.patch.object(mod.signal, "signal", side_effect=lambda sig, fn: handlers.update({sig: fn})), \
                mock.patch.object(mod.time, "sleep", side_effect=stop_after_one), \
                mock.patch.dict(sys.modules, {"gpiod": None}), \
                mock.patch.object(sys, "argv", ["pdcharge.py"]), contextlib.redirect_stdout(io.StringIO()) as output:
            result = mod.main()
        check(result in (None, 0) and pmic.closed and pmic.writes == want,
              "PD loop initial=%02x online=%s external=%s writes=%s expected=%s" %
              (initial, online, external, pmic.writes, want))
        check("LEDs disabled" in output.getvalue() or "LED は使いません" in output.getvalue(),
              "PD charging did not continue without gpiod: " + output.getvalue())


def test_cve_questions(m):
    print("== CVE mitigation question (fake probe)")
    m.device(POMERA_DT)
    for answer, expected in (("y", "yes"), ("n", "no")):
        code, text, mk = m.configure("--lang=en", uid="0", probe="echo vfat", pdcharge_probe="exit 0",
                                     cve_probe="exit 0", tty="yes", stdin="n\n1\nn\n" + answer + "\n")
        check(code == 0 and mk and mk.get("POMERA_CVE") == expected,
              "configure CVE answer %s: %s %s" % (answer, mk, text))
        check(0 <= text.find("Enable FDTP PD hub charging") < text.find("Enable the network vulnerability CVE-2026-98432 mitigation?\nIf yes: block affected data\nIf no: remove earlier mitigation too\nAnswer:"),
              "CVE question was not fourth: " + text)
    for args, probe, expected in ((["--yes"], "exit 0", "yes"), (["--yes"], "exit 1", "keep"),
                                  (["--yes", "--no-pomera"], "echo unexpected-cve-probe; exit 0", "keep")):
        code, text, mk = m.configure(*args, cve_probe=probe)
        check(code == 0 and mk and mk.get("POMERA_CVE") == expected and "unexpected-cve-probe" not in text,
              "configure CVE default/skip: %s %s" % (mk, text))
    body = '. "$QUESTION_SOURCE"; msg_init; tty_init "$TEST_YES"; ask_cve; echo "RESULT=$CVE"'
    for shell in SHELLS:
        for lang in ("en", "ja"):
            for probe, tty, yes, answers, default, expected in (
                    ("exit 0", "no", "1", "", "y", "yes"),
                    ("exit 0", "yes", "0", "n\n", "y", "no"),
                    ("exit 0", "yes", "0", "wrong\ny\n", "y", "yes"),
                    ("exit 1", "yes", "0", "y\n", "y", "keep"),
                    ("exit 0", "no", "1", "", "n", "no")):
                env = base_env(QUESTION_SOURCE=os.path.join(POMERA, "pomera_questions.sh"), PDIR=POMERA,
                               FDTP_CONFIGURE_CVE_PROBE=probe, FDTP_CONFIGURE_TTY=tty, TEST_YES=yes,
                               POMERA_CVE_DEFAULT=default, FDTP_MSG_LANG=lang)
                code, text = run([shell, "-c", body], env=env, stdin=answers)
                check(code == 0 and "RESULT=" + expected in text, "%s CVE question: %s" % (shell, text))
                if expected != "keep":
                    question = "通信の脆弱性 CVE-2026-98432 への対策を有効にしますか" if lang == "ja" else "Enable the network vulnerability CVE-2026-98432 mitigation?\nIf yes: block affected data\nIf no: remove earlier mitigation too\nAnswer:"
                    check(0 <= text.find("pomera_cve_2026_98432.sh remove") < text.find(question)
                          and all(s in text for s in ("RTP-MIDI", "mDNS", "LocalSend", "ssh", "nftables", "6.18.56", "apt upgrade")),
                          "CVE explanation/undo missing: " + text)
    m.device()


def test_cve_setup(m):
    print("== CVE setup status, undo and answers (fake helpers)")
    fake = os.path.join(m.root, "cve-setup")
    os.makedirs(fake, exist_ok=True)
    shutil.copy(os.path.join(POMERA, "pomera_questions.sh"), fake)
    log = os.path.join(fake, "calls")
    helpers = {
        "pomera_stability.sh": "exit 1\n",
        "pomera_mount_internal.sh": "exit 1\n",
        "pomera_dma_pio.sh": 'case "$1" in state) echo original ;; backup-dir) echo /fake ;; *) exit 8 ;; esac\n',
        "pomera_pdcharge.sh": "exit 1\n",
        "pomera_cve_2026_98432.sh": 'echo "cve $*" >> "$CVE_LOG"\ncase "$1" in status) [ "$FAKE_CVE_SET" = yes ] ;; probe) exit "$CVE_PROBE" ;; install|remove) exit 0 ;; *) exit 8 ;; esac\n',
        "install_pomera.sh": 'echo "answers $POMERA_CVE" >> "$CVE_LOG"\n',
    }
    for name, body in helpers.items():
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body)
    m.device(POMERA_DT)
    env = base_env(FDTP_POMERA_DIR=fake, FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID="0",
                   FDTP_CONFIGURE_TTY="yes", FDTP_CONFIGURE_HEIF="yes", CVE_LOG=log, FAKE_CVE_SET="yes", CVE_PROBE="0")
    setup = os.path.join(POMERA, "pomera_setup.sh")
    for present in ("yes", "no"):
        env["FAKE_CVE_SET"] = present
        code, text = run(["sh", setup, "--status"], env=env)
        check(code == (0 if present == "yes" else 1) and (present == "no" or "CVE-2026-98432" in text),
              "setup CVE status: " + text)
        open(log, "w").close()
        code, text = run(["sh", setup, "--undo", "--yes"], env=env)
        calls = open(log).read().splitlines()
        check(code == 0 and calls.count("cve remove") == (1 if present == "yes" else 0),
              "setup CVE undo: %s %s" % (calls, text))
    for answer, expected in (("y", "yes"), ("n", "no")):
        code, text = run(["sh", setup], env=env, stdin="n\n" + answer + "\n")
        check(code == 0 and "answers " + expected in open(log).read(), "setup CVE answer: " + text)
    env["CVE_PROBE"] = "1"
    code, text = run(["sh", setup], env=env, stdin="n\n")
    check(code == 0 and "Pomera-specific additional settings were not changed." in text, "setup skipped CVE: " + text)
    # Exercise install_pomera itself in isolation, without real services.
    shutil.copy(os.path.join(POMERA, "install_pomera.sh"), fake)
    for value, call in (("yes", "install"), ("no", "remove"), ("keep", None)):
        open(log, "w").close()
        env.update(POMERA_CVE=value, POMERA_STABILITY="no", POMERA_INTERNAL="keep")
        # install_pomera uses the real uid; fake it through PATH for this isolated test.
        with open(os.path.join(fake, "id"), "w") as f:
            f.write("#!/bin/sh\necho 0\n")
        os.chmod(os.path.join(fake, "id"), 0o755)
        env["PATH"] = fake + os.pathsep + os.environ.get("PATH", "")
        code, text = run(["sh", os.path.join(fake, "install_pomera.sh")], env=env)
        check(code == 0 and open(log).read() == ("cve " + call + "\n" if call else ""),
              "install_pomera CVE %s: %s" % (value, text))
    m.device()


def test_cve_kernel(m):
    print("== CVE kernel version table")
    script = os.path.join(POMERA, "pomera_cve_2026_98432.sh")
    m.device(POMERA_DT)
    cases = [("6.18.41-g653d8276acb7", True), ("6.12.113+deb13-arm64", False),
             ("6.12.107", True), ("7.3.1", False), ("4.3", False), ("4.4", True),
             ("6.9.1", True), ("8.0.0", False), ("bad", False), ("6.18.bad", False)]
    for series, patch in (("5.10", 272), ("5.15", 223), ("6.1", 190), ("6.6", 159),
                          ("6.12", 113), ("6.18", 56), ("7.2", 10)):
        cases += [(series + "." + str(patch - 1), True), (series + "." + str(patch), False),
                  (series + "." + str(patch + 1), False)]
    for shell in SHELLS:
        for kernel, affected in cases:
            code, text = run([shell, script, "probe"], env=base_env(FDTP_CONFIGURE_DT=m.dt, FDTP_CVE_UNAME_R=kernel))
            check(code == (0 if affected else 1) and (affected or len(text.strip().splitlines()) == 1),
                  "%s CVE kernel %s: %s %s" % (shell, kernel, code, text))
    m.device(b"raspberrypi,4-model-b\0")
    code, text = run(["sh", script, "probe"], env=base_env(FDTP_CONFIGURE_DT=m.dt, FDTP_CVE_UNAME_R="6.18.41"))
    check(code == 1 and "not a Pomera DM250" in text, "CVE probe accepted another machine: " + text)
    m.device()


def test_cve_rules(m):
    print("== CVE install/remove/status (fake nft, systemctl and apt)")
    script = os.path.join(POMERA, "pomera_cve_2026_98432.sh")
    include = 'include "/etc/nftables.d/fdtp-cve-2026-98432.nft"'
    user = '#!/usr/sbin/nft -f\n# user rules\ntable inet user { }\n'
    marker = '# pomera_cve_2026_98432: written by pomera_cve_2026_98432.sh (see the script for why)\n'
    m.device(POMERA_DT)
    for shell in SHELLS:
        for kind, enabled in (("user", "no"), ("user", "yes"), ("marker", "yes"),
                              ("shebang-marker", "no"), ("marker-no-orig", "no"), ("absent", "no")):
            with tempfile.TemporaryDirectory(dir=m.root) as root:
                etc = os.path.join(root, "etc")
                os.makedirs(etc)
                conf = os.path.join(etc, "nftables.conf")
                rule = os.path.join(etc, "nftables.d", "fdtp-cve-2026-98432.nft")
                flag = os.path.join(root, "var", "lib", "fdtp", "cve-2026-98432-enabled")
                original = user if kind == "user" else ("#!/usr/sbin/nft -f\n" if kind == "shebang-marker" else "") + marker + "table ip pomera_cve { }\n"
                if kind != "absent":
                    with open(conf, "w") as f:
                        f.write(original)
                if kind in ("marker", "shebang-marker"):
                    with open(conf + ".orig", "w") as f:
                        f.write(user)
                log = os.path.join(root, "calls")
                nft = os.path.join(root, "nft")
                ctl = os.path.join(root, "systemctl")
                apt = os.path.join(root, "apt")
                bodies = {
                    nft: 'echo "nft $*" >> "$CVE_LOG"\n[ "$1" != -c ] || exit "${CVE_CHECK:-0}"\n',
                    ctl: 'echo "ctl $*" >> "$CVE_LOG"\ncase "$1" in is-enabled) [ -f "$CVE_ENABLED" ] ;; enable) touch "$CVE_ENABLED" ;; disable) rm -f "$CVE_ENABLED" ;; esac\n',
                    apt: 'echo "apt $*" >> "$CVE_LOG"\ncp "$CVE_NFT_SOURCE" "$FDTP_CVE_NFT"\ntouch "$CVE_ENABLED"\n',
                }
                for path, body in bodies.items():
                    with open(path, "w") as f:
                        f.write("#!/bin/sh\n" + body)
                    os.chmod(path, 0o755)
                # Start without nft once, so apt must supply the fake command.
                source = nft + "-source"
                shutil.copy(nft, source)
                os.remove(nft)
                env = base_env(FDTP_CVE_ROOT=root, FDTP_CVE_NFT=nft, FDTP_CVE_SYSTEMCTL=ctl,
                               FDTP_CONFIGURE_APT=apt, FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID="0",
                               FDTP_CVE_UNAME_R="6.18.41-g653d8276acb7", CVE_LOG=log,
                               CVE_ENABLED=os.path.join(root, "enabled"), CVE_NFT_SOURCE=source)
                if enabled == "yes":
                    open(env["CVE_ENABLED"], "w").close()
                for attempt in range(2):
                    code, text = run([shell, script, "install"], env=env)
                    check(code == 0, "%s CVE install %s: %s" % (shell, kind, text))
                contents = open(conf).read()
                check(contents.splitlines().count(include) == 1, "CVE duplicate include: " + contents)
                if kind in ("user", "marker", "shebang-marker"):
                    check(contents.startswith(user), "CVE lost user rules: " + contents)
                if kind != "absent":
                    check(open(conf + ".bak-fdtp-cve").read() == original, "CVE backup changed")
                rules = open(rule).read()
                check(rules.startswith("table ip pomera_cve\ndelete table ip pomera_cve\n")
                      and "type filter hook prerouting priority raw; policy accept;" in rules
                      and "ip protocol tcp ip hdrlength != 5 counter drop" in rules
                      and "flush ruleset" not in rules, "CVE rule: " + rules)
                calls = open(log).read().splitlines()
                check(calls.count("apt install -y nftables") == 1
                      and calls.index("nft -c -f " + conf) < calls.index("ctl enable nftables.service")
                      and calls.count("nft list table ip pomera_cve") == 2, "CVE install calls: %s" % calls)
                check(os.path.exists(flag) == (enabled == "no"), "CVE enable ownership wrong")
                env["CVE_CHECK"] = "1"
                before = len(calls)
                code, text = run([shell, script, "install"], env=env)
                failed_calls = open(log).read().splitlines()[before:]
                check(code != 0 and "nft -c -f " + conf in failed_calls
                      and not any(c.startswith(("ctl enable", "ctl restart", "nft list")) for c in failed_calls),
                      "CVE applied after failed syntax check: %s %s" % (failed_calls, text))
                del env["CVE_CHECK"]
                # status/remove must work even with another DT and a fixed kernel.
                env.update(FDTP_CONFIGURE_DT=os.path.join(root, "no-dt"), FDTP_CVE_UNAME_R="7.3.1",
                           FDTP_CONFIGURE_UID="1000")
                code, text = run([shell, script, "status"], env=env)
                check(code == 0 and "IP options" in text, "CVE status: " + text)
                code, text = run([shell, script, "remove"], env=env)
                check(code == 1 and os.path.exists(rule), "CVE remove did not require root")
                env["FDTP_CONFIGURE_UID"] = "0"
                code, text = run([shell, script, "remove"], env=env)
                check(code == 0 and not os.path.exists(rule) and include not in open(conf).read(), "CVE remove: " + text)
                if kind in ("user", "marker", "shebang-marker"):
                    check(open(conf).read().startswith(user), "CVE remove lost user rules")
                calls = open(log).read().splitlines()
                check(calls.count("ctl disable nftables.service") == (1 if enabled == "no" else 0)
                      and calls.count("ctl restart nftables.service") == (2 if enabled == "no" else 3)
                      and "nft delete table ip pomera_cve" in calls and not os.path.exists(flag),
                      "CVE remove service calls: %s" % calls)
                code, text = run([shell, script, "status"], env=env)
                check(code == 1 and "not installed" in text, "CVE status after remove: " + text)
    m.device()


def test_own_kernel(m):
    print("== own kernel questions, RTC and defaults")
    m.device(POMERA_DT)
    adj = os.path.join(m.root, "adjtime")
    for shell in SHELLS:
        for lang in ("en", "ja"):
            for value, rtc in (("LOCAL", "yes"), ("UTC", "keep"), (None, "keep")):
                if value is None:
                    os.remove(adj)
                else:
                    with open(adj, "w") as f:
                        f.write("0.0 0 0.0\n0\n" + value + "\n")
                code, text, mk = m.configure("--yes", "--lang=" + lang, shell=shell, ownkernel="yes",
                                             adjtime=adj, lang=lang, uid="0", probe="echo vfat",
                                             pdcharge_probe="echo UNEXPECTED-PD; exit 0",
                                             cve_probe="echo UNEXPECTED-CVE; exit 0")
                check(code == 0 and all(mk.get(k) == v for k, v in {
                    "POMERA_STABILITY": "no", "POMERA_PDCHARGE": "keep", "POMERA_CVE": "keep",
                    "POMERA_WIFI_PS": "on", "POMERA_CLEAN_OLD": "keep", "POMERA_RTC": rtc,
                    "POMERA_INTERNAL": "ro"}.items()), "own configure: %s %s" % (mk, text))
                question = "時刻のずれを直すため、内蔵時計を UTC として読み取る設定に変えますか？\nはいの場合：変更\nいいえの場合：現在の設定を残す\n入力:" if lang == "ja" else "Read the hardware clock as UTC to correct the time shift?\nIf yes: change\nIf no: keep the current setting\nAnswer:"
                check((question in text) == (rtc == "yes") and "UNEXPECTED" not in text
                      and "Set up Wi-Fi freeze workarounds?\nIf yes: disable power saving and enable automatic restart on a freeze; SD rewriting is confirmed separately\nIf no: keep existing settings\nAnswer:" not in text and "Wi-Fi が固まる不具合への対策を設定しますか？\nはいの場合：省電力オフ・停止時の自動再起動を設定、SD の書き換えは別途確認\nいいえの場合：既存設定を変更しない\n入力:" not in text,
                      "own configure skipped questions: " + text)
            # Default remains a release-time choice.
            code, text, mk = m.configure("--yes", ownkernel="yes", wifi_default="n")
            check(code == 0 and mk.get("POMERA_WIFI_PS") == "off", "wifi default n: " + text)
    # Author kernel also asks RTC, after the unchanged four questions.
    with open(adj, "w") as f:
        f.write("0\n0\nLOCAL\n")
    code, text, mk = m.configure("--lang=en", uid="0", probe="echo vfat", pdcharge_probe="exit 0",
                                 cve_probe="exit 0", tty="yes", adjtime=adj, stdin="n\n1\nn\nn\nn\n")
    check(code == 0 and mk.get("POMERA_RTC") == "keep"
          and text.find("Enable the network vulnerability CVE-2026-98432 mitigation?\nIf yes: block affected data\nIf no: remove earlier mitigation too\nAnswer:") < text.find("Read the hardware clock as UTC to correct the time shift?\nIf yes: change\nIf no: keep the current setting\nAnswer:"),
          "author RTC question: " + text)
    m.device()


def test_own_setup_install(m):
    print("== own setup leftovers and install (fake helpers and commands)")
    fake = os.path.join(m.root, "own-setup")
    os.makedirs(fake, exist_ok=True)
    for name in ("pomera_questions.sh", "install_pomera.sh"):
        shutil.copy(os.path.join(POMERA, name), fake)
    log = os.path.join(fake, "calls")
    helpers = {
        "pomera_kernel_apt.sh": '[ "$1" = status ]\n',
        "pomera_stability.sh": 'case "$1" in present) printf "%s\\n" /etc/NetworkManager/conf.d/90-pomera-wifi-powersave.conf; if [ "$LEFT" = yes ]; then printf "%s\\n" /etc/udev/rules.d/92-pomera-dma.rules /etc/systemd/system.conf.d/90-pomera-watchdog.conf; fi ;; *) echo "stability $*" >> "$OWN_LOG" ;; esac\n',
        "pomera_pdcharge.sh": 'case "$1" in status) [ "$LEFT" = yes ] ;; *) echo "pd $*" >> "$OWN_LOG" ;; esac\n',
        "pomera_cve_2026_98432.sh": 'case "$1" in status) [ "$LEFT" = yes ] ;; *) echo "cve $*" >> "$OWN_LOG" ;; esac\n',
        "pomera_dma_pio.sh": 'case "$1" in state) echo wifi-pio ;; *) echo "UNEXPECTED pio $*" >> "$OWN_LOG"; exit 8 ;; esac\n',
        "pomera_mount_internal.sh": 'case "$1" in fstype|persisted) exit 1 ;; *) echo "UNEXPECTED mount $*" >> "$OWN_LOG"; exit 8 ;; esac\n',
        "id": 'echo 0\n',
        "timedatectl": 'echo "rtc $*" >> "$OWN_LOG"\n',
    }
    for name, body in helpers.items():
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body)
        os.chmod(os.path.join(fake, name), 0o755)
    adj = os.path.join(fake, "adjtime")
    with open(adj, "w") as f:
        f.write("0\n0\nLOCAL\n")
    env = base_env(FDTP_POMERA_DIR=fake, FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID="0",
                   FDTP_CONFIGURE_OWNKERNEL="yes", FDTP_CONFIGURE_ADJTIME=adj,
                   FDTP_CONFIGURE_TTY="yes", FDTP_CONFIGURE_HEIF="yes", FDTP_MSG_LANG="en",
                   OWN_LOG=log, LEFT="yes", PATH=fake + os.pathsep + os.environ.get("PATH", ""))
    m.device(POMERA_DT)
    # A tiny source tree lets configure use the same fake present/status helpers.
    project = os.path.join(fake, "source")
    os.makedirs(os.path.join(project, "mk"), exist_ok=True)
    open(os.path.join(project, "mk", "install.mk"), "w").close()
    os.symlink(fake, os.path.join(project, "pomera"))
    shutil.copy(CONFIGURE, os.path.join(project, "configure"))
    with mock.patch(__name__ + ".CONFIGURE", os.path.join(project, "configure")), \
            mock.patch.dict(os.environ, {"LEFT": "yes", "OWN_LOG": log}):
        for answers, args, expected in (("", ["--yes"], "yes"), ("n\nn\n", ["--lang=en"], "no")):
            code, text, mk = m.configure(*args, ownkernel="yes", tty="yes", stdin=answers)
            check(code == 0 and mk.get("POMERA_CLEAN_OLD") == expected
                  and "pomera-kernel (test release) does not need these additional workarounds for the current kernel (distributed at https://www.ekesete.net/log/?p=9565):" in text, "configure leftovers: %s %s" % (mk, text))
    setup = os.path.join(POMERA, "pomera_setup.sh")
    for shell in SHELLS:
        for left, args, answers, want in (
                ("yes", ["--yes"], "", ["stability remove-ps", "stability remove-old", "pd remove", "cve remove", "rtc set-local-rtc 0"]),
                ("yes", [], "n\nn\nn\n", ["stability install-ps"]),
                ("no", ["--yes"], "", ["stability remove-ps", "rtc set-local-rtc 0"])):
            env["LEFT"] = left
            open(log, "w").close()
            code, text = run([shell, setup] + args, env=env, stdin=answers)
            check(code == 0 and open(log).read().splitlines() == want,
                  "own setup calls: %s %s" % (open(log).read(), text))
            check(("Remove these workarounds?\nIf yes: remove the listed additional workarounds.\nIf no: keep the listed additional workarounds.\nEither way, keep the Wi-Fi power-saving setting just selected.\nAnswer:" in text) == (left == "yes"),
                  "leftovers question: " + text)
            if left == "yes":
                check("92-pomera-dma.rules" in text and "90-pomera-watchdog.conf" in text
                      and "90-pomera-wifi-powersave.conf" not in text and "pdcharge.service" in text,
                      "leftovers list: " + text)
        open(log, "w").close()
        code, text = run([shell, setup, "--undo", "--yes"], env=env)
        check(code == 0 and "Restore the SD boot settings from the original backup to undo PIO?\nIf yes: restore\nIf no: keep the current settings\nAnswer:" not in text
              and "pomera-kernel rollback" in text and "pio" not in open(log).read(), "own undo: " + text)
        # Stale answers cannot reapply the author's workarounds.
        open(log, "w").close()
        stale = dict(env, POMERA_STABILITY="yes", POMERA_PDCHARGE="yes", POMERA_CVE="yes")
        code, text = run([shell, os.path.join(fake, "install_pomera.sh")], env=stale)
        check(code == 0 and not open(log).read(), "stale own install: " + text)
        for name, value, want in (("POMERA_WIFI_PS", "off", "stability install-ps\n"),
                                  ("POMERA_WIFI_PS", "on", "stability remove-ps\n"),
                                  ("POMERA_CLEAN_OLD", "yes", "stability remove-old\npd remove\ncve remove\n"),
                                  ("POMERA_RTC", "yes", "rtc set-local-rtc 0\n")):
            open(log, "w").close()
            code, text = run([shell, os.path.join(fake, "install_pomera.sh")], env=dict(env, **{name: value}))
            check(code == 0 and open(log).read() == want, "isolated install %s: %s" % (name, text))
    m.device()


def test_own_safety(m):
    print("== own kernel refuses RSCE/charging before hardware access")
    fake = os.path.join(m.root, "own-safety")
    os.makedirs(fake, exist_ok=True)
    log = os.path.join(fake, "calls")
    for name in ("dd", "findmnt", "id", "systemctl", "timedatectl"):
        path = os.path.join(fake, name)
        with open(path, "w") as f:
            f.write('#!/bin/sh\necho UNEXPECTED >> "$OWN_LOG"\nexit 99\n')
        os.chmod(path, 0o755)
    env = base_env(FDTP_CONFIGURE_OWNKERNEL="yes", FDTP_MSG_LANG="en", OWN_LOG=log,
                   PATH=fake + os.pathsep + os.environ.get("PATH", ""))
    for shell in SHELLS:
        for script, modes in (("pomera_dma_pio.sh", ("wifi", "install", "restore")),
                              ("pomera_pdcharge.sh", ("probe", "install"))):
            for mode in modes:
                code, text = run([shell, os.path.join(POMERA, script), mode], env=env)
                check(code == 1 and "pomera-kernel (test release)" in text and not os.path.exists(log),
                      "own safety %s %s: %s" % (script, mode, text))
    spec = importlib.util.spec_from_file_location("own_pdcharge", os.path.join(POMERA, "pdcharge.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with mock.patch.dict(os.environ, {"FDTP_CONFIGURE_OWNKERNEL": "yes"}), \
            mock.patch.object(mod, "open_checked", side_effect=AssertionError("hardware accessed")), \
            contextlib.redirect_stdout(io.StringIO()) as output:
        check(mod.main() == 0 and "pomera-kernel (test release)" in output.getvalue(), "own daemon did not stop")
    code, text = run([sys.executable, "-B", os.path.join(POMERA, "pdcharge.py")], env=env)
    check(code == 0 and "pomera-kernel (test release)" in text, "own daemon subprocess: " + text)
    # Directory test (not just existence), and explicit no overriding detection.
    with mock.patch.dict(os.environ, {"FDTP_CONFIGURE_OWNKERNEL": ""}), \
            mock.patch.object(mod.os.path, "isdir", return_value=True) as isdir:
        check(mod.own_kernel(), "own daemon directory detection")
        isdir.assert_called_once_with("/usr/lib/pomera-kernel/" + os.uname().release)
    with mock.patch.dict(os.environ, {"FDTP_CONFIGURE_OWNKERNEL": "no"}), \
            mock.patch.object(mod.os.path, "isdir", side_effect=AssertionError("override ignored")):
        check(not mod.own_kernel(), "own daemon no override")


def test_stability_parts(m):
    print("== stability selective files (sandbox copy, fake iw/systemctl)")
    root = os.path.join(m.root, "stability")
    os.makedirs(root, exist_ok=True)
    source = open(os.path.join(POMERA, "pomera_stability.sh")).read()
    # Rewrite paths only in a temporary copy; none can address the host.
    source = source.replace("/etc/", root + "/etc/").replace("/sys/", root + "/sys/")
    source = source.replace("/usr/sbin/iw", root + "/iw")
    script = os.path.join(root, "stability.sh")
    with open(script, "w") as f:
        f.write(source)
    for dev in ("20078000.dma-controller", "1021c000.mmc", "10214000.mmc", "10218000.mmc"):
        os.makedirs(root + "/sys/devices/platform/" + dev)
    log = root + "/calls"
    for name, body in (("id", "echo 0"), ("iw", 'echo "iw $*" >> "$OWN_LOG"'),
                       ("systemctl", 'echo "ctl $*" >> "$OWN_LOG"')):
        with open(root + "/" + name, "w") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(root + "/" + name, 0o755)
    env = base_env(PATH=root + os.pathsep + os.environ.get("PATH", ""), OWN_LOG=log)
    paths = [root + "/etc/" + p for p in ("udev/rules.d/92-pomera-dma.rules",
              "NetworkManager/conf.d/90-pomera-wifi-powersave.conf", "systemd/system.conf.d/90-pomera-watchdog.conf")]
    for shell in SHELLS:
        for p in paths:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write("sentinel\n")
        open(log, "w").close()
        code, text = run([shell, script, "install-ps"], env=env)
        check(code == 0 and "wifi.powersave = 2" in open(paths[1]).read()
              and all(open(p).read() == "sentinel\n" for p in (paths[0], paths[2])), "install-ps files: " + text)
        code, text = run([shell, script, "remove-old"], env=env)
        check(code == 0 and os.path.isfile(paths[1]) and not any(os.path.exists(p) for p in (paths[0], paths[2])),
              "remove-old files: " + text)
        # remove-ps must also preserve old files if they still exist.
        for p in (paths[0], paths[2]):
            with open(p, "w") as f:
                f.write("sentinel\n")
        code, text = run([shell, script, "remove-ps"], env=env)
        check(code == 0 and not os.path.exists(paths[1])
              and all(open(p).read() == "sentinel\n" for p in (paths[0], paths[2])), "remove-ps files: " + text)
        check(open(log).read().splitlines() == ["iw dev wlan0 set power_save off", "ctl reload NetworkManager",
                                               "iw dev wlan0 set power_save on", "ctl reload NetworkManager"],
              "selective stability commands: " + open(log).read())


def test_kernel_apt(m):
    print("== kernel apt questions, source/install/remove/status and key validation")
    m.device(POMERA_DT)
    fake = os.path.join(m.root, "kernel-apt")
    os.makedirs(fake)
    for name in ("pomera_questions.sh", "pomera_kernel_apt.sh", "pomera-kernel.asc", "install_pomera.sh"):
        shutil.copy(os.path.join(POMERA, name), fake)
    # macOS mktemp -d can choose its OS-managed temp directory even with TMPDIR.
    # Keep gpg's throwaway directory inside this test's sandbox on every platform.
    helper_copy = os.path.join(fake, "pomera_kernel_apt.sh")
    with open(helper_copy) as f:
        helper_source = f.read()
    with open(helper_copy, "w") as f:
        f.write(helper_source.replace('gpghome=$(mktemp -d)',
                                     'gpghome=$(mktemp -d "$FDTP_KERNEL_APT_ROOT/../gpg.XXXXXXXX")'))
    log = os.path.join(fake, "calls")
    root = os.path.join(fake, "root")
    os.makedirs(root)
    key = os.path.join(root, "usr/share/keyrings/pomera-kernel.asc")
    source = os.path.join(root, "etc/apt/sources.list.d/pomera-kernel.sources")
    for name, body in {
        "id": "echo 0\n",
        "apt": 'echo "$*" >> "$APT_LOG"; exit "${APT_FAIL:-0}"\n',
        "pomera_stability.sh": 'case "$1" in remove-ps|install) exit 0 ;; *) exit 1 ;; esac\n',
        "pomera_mount_internal.sh": 'exit 1\n',
        "pomera_pdcharge.sh": 'exit 1\n',
        "pomera_cve_2026_98432.sh": 'exit 1\n',
        "pomera_dma_pio.sh": 'case "$1" in backup-dir) echo /fake ;; *) exit 1 ;; esac\n',
    }.items():
        path = os.path.join(fake, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body)
        os.chmod(path, 0o755)
    env = base_env(FDTP_POMERA_DIR=fake, FDTP_KERNEL_APT_ROOT=root, FDTP_CONFIGURE_UID="0",
                   FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_TTY="yes", FDTP_CONFIGURE_HEIF="yes",
                   FDTP_CONFIGURE_PROBE="exit 1", FDTP_CONFIGURE_APT=os.path.join(fake, "apt"),
                   FDTP_CONFIGURE_PDCHARGE_PROBE="exit 1", FDTP_CONFIGURE_CVE_PROBE="exit 1",
                   POMERA_KERNEL_APT_URL="http://apt.example.invalid/kernel/", APT_LOG=log,
                   FDTP_MSG_LANG="en", PATH=fake + os.pathsep + os.environ.get("PATH", ""))
    setup = os.path.join(POMERA, "pomera_setup.sh")
    helper = os.path.join(fake, "pomera_kernel_apt.sh")
    installer = os.path.join(fake, "install_pomera.sh")
    bundled = open(os.path.join(fake, "pomera-kernel.asc"), "rb").read()
    expected = ("Types: deb\nURIs: http://apt.example.invalid/kernel/\nSuites: stable\n"
                "Components: main\nArchitectures: armhf\nSigned-By: /usr/share/keyrings/pomera-kernel.asc\n")
    for shell in SHELLS:
        # Configure persists the default and explicit yes; it never installs.
        for lang in ("en", "ja"):
            question = ("pomera-kernel（試験版、カーネルと U-Boot の組）を apt で入れ、次回起動で試しますか？\nはいの場合：導入\nいいえの場合：導入しない、既存設定を残す\n入力:" if lang == "ja" else
                        "Install pomera-kernel (test release, the kernel and U-Boot as a set) through apt and try it on the next boot?\nIf yes: install\nIf no: do not install; keep existing settings\nAnswer:")
            code, text, mk = m.configure("--yes", "--lang=" + lang, shell=shell, lang=lang)
            check(code == 0 and mk.get("POMERA_KERNEL_APT") == "no" and question + " [y/N]" in text,
                  "kernel apt configure default: " + text)
            code, text, mk = m.configure("--lang=" + lang, tty="yes", stdin="n\ny\n", shell=shell,
                                         pdcharge_probe="exit 1", lang=lang)
            check(code == 0 and mk.get("POMERA_KERNEL_APT") == "yes" and question in text,
                  "kernel apt configure yes: " + text)
        question_body = '. "$PDIR/pomera_questions.sh"; msg_init; tty_init 1; ask_kernel_apt; echo "RESULT=$KERNEL_APT"'
        code, text = run([shell, "-c", question_body], env=dict(env, PDIR=fake, POMERA_KERNEL_APT_DEFAULT="y"))
        check(code == 0 and "[Y/n]" in text and "RESULT=yes" in text and not os.path.exists(key),
              "kernel apt default override: " + text)
        for mode in ("no", "keep"):
            code, text = run([shell, installer], env=dict(env, POMERA_KERNEL_APT=mode))
            check(code == 0 and not os.path.exists(log) and not os.path.exists(key), "kernel apt no/keep: " + text)
        code, text = run([shell, setup, "--yes"], env=env)
        check(code == 0 and "[y/N]" in text and not os.path.exists(key) and not os.path.exists(log),
              "kernel apt setup default did something: " + text)
        code, text = run([shell, setup], env=env, stdin="n\ny\n")
        check(code == 0 and os.path.exists(key) and open(key, "rb").read() == bundled
              and open(source).read() == expected and os.stat(key).st_mode & 0o777 == 0o644
              and os.stat(source).st_mode & 0o777 == 0o644
              and open(log).read().splitlines() == ["update", "install -y pomera-kernel"]
              and "kept for later boots" in text
              and "./configure" in text and "sudo make install" in text and "sudo fdtp-pomera-setup" in text, "kernel apt install: " + text)
        # --status and --undo include a source even when nothing else is set.
        for lang, label in (("en", "apt distribution source registration / signature verification key for pomera-kernel (test release)"), ("ja", "pomera-kernel（試験版）を apt で入れるための配布元の登録・署名確認用の鍵")):
            code, text = run([shell, setup, "--status"], env=dict(env, FDTP_MSG_LANG=lang))
            check(code == 0 and label in text, "kernel apt setup status: " + text)
        code, text = run([shell, setup, "--undo", "--yes"], env=env)
        check(code == 0 and "kernel stays installed" in text and not os.path.exists(key)
              and not os.path.exists(source), "kernel apt undo: " + text)
        os.remove(log)
        own = dict(env, FDTP_CONFIGURE_OWNKERNEL="yes")
        for want in ("source", "keep"):
            code, text, mk = m.configure("--yes", ownkernel="yes", kernel_root=root, shell=shell)
            check(code == 0 and mk.get("POMERA_KERNEL_APT") == want
                  and "Install pomera-kernel (test release) through apt and try" not in text, "own apt configure: " + text)
            question_body = '. "$PDIR/pomera_questions.sh"; msg_init; tty_init 1; ask_kernel_apt; echo "RESULT=$KERNEL_APT"'
            code, text = run([shell, "-c", question_body], env=dict(own, PDIR=fake))
            check(code == 0 and text == "RESULT=" + want + "\n", "own apt question: " + text)
            code, text = run([shell, setup, "--yes"], env=own)
            check(code == 0 and os.path.exists(key) and not os.path.exists(log)
                  and "Install pomera-kernel (test release) through apt and try" not in text, "own apt setup: " + text)
        # Either file alone means status=true, as specified.
        os.remove(key)
        code, text = run([shell, helper, "status"], env=env)
        check(code == 0, "kernel source-only status: " + text)
        code, text = run([shell, helper, "source"], env=env)
        os.remove(source)
        code, text = run([shell, helper, "status"], env=env)
        check(code == 0, "kernel key-only status: " + text)
        code, text = run([shell, helper, "remove"], env=env)
        code, text = run([shell, helper, "status"], env=env)
        check(code == 1, "kernel apt empty status: " + text)
        # Missing gpg must stop before placing either file or calling apt.
        nogpg = os.path.join(fake, "nogpg")
        os.makedirs(nogpg, exist_ok=True)
        if not os.path.exists(os.path.join(nogpg, "dirname")):
            os.symlink(shutil.which("dirname"), os.path.join(nogpg, "dirname"))
        code, text = run([shutil.which(shell), helper, "source"], env=dict(env, PATH=nogpg))
        check(code == 1 and "needs gpg" in text and not os.path.exists(key)
              and not os.path.exists(source) and not os.path.exists(log), "kernel apt missing gpg: " + text)
        code, text = run([shell, helper, "source"], env=dict(env, POMERA_KERNEL_APT_URL=""))
        check(code == 0 and "URIs: https://yasoteien.github.io/pomera-kernel-apt/\n" in open(source).read()
              and not os.path.exists(log), "kernel apt default URL: " + text)
        code, text = run([shell, helper, "remove"], env=env)
        # Fake gpg reports a different primary fingerprint without importing keys.
        badbin = os.path.join(fake, "badbin")
        os.makedirs(badbin, exist_ok=True)
        with open(os.path.join(badbin, "gpg"), "w") as f:
            f.write("#!/bin/sh\nprintf 'pub::::::::::\\nfpr:::::::::0000000000000000000000000000000000000000:\\n'\n")
        os.chmod(os.path.join(badbin, "gpg"), 0o755)
        bad = dict(env, PATH=badbin + os.pathsep + env["PATH"])
        for mode in ("source", "install"):
            code, text = run([shell, helper, mode], env=bad)
            check(code == 1 and "fingerprint mismatch" in text and not os.path.exists(key)
                  and not os.path.exists(source) and not os.path.exists(log), "kernel apt wrong fingerprint: " + text)
        # Unreadable key and failed apt update must stop before installation.
        with open(os.path.join(fake, "pomera-kernel.asc"), "w") as f:
            f.write("invalid public key\n")
        code, text = run([shell, helper, "install"], env=env)
        check(code != 0 and not os.path.exists(key) and not os.path.exists(log), "kernel apt invalid key: " + text)
        with open(os.path.join(fake, "pomera-kernel.asc"), "wb") as f:
            f.write(bundled)
        code, text = run([shell, installer], env=dict(env, POMERA_KERNEL_APT="yes", APT_FAIL="9"))
        check(code != 0 and open(log).read() == "update\n" and "kept for later boots" not in text,
              "kernel apt update failure not propagated: " + text)
        code, text = run([shell, helper, "remove"], env=env)
        os.remove(log)
        check(not any(f.endswith((".gpg", ".kbx")) for f in os.listdir(fake)), "gpg wrote a keyring/trustdb")
    m.device()



def test_wording_regressions(m):
    print("== wording: configured ro/rw, clock-only undo, declined undo, reboot and post-kernel instructions")
    fake = os.path.join(m.root, "wording")
    os.makedirs(fake)
    shutil.copy(os.path.join(POMERA, "pomera_questions.sh"), fake)
    source = open(os.path.join(POMERA, "pomera_mount_internal.sh")).read()
    source = source.replace("/etc/", fake + "/etc/")
    # The original helper requires root and an eMMC device even for persisted.
    # Remove only those two guards in the sandbox copy; exercise the real readers.
    source = source.replace('[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }', ':')
    source = source.replace('[ -b $DEV ] || { echo "$DEV is missing: no internal eMMC here" >&2; exit 1; }', ':')
    with open(os.path.join(fake, "pomera_mount_internal.sh"), "w") as f:
        f.write(source)
    os.makedirs(os.path.join(fake, "etc/systemd/system"))
    fstab = os.path.join(fake, "etc/fstab")
    unit = os.path.join(fake, "etc/systemd/system/pomera-internal.service")
    with open(fstab, "w") as f:
        f.write("/dev/fake /mnt/pomera-internal vfat ro 0 0\n")
    log = os.path.join(fake, "calls")
    helpers = {
        "pomera_stability.sh": 'case "$1" in present) [ "$STAB" = yes ] || exit 1; printf "%s\\n" /etc/NetworkManager/conf.d/90-pomera-wifi-powersave.conf /etc/systemd/system.conf.d/90-pomera-watchdog.conf ;; remove) echo stability >> "$WORD_LOG"; exit "${REMOVE_FAIL:-0}" ;; *) exit 1 ;; esac\n',
        "pomera_pdcharge.sh": 'exit 1\n',
        "pomera_cve_2026_98432.sh": 'exit 1\n',
        "pomera_kernel_apt.sh": 'case "$1" in status) [ "$APT_SET" = yes ] ;; remove) echo apt >> "$WORD_LOG" ;; *) exit 1 ;; esac\n',
        "pomera_dma_pio.sh": 'case "$1" in state) echo "$PIO" ;; backup-dir) echo /fake ;; restore) echo restore >> "$WORD_LOG"; exit "${RESTORE_FAIL:-0}" ;; *) exit 1 ;; esac\n',
    }
    for name, body in helpers.items():
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body)
    adj = os.path.join(fake, "adjtime")
    with open(adj, "w") as f:
        f.write("0\n0\nLOCAL\n")
    m.device(POMERA_DT)
    env = base_env(FDTP_POMERA_DIR=fake, FDTP_CONFIGURE_DT=m.dt, FDTP_CONFIGURE_UID="0",
                   FDTP_CONFIGURE_ADJTIME=adj, FDTP_CONFIGURE_TTY="yes", WORD_LOG=log,
                   STAB="no", PIO="original", APT_SET="no")
    setup = os.path.join(POMERA, "pomera_setup.sh")
    mount = os.path.join(fake, "pomera_mount_internal.sh")
    for shell in SHELLS:
        for lang, ro, rw, clock, none in (
                ("ja", "内蔵文書：読み取り専用で使う設定", "内蔵文書：読み書きで使う設定", "時計の読み取り設定は対象外", "このコマンドで解除できる追加設定はありません"),
                ("en", "Internal documents: read-only access configured", "Internal documents: read-write access configured", "Hardware clock interpretation is outside", "There are no additional settings this command can remove")):
            env["FDTP_MSG_LANG"] = lang
            for mode, want, absent in (("ro", ro, rw), ("rw", rw, ro)):
                if mode == "rw":
                    open(unit, "w").close()
                code, text = run([shell, mount, "persisted-mode"], env=env)
                check(code == 0 and text.strip() == mode, "real configured-mode reader: " + text)
                code, text = run([shell, setup, "--status"], env=env)
                check(code == 0 and want in text and absent not in text, "configured status %s: %s" % (mode, text))
            os.remove(unit)
            # Read-write service alone is sufficient, independently of fstab.
            content = open(fstab).read()
            open(fstab, "w").close()
            open(unit, "w").close()
            code, text = run([shell, setup, "--status"], env=env)
            check(code == 0 and rw in text and ro not in text, "service-only rw status: " + text)
            os.remove(unit)
            code, text = run([shell, setup, "--undo", "--yes"], env=env)
            check(code == 0 and clock in text and none in text and "sudo reboot" not in text,
                  "clock-only undo: " + text)
            with open(fstab, "w") as f:
                f.write(content)
            # Both removal questions declined: no writes and no reboot advice.
            open(log, "w").close()
            code, text = run([shell, setup, "--undo"], env=dict(env, STAB="yes", PIO="wifi-pio"), stdin="n\nn\n")
            check(code == 0 and "sudo reboot" not in text and not open(log).read(), "declined undo: " + text)
            check(text.count("大文字の方を選びます" if lang == "ja" else "Press Enter without typing") == 1,
                  "hint not shown exactly once: " + text)
            # Isolate each successful, failed, or immediate-only change.
            open(fstab, "w").close()
            for extra, answers, calls, reboot in (
                    ({"STAB": "yes"}, "y\n", "stability\n", True),
                    ({"STAB": "yes", "REMOVE_FAIL": "8"}, "y\n", "stability\n", False),
                    ({"PIO": "wifi-pio"}, "y\n", "restore\n", True),
                    ({"PIO": "wifi-pio", "RESTORE_FAIL": "8"}, "y\n", "restore\n", False),
                    ({"APT_SET": "yes"}, "y\n", "apt\n", False)):
                open(log, "w").close()
                code, text = run([shell, setup, "--undo"], env=dict(env, **extra), stdin=answers)
                check(code == 0 and open(log).read() == calls and ("sudo reboot" in text) == reboot,
                      "conditional reboot %s: %s" % (extra, text))
            for pio, label in (("wifi-pio", "Wi-Fi だけ" if lang == "ja" else "Only Wi-Fi"),
                               ("pio", "Wi-Fi 以外も" if lang == "ja" else "Wi-Fi and other devices")):
                code, text = run([shell, setup, "--status"], env=dict(env, PIO=pio))
                check(code == 0 and label in text and "PIO" in text, "PIO status label: " + text)
            with open(fstab, "w") as f:
                f.write(content)
    m.device()



def test_review2_wording(m):
    print("== review 2: USB conditions, standalone removal list, login mount state and public help")
    m.device(POMERA_DT)
    for lang, required in (
            ("ja", ("USB 接続中、充電に使う入力電流の上限が 2.0 A 未満なら 2.0 A に引き上げます。",
                    "すでに 2.0 A 以上なら変えません。ハブの有無は判別しません。",
                    "有効な間は、PC の USB ポートなど給電能力の低い接続先では使わないでください。")),
            ("en", ("While USB is connected, raise the charging input current limit to 2.0 A if it is below 2.0 A.",
                    "If it is already 2.0 A or higher, leave it unchanged. The program does not detect whether a hub is present.",
                    "While enabled, do not use connections with low power capacity, such as a PC USB port."))):
        code, text, mk = m.configure("--yes", "--lang=" + lang, uid="0", probe="echo vfat",
                                     pdcharge_probe="exit 0", cve_probe="exit 0", lang=lang)
        check(code == 0 and all(part in text for part in required), "USB conditions / warning " + lang + ": " + text)
        summary = text.split("== 設定（" if lang == "ja" else "== Settings (", 1)[1]
        check(("起動時に読み取り専用で自動利用" if lang == "ja" else "automatic read-only access at boot") in summary
              and ("既存設定を変更しない" if lang == "ja" else "keep existing settings") in summary,
              "summary meanings " + lang + ": " + summary)
    fake = os.path.join(m.root, "review2")
    os.makedirs(fake)
    helpers = {
        "pomera_stability.sh": 'case "$1" in present) case "$LEFT" in dma|all) echo /etc/udev/rules.d/92-pomera-dma.rules ;; esac; case "$LEFT" in watchdog|all) echo /etc/systemd/system.conf.d/90-pomera-watchdog.conf ;; esac ;; esac\n',
        "pomera_pdcharge.sh": '[ "$LEFT" = pd ] || [ "$LEFT" = all ]\n',
        "pomera_cve_2026_98432.sh": '[ "$LEFT" = cve ] || [ "$LEFT" = all ]\n',
    }
    for name, body in helpers.items():
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body)
    labels = {
        "ja": ("機器の省電力に関する旧対策", "停止時の自動再起動", "PD ハブ充電の旧対策", "通信の脆弱性への旧対策（通信を遮断する部品 nftables）"),
        "en": ("Old device power-saving workaround", "Automatic restart on a freeze", "Old PD hub charging workaround", "Old network vulnerability workaround (nftables traffic filter)"),
    }
    script = '. "$QUESTIONS"; msg_init; TTY=yes; WIFI_PS=off; ask_leftovers; printf "\\nCLEAN=%s WIFI=%s\\n" "$CLEAN_OLD" "$WIFI_PS"'
    for shell in SHELLS:
        for lang in ("ja", "en"):
            for left, indexes in (("dma", (0,)), ("watchdog", (1,)), ("pd", (2,)), ("cve", (3,)), ("all", (0, 1, 2, 3))):
                env = base_env(QUESTIONS=os.path.join(POMERA, "pomera_questions.sh"), PDIR=fake,
                               FDTP_MSG_LANG=lang, LEFT=left)
                code, text = run([shell, "-c", script], env=env, stdin="n\n")
                question = text.split("向けの次の追加対策は、" if lang == "ja" else "pomera-kernel (test release) does not need these additional workarounds", 1)[1]
                check(code == 0 and all((label in question) == (i in indexes) for i, label in enumerate(labels[lang]))
                      and ("直前に選んだ Wi-Fi の省電力設定は残します。" if lang == "ja" else "keep the Wi-Fi power-saving setting just selected.") in question
                      and "CLEAN=no WIFI=off" in text, "standalone removal targets %s %s: %s" % (lang, left, text))
    source = open(os.path.join(POMERA, "pomera_mount_internal.sh")).read()
    profile = source.split("cat > $PROFILE <<'PROFILE_END'\n", 1)[1].split("\nPROFILE_END", 1)[0]
    statefile = os.path.join(fake, "check")
    profile = profile.replace("/var/lib/pomera-internal/check", statefile)
    for shell in SHELLS:
        for lang in ("ja", "en"):
            for mount, expected, absent in (
                    ("ro", "読み取り専用で利用できます。" if lang == "ja" else "Available read-only.", "現在は利用できません。" if lang == "ja" else "Currently unavailable."),
                    ("none", "現在は利用できません。" if lang == "ja" else "Currently unavailable.", "読み取り専用で利用できます。" if lang == "ja" else "Available read-only.")):
                with open(statefile, "w") as f:
                    f.write("state=damaged\nmount=" + mount + "\n")
                code, text = run([shell, "-c", profile], env=base_env(LANG=lang))
                check(code == 0 and expected in text and absent not in text
                      and ("読み書きできることを確認できませんでした。" if lang == "ja" else "could not confirm that internal documents") in text,
                      "login mount state %s %s: %s" % (lang, mount, text))
        code, help_text = run([shell, os.path.join(POMERA, "pomera_setup.sh"), "--help"])
        # Usage is reached after hardware guards: inspect the exact excerpt without accessing hardware.
        source = open(os.path.join(POMERA, "pomera_dma_pio.sh")).read()
        first, last = map(int, re.search(r"sed -n '(\d+),(\d+)p'", source).groups())
        usage_text = "\n".join(source.splitlines()[first - 1:last])
        for kind, text in (("help", help_text), ("usage", usage_text)):
            check("pomera-kernel (test release)" in text and "previous kernel and U-Boot together" in text
                  and not re.search(r"own kernel|自前|作者|FDTP_CONFIGURE_", text, re.I), "public " + kind + ": " + text)
    m.device()

def test_private(tmp):
    """The checks of tools/make_public.sh on these files (when it is there:
    it is not in the public copy)."""
    print("== no private names")
    public = os.path.join(APP, "tools", "make_public.sh")
    if not os.path.exists(public):
        print("  (tools/make_public.sh not here: skipped)")
        return
    with open(public, encoding="utf-8") as f:
        m = re.search(r"^PRIVATE='([^']*)'", f.read(), re.M)
    check(m, "PRIVATE= not found in make_public.sh")
    if not m:
        return
    pattern = re.compile(m.group(1), re.I)
    files = [CONFIGURE, os.path.join(APP, "mk", "install.mk"), os.path.abspath(__file__),
             os.path.join(APP, "tools", "make_deb.sh"), os.path.join(APP, "tools", "make_sysroot.sh")]
    debian = os.path.join(APP, "packaging", "debian")
    if os.path.isdir(debian):
        files += [os.path.join(debian, f) for f in sorted(os.listdir(debian))]
    files += [os.path.join(POMERA, f) for f in sorted(os.listdir(POMERA)) if not f.startswith(".") and f != "__pycache__"]
    for path in files:
        if os.path.isdir(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                check(not pattern.search(line) and not pattern.search(os.path.basename(path)),
                      "%s:%d private: %s" % (os.path.relpath(path, APP), n, line.strip()[:80]))


def main():
    tmp = tempfile.mkdtemp(prefix="fdtp-configure-")
    try:
        m = Machine(tmp)
        for test, args in ((test_syntax, ()), (test_deps_cover_makefiles, ()), (test_darwin, (m,)),
                           (test_linux_deps, (m,)), (test_options, (m,)), (test_pomera, (m,)), (test_install, (m,)),
                           (test_make_reads_config, (m,)), (test_make_pio, (tmp,)), (test_install_pomera, (m,)),
                           (test_setup, (m,)), (test_heif, (m,)), (test_pdcharge_questions, (m,)),
                           (test_pdcharge_setup, (m,)), (test_pdcharge_safety, (m,)), (test_pdcharge_loop, (m,)),
                           (test_cve_questions, (m,)), (test_cve_setup, (m,)), (test_cve_kernel, (m,)),
                           (test_cve_rules, (m,)), (test_own_kernel, (m,)), (test_own_setup_install, (m,)),
                           (test_kernel_apt, (m,)), (test_wording_regressions, (m,)), (test_review2_wording, (m,)), (test_own_safety, (m,)), (test_stability_parts, (m,)), (test_private, (tmp,))):
            try:
                test(*args)
            except Exception as e:  # a broken step is a failure, the others still run
                check(False, "%s: %s: %s" % (test.__name__, type(e).__name__, e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        print("check_configure: %d failure(s)" % len(FAILS))
        return 1
    print("check_configure: ok (%d checks)" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
