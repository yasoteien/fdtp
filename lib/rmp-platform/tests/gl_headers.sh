#!/bin/sh
# Creates EGL/egl.h, EGL/eglext.h and GLES2/gl2.h in $1 from the Khronos
# headers that SDL2 installs (Homebrew), for Mac syntax checks and the
# recording GL mock. No EGL/GLES runtime is involved.
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
out=$1
sdl=${SDL2_HEADERS:-/opt/homebrew/include/SDL2}
[ -f "$sdl/SDL_egl.h" ] || { echo "SDL2 Khronos headers not found in $sdl" >&2; exit 1; }
mkdir -p "$out/EGL" "$out/GLES2"
cat > "$out/EGL/egl.h" <<HEADER
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS
#define __GBM__
#include "$sdl/SDL_egl.h"
HEADER
printf '#include <EGL/egl.h>\n' > "$out/EGL/eglext.h"
cat > "$out/GLES2/gl2.h" <<HEADER
#include "$sdl/SDL_opengles2_khrplatform.h"
#include "$sdl/SDL_opengles2_gl2platform.h"
#include "$sdl/SDL_opengles2_gl2.h"
HEADER
