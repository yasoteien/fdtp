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


def check(cond, what):
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
                  libs="gif mupdf uim uim-scm", shell="sh", heif="yes", apt=None, brew=None, pdcharge_probe=None):
        if os.path.exists(self.out):
            os.remove(self.out)
        env = base_env(FDTP_CONFIG_MK=self.out, FDTP_CONFIGURE_UNAME=uname, FDTP_CONFIGURE_DT=self.dt,
                       FDTP_CONFIGURE_UID=uid, PKG_CONFIG=self.pkg_config, CC=self.cc, FAKE_PC=pc, FAKE_LIBS=libs,
                       FDTP_CONFIGURE_PROBE=probe if probe is not None else "echo should-not-run; exit 1",
                       FDTP_CONFIGURE_HEIF=heif, FDTP_CONFIGURE_BREW=brew or os.path.join(self.root, "no-brew"),
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
                 "POMERA_INTERNAL": "keep", "POMERA_PDCHARGE": "keep", "HAVE_XCB": "no", "HAVE_UIM": "no"}, "Darwin config.mk: %s" % mk)
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
    i_notes, i_ask = text.find("Notes:"), text.find("Make the Wi-Fi stable? [Y/n]")
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
    i_notes, i_ask = text.find("To stop either setting:"), text.find("Choice [1/2/3]")
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
    i_notes, i_ask = text.find("注意:"), text.find("Wi-Fi を安定させますか [Y/n]")
    check(0 <= i_notes < i_ask, "ja: notes not before [Y/n]")
    check(0 <= text.find("設定をやめるには:") < text.find("選択 [1/2/3]"), "ja internal: undo instructions after choice")
    # Choose rw, decline its warning, and choose no; confirm rw; reject invalid input.
    for answers, expected in (("2\nn\n3\n", "no"), ("2\ny\n", "rw"), ("x\n1\n", "ro")):
        code, text, mk = m.configure("--lang=en", uid="0", probe="echo vfat", tty="yes", stdin="n\n" + answers)
        check(code == 0 and mk and mk.get("POMERA_INTERNAL") == expected,
              "interactive internal %r: %s %s" % (answers, mk, text))
        if answers.startswith("2"):
            check("fsck.fat -n" in text and "Mount it read-write anyway? [y/N]" in text,
                  "read-write warning missing: " + text)
            if expected == "no":
                check(text.count("Choice [1/2/3]") >= 2, "declined read-write did not return to choice: " + text)
        if answers.startswith("x"):
            check("Please answer 1, 2 or 3." in text and text.count("Choice [1/2/3]") >= 2, "invalid internal choice: " + text)
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
    check("Make the Wi-Fi stable?" in text and "cannot show Japanese" in text, "TERM=linux not in English")
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
    i_notes, i_ask = text.find("To remove it: sudo apt remove"), text.find("Opening HEIC needs libheif. Install it? [Y/n]")
    check(code == 0 and 0 <= i_notes < i_ask and "HEIC (libheif): yes" in text, "heif root --yes: " + text)
    check(os.path.exists(log) and open(log).read() == "install -y libheif1 libheif-plugin-libde265\n", "heif: apt not run as expected")
    os.remove(log)
    code, text, mk = m.configure("--lang=en", heif="no", uid="0", apt=fake_apt, tty="yes", stdin="n\n", lang="ja_JP.UTF-8")
    check(code == 0 and "HEIC を開くには libheif が要ります。入れますか [Y/n]" in text and "あとで入れるには: sudo apt install" in text
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
        f.write("POMERA_STABILITY = yes\nPOMERA_INTERNAL = ro\nPOMERA_PDCHARGE = yes\n")
    code, text = make(["uninstall"] + common, env)
    check("pomera_stability.sh remove" in text and "pomera_dma_pio.sh restore" in text and "unpersist" in text,
          "uninstall: how to undo the Pomera settings")
    check("pomera_pdcharge.sh remove" in text, "uninstall: how to undo PD charging")
    code, text = run(["make", "-n", "-C", APP, "install"] + common, env=env)
    check("install_pomera.sh" in text, "make -n install: no Pomera part")
    check(re.search(r'POMERA_PDCHARGE=["\']?yes["\']?', text), "make install did not pass PD answer: " + text)
    # A legacy config containing only the old setting is translated for the installer.
    legacy = os.path.join(m.root, "legacy.mk")
    with open(legacy, "w") as f:
        f.write("POMERA_INTERNAL_RO = yes\n")
    code, text = run(["make", "-n", "-C", APP, "install", "FDTP_CONFIG_MK=" + legacy,
                      "INSTALL_EXE=" + fake, "UNAME=Linux"], env=env)
    check(code == 0 and re.search(r'POMERA_INTERNAL=["\']?ro["\']?', text) and "install_pomera.sh" in text,
          "legacy config did not pass ro to installer: " + text)
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
                       FDTP_CONFIGURE_HEIF=heif, FDTP_CONFIGURE_APT="echo apt-ran")
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
    check(code == 0 and "Nothing was changed." in text, "setup n 3: " + text)
    i_notes, i_ask = text.find("Notes:"), text.find("Make the Wi-Fi stable? [Y/n]")
    check(0 <= i_notes < i_ask and "pomera_dma_pio.sh restore" in text and "fdtp-pomera-setup --undo" in text
          and "asked once more" in text and "sudo make install" not in text, "setup: Wi-Fi notes: " + text)
    check(0 <= text.find("To stop either setting:") < text.find("Choice [1/2/3]") and "unpersist" in text,
          "setup: internal undo instructions not before choice")
    # libheif missing: asked after the internal memory; n keeps it out.
    code, text = setup(uid="0", probe="echo vfat", tty="yes", stdin="n\n3\nn\n", heif="no")
    check(code == 0 and 0 <= text.find("Choice [1/2/3]") < text.find("Opening HEIC needs libheif. Install it? [Y/n]")
          and "apt-ran" not in text and "sudo apt install libheif1 libheif-plugin-libde265" in text, "setup heif n: " + text)
    code, text = setup(uid="0", probe="echo vfat", tty="no", heif="no")
    check("apt-ran install -y libheif1 libheif-plugin-libde265" in text, "setup heif default yes: " + text)
    code, text = setup(uid="0", probe="echo vfat", tty="yes", stdin="n\n3\n", lang="ja_JP.UTF-8")
    check(code == 0 and "何も変えませんでした。" in text and "書き換える直前にもう一度聞きます" in text, "setup ja: " + text)
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
        check(0 <= text.find("Choice [1/2/3]") < text.find("Enable PD hub charging"),
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
    check(code == 0 and "pdcharge" in text.lower(), "setup did not report PD service: " + text)
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
                           (test_private, (tmp,))):
            try:
                test(*args)
            except Exception as e:  # a broken step is a failure, the others still run
                check(False, "%s: %s: %s" % (test.__name__, type(e).__name__, e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        print("check_configure: %d failure(s)" % len(FAILS))
        return 1
    print("check_configure: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
