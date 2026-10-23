# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Platform sources for port/app/Makefile (include this file).
# Paths are absolute, so the includer's directory does not matter.
# pkg-config through $(PKG_CONFIG): the Makefile sets it for cross builds.
PKG_CONFIG ?= pkg-config
PLATFORM_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PLATFORM_SRCS := \
	$(PLATFORM_DIR)/rmp_platform.c \
	$(PLATFORM_DIR)/rmp_kms.c \
	$(PLATFORM_DIR)/rmp_input.c \
	$(PLATFORM_DIR)/rmp_gfx.c \
	$(PLATFORM_DIR)/rmp_gfx_assets.c \
	$(PLATFORM_DIR)/rmp_paint.c \
	$(PLATFORM_DIR)/rmp_damage.c \
	$(PLATFORM_DIR)/rmp_pacing.c
# Pure C pieces (no Linux/GL headers): Mac tests, previews.
PLATFORM_PORTABLE_SRCS := \
	$(PLATFORM_DIR)/rmp_paint.c \
	$(PLATFORM_DIR)/rmp_paint_ref.c \
	$(PLATFORM_DIR)/rmp_damage.c \
	$(PLATFORM_DIR)/rmp_pacing.c \
	$(PLATFORM_DIR)/rmp_gfx_assets.c
PLATFORM_PKGS := libdrm gbm egl glesv2
# The X11 output (docs/PORT_X11.md) when libxcb's headers are there
# (libxcb1-dev); without them the binary is KMS only. RMP_X11=0 leaves it out.
RMP_X11 ?= $(shell $(PKG_CONFIG) --exists xcb 2>/dev/null && echo 1 || echo 0)
ifeq ($(RMP_X11),1)
PLATFORM_SRCS += $(PLATFORM_DIR)/rmp_x11.c
PLATFORM_PKGS += xcb
endif
# C11 sources: compile them with $(CC) -std=c11 (headers are C++-safe).
# Recursive (=) so pkg-config only runs where the variables are used.
PLATFORM_CFLAGS = -I$(PLATFORM_DIR) -DRMP_WITH_X11=$(RMP_X11) $(shell $(PKG_CONFIG) --cflags $(PLATFORM_PKGS) 2>/dev/null)
PLATFORM_LIBS = $(shell $(PKG_CONFIG) --libs $(PLATFORM_PKGS) 2>/dev/null) -lm
