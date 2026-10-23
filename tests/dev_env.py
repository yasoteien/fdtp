# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Settings of the developer's own machines, kept out of the sources.

get('FDTP_BUILD_HOST', 'build-host') returns the environment variable if it
is set, else the line FDTP_BUILD_HOST=... of the file named by FDTP_DEV_ENV
(default ~/.config/fdtp-dev/env; KEY=VALUE lines, # comments), else the
default. The file is the same one tools/deploy.sh reads.
"""
import os
from pathlib import Path


def _file():
    path = Path(os.environ.get('FDTP_DEV_ENV') or Path.home() / '.config' / 'fdtp-dev' / 'env')
    values = {}
    try:
        for line in path.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def get(name, default=None):
    value = os.environ.get(name)
    if value:
        return value
    return _file().get(name, default)
