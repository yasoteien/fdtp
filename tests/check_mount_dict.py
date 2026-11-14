#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Test the mount helper with a private fstab and fake system commands."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''#!/bin/sh
name=${0##*/}
printf '%s' "$name" >> "$FAKE_LOG"
for arg do printf ' <%s>' "$arg" >> "$FAKE_LOG"; done
printf '\n' >> "$FAKE_LOG"
case "$name" in
id) echo 0 ;;
losetup)
    if [ "${1:-}" != -d ]; then
        [ "${FAKE_ATTACH_FAIL:-0}" = 0 ] || exit 1
        echo /dev/loop-test
    fi ;;
blkid) echo "${FAKE_TYPE:-ext4}" ;;
mount)
    case " $* " in
        *' -t ext4 -o ro,noload '*)
            [ "${FAKE_MOUNT_FAIL:-0}" = 0 ] || exit 1
            for last do :; done
            mkdir -p "$last/JDS"
            printf '%s' "${FAKE_MAGIC:-SDSJ}" > "$last/JDS/kr.jds" ;;
    esac ;;
umount)
    [ "${FAKE_UMOUNT_FAIL:-0}" = 0 ] || exit 1
    case "$1" in "$FAKE_BASE"/probe.*) rm -rf "$1/JDS" ;; esac ;;
findmnt) exit 1 ;;
systemctl) : ;;
esac
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix='fdtp-mount-test-') as tmp:
        base = Path(tmp)
        bindir = base / 'bin'
        bindir.mkdir()
        fake = bindir / 'fake'
        fake.write_text(FAKE)
        fake.chmod(0o755)
        for name in ('id', 'losetup', 'blkid', 'mount', 'umount', 'findmnt', 'systemctl'):
            (bindir / name).symlink_to(fake)
        if sys.platform == 'darwin':
            # The target runs GNU sed; BSD sed requires an explicit backup suffix.
            sed = bindir / 'sed'
            sed.write_text('#!/bin/sh\n[ "$1" = -i ] && { shift; exec /usr/bin/sed -i "" "$@"; }\nexec /usr/bin/sed "$@"\n')
            sed.chmod(0o755)
        fstab = base / 'fstab'
        original = '# existing entry\n/dev/example /example ext4 defaults 0 0\n'
        fstab.write_text(original)
        script = (ROOT / 'pomera/pomera_mount_dict.sh').read_text()
        substitutions = {
            'DEV=/dev/mmcblk0': 'DEV=' + str(base / 'device'),
            'MNT=/mnt/pomera-dict': 'MNT=' + str(base / 'mountpoint'),
            '/etc/fstab': str(fstab),
            '/tmp/pomera-dict.XXXXXX': str(base / 'probe.XXXXXX'),
            '[ -b "$DEV" ] || { echo "$DEV is missing: no internal eMMC here" >&2; exit 1; }': ': # synthetic device',
        }
        for old, new in substitutions.items():
            require(old in script, 'source contract changed: ' + old)
            script = script.replace(old, new)
        copy = base / 'mount-dict.sh'
        copy.write_text(script)
        log = base / 'calls'
        env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ['PATH'],
                   FAKE_LOG=str(log), FAKE_BASE=str(base))

        def run(*args, expected=0, **extra):
            log.write_text('')
            result = subprocess.run(['sh', str(copy), *args], env=dict(env, **extra),
                                    text=True, capture_output=True)
            require(result.returncode == expected,
                    f'{args}: exit {result.returncode}, expected {expected}: {result.stderr}')
            return log.read_text()

        for label, extra in [('bad type', {'FAKE_TYPE': 'vfat'}),
                             ('bad header', {'FAKE_MAGIC': 'BAD!'}),
                             ('mount failure', {'FAKE_MOUNT_FAIL': '1'})]:
            calls = run('persist', expected=1, **extra)
            require(fstab.read_text() == original, label + ': fstab changed')
            require('losetup <-d> </dev/loop-test>' in calls, label + ': loop not detached')
            require(not list(base.glob('probe.*')), label + ': probe directory remains')
            if label == 'bad header':
                require('umount <' in calls, label + ': mount not released')

        calls = run('persist', expected=1, FAKE_ATTACH_FAIL='1')
        require(fstab.read_text() == original, 'attach failure changed fstab')
        require('mount <' not in calls and 'blkid <' not in calls,
                'attach failure reached probe commands')
        calls = run('persist', expected=1, FAKE_UMOUNT_FAIL='1')
        require(fstab.read_text() == original, 'failed probe umount changed fstab')
        require('umount <' in calls, 'probe umount failure was not exercised')

        # The failed fake umount intentionally leaves its synthetic mount behind.
        for probe in base.glob('probe.*'):
            shutil.rmtree(probe)
        calls = run('probe')
        require(fstab.read_text() == original, 'probe changed fstab')
        require(not (base / 'fstab.bak-dict').exists(), 'probe created backup')
        require(not (base / 'mountpoint').exists(), 'probe created mountpoint')
        require('umount <' in calls and 'losetup <-d> </dev/loop-test>' in calls,
                'probe did not clean up')
        calls = run('mount')
        require(f'mount <-t> <ext4> <-o> <loop,offset=639631360,sizelimit=268435456,ro,noload> <{base}/device> <{base}/mountpoint>' in calls,
                'final mount options incorrect')
        calls = run('umount')
        require(f'umount <{base}/mountpoint>' in calls, 'umount target incorrect')

        run('persisted', expected=1)
        calls = run('persist')
        expected_line = (f'{base}/device {base}/mountpoint ext4 '
                         'loop,offset=639631360,sizelimit=268435456,ro,noload,nofail 0 0')
        contents = fstab.read_text()
        require(expected_line in contents.splitlines(), 'incorrect fstab entry: ' + contents)
        require('losetup <-r> <-o> <639631360> <--sizelimit> <268435456>' in calls,
                'probe loop options missing')
        require('mount <-t> <ext4> <-o> <ro,noload>' in calls, 'probe mount options missing')
        require('umount <' in calls and 'losetup <-d> </dev/loop-test>' in calls,
                'successful probe not cleaned up')
        require(not list(base.glob('probe.*')), 'probe directory remains')
        run('persisted')
        run('persist')
        require(fstab.read_text().splitlines().count(expected_line) == 1, 'duplicate entry')
        run('unpersist')
        require(fstab.read_text() == original, 'unpersist did not restore original entries')
        run('persisted', expected=1)
        calls = run('persist', 'rw', expected=2)
        require(not calls and fstab.read_text() == original, 'extra rw argument had effects')
        calls = run('mount', 'rw', expected=2)
        require(not calls, 'extra rw argument reached system commands')
    print('mount dict: ok')


if __name__ == '__main__':
    main()
