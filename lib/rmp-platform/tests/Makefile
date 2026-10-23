# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Platform tests. Run from this directory:
#   make test    unit tests (Mac or Linux; no display needed)
#   make check   -Wall -Wextra -Werror syntax check of the device-only files
# Device-only files are checked against mock headers on the Mac (DRM/GBM,
# linux/input.h) and SDL2's Khronos EGL/GLES2 headers (gl_headers.sh).
include ../sources.mk
CC ?= cc
OUT ?= build
WARN = -std=c11 -Wall -Wextra -Werror -Wimplicit-fallthrough
CFLAGS ?= -O2
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
GLH = $(OUT)/glh
GLINC = -I$(GLH)
else
GLH =
GLINC =
endif

# libxcb's headers (libxcb1-dev, or Homebrew's libxcb on the Mac) for the
# X11 output; libxcb itself is not linked (test_x11.c fakes it). Without the
# headers test_x11 and the rmp_x11.c syntax check are skipped.
XCB_H := $(firstword $(wildcard /opt/homebrew/include/xcb/xcb.h /usr/local/include/xcb/xcb.h /opt/X11/include/xcb/xcb.h /usr/include/xcb/xcb.h))
XCB_INC := $(if $(XCB_H),-I$(patsubst %/xcb/xcb.h,%,$(XCB_H)))
TESTS = $(OUT)/test_damage $(OUT)/test_pacing $(OUT)/test_kms $(OUT)/test_input $(OUT)/test_frame $(if $(XCB_H),$(OUT)/test_x11)
.PHONY: test check clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done
	@$(if $(XCB_H),:,echo "SKIP test_x11: no xcb/xcb.h (libxcb1-dev)")

$(OUT):
	mkdir -p $(OUT)
$(OUT)/glh: | $(OUT)
	./gl_headers.sh $(OUT)/glh

$(OUT)/test_damage: test_damage.c ../rmp_damage.c ../rmp_damage.h | $(OUT)
	$(CC) $(WARN) $(CFLAGS) -I.. test_damage.c ../rmp_damage.c -o $@
$(OUT)/test_pacing: test_pacing.c ../rmp_pacing.c ../rmp_pacing.h | $(OUT)
	$(CC) $(WARN) $(CFLAGS) -I.. test_pacing.c ../rmp_pacing.c -o $@ -lm
$(OUT)/test_kms: test_kms.c ../rmp_kms.c ../rmp_kms.h | $(OUT)
	$(CC) $(WARN) -O0 -g -Imock/include test_kms.c -o $@
$(OUT)/test_input: test_input.c ../rmp_input.c ../rmp_input.h | $(OUT)
	$(CC) $(WARN) -O0 -g -Imock/include -I.. test_input.c -o $@
FRAME_SRCS = test_frame.c platform_harness.c mock_gl.c ../rmp_gfx.c ../rmp_gfx_assets.c ../rmp_paint.c ../rmp_damage.c ../rmp_pacing.c
$(OUT)/test_x11: test_x11.c ../rmp_x11.c ../rmp_x11.h ../rmp_platform.h | $(OUT)
	$(CC) $(WARN) -O0 -g $(XCB_INC) -I.. test_x11.c -o $@
$(OUT)/test_frame: $(FRAME_SRCS) ../rmp_platform.c $(wildcard ../*.h) $(wildcard *.h) | $(OUT) $(GLH)
	$(CC) $(WARN) -O0 -g $(GLINC) -I.. -I. $(FRAME_SRCS) -o $@ -lm

check: | $(OUT) $(GLH)
	$(CC) $(WARN) -fsyntax-only $(GLINC) -Imock/include -I.. ../rmp_platform.c ../rmp_gfx.c ../rmp_kms.c ../rmp_input.c
	$(CC) $(WARN) -fsyntax-only $(GLINC) -I.. -DRMP_GFX_USE_VBO=1 ../rmp_gfx.c
	$(CC) $(WARN) -fsyntax-only -I.. $(PLATFORM_PORTABLE_SRCS)
	$(if $(XCB_H),$(CC) $(WARN) -fsyntax-only $(XCB_INC) -I.. ../rmp_x11.c,@echo "SKIP rmp_x11.c: no xcb/xcb.h")
	$(if $(XCB_H),$(CC) $(WARN) -fsyntax-only $(GLINC) $(XCB_INC) -Imock/include -I.. -DRMP_WITH_X11=1 ../rmp_platform.c)
	@echo "PASS check: platform sources compile with -Wall -Wextra -Werror (device headers mocked on $(UNAME))"
clean:
	rm -rf $(OUT)
