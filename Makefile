# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
APP_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
.DEFAULT_GOAL := all
UNAME := $(shell uname)
CROSS ?=
ifneq ($(CROSS),)
CROSS_TRIPLET := $(patsubst %-,%,$(CROSS))
CC := $(CROSS)gcc
# Cross build (amd64 Debian 13 + gcc-arm-linux-gnueabihf): libraries the platform does not need come from a sysroot made by
# tools/make_sysroot.sh (no root); the system's armhf -dev packages too.
SYSROOT ?= $(HOME)/work/sysroot-armhf
PKG_CONFIG ?= PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/$(CROSS_TRIPLET)/pkgconfig:$(SYSROOT)/usr/share/pkgconfig:/usr/lib/$(CROSS_TRIPLET)/pkgconfig:/usr/share/pkgconfig pkg-config
SYSROOT_CFLAGS = -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/$(CROSS_TRIPLET)
# Runtime: libraries the Pomera lacks are installed beside fdtp in ~/.fdtp/lib
# (tools/deploy.sh). FDTP_RPATH= (empty) leaves the RPATH out: the .deb
# (tools/make_deb.sh) takes the libraries from Debian's packages.
FDTP_RPATH ?= $$ORIGIN/../lib
SYSROOT_RPATH = -Wl,-rpath,'$(FDTP_RPATH)'
SYSROOT_LIBS = -L$(SYSROOT)/usr/lib/$(CROSS_TRIPLET) -Wl,-rpath-link,$(SYSROOT)/usr/lib/$(CROSS_TRIPLET) $(if $(FDTP_RPATH),$(SYSROOT_RPATH))
endif
PKG_CONFIG ?= pkg-config
# Where the pieces are. Two layouts: this private tree (port/app/platform
# beside fdtp/, fdtp/fonts, fdtp/icons, fdtp/pomera) and the public copy
# made by tools/make_public.sh (lib/rmp-platform, data/fonts, data/icons,
# scripts/pomera at the top). The first that is there wins.
PLATFORM_ROOT ?= $(patsubst %/sources.mk,%,$(firstword $(wildcard $(APP_DIR)/lib/rmp-platform/sources.mk $(APP_DIR)/../port/app/platform/sources.mk)))
FONTS_DIR ?= $(firstword $(wildcard $(APP_DIR)/data/fonts $(APP_DIR)/fonts))
ICONS_DIR ?= $(firstword $(wildcard $(APP_DIR)/data/icons $(APP_DIR)/icons))
POMERA_SCRIPTS ?= $(firstword $(wildcard $(APP_DIR)/scripts/pomera $(APP_DIR)/pomera))
ifeq ($(PLATFORM_ROOT),)
$(error the platform layer is missing: neither $(APP_DIR)/lib/rmp-platform nor $(APP_DIR)/../port/app/platform has sources.mk)
endif
include $(PLATFORM_ROOT)/sources.mk
BUILD ?= $(APP_DIR)/build$(if $(CROSS),-$(CROSS_TRIPLET))
CC ?= cc
export TMPDIR := $(BUILD)/tmp
# glibc hides POSIX functions under -std=c11 without a feature macro; armhf
# needs 64-bit file offsets for files over 2 GB.
CFLAGS_ALL = -O2 -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -MMD -MP $(CFLAGS)
INCLUDES = -I$(APP_DIR)/src -I$(BUILD)
COMMON = font24 cells canvas raster dirlist fdscreen keys widgets commands app filer headless fileops textview hexview jis archive config launcher pdfview slideview sysinfo splitjoin order_box pdftools pomera_notice localsend_ui
# Modules with their own libraries: mk/*.mk add to MODULES and the flags.
MODULES :=
MODULE_CFLAGS :=
MODULE_LIBS :=
include $(sort $(wildcard $(APP_DIR)/mk/*.mk))
COMMON += $(MODULES)
COMMON_OBJS = $(addprefix $(BUILD)/,$(addsuffix .o,$(COMMON)))
PLATFORM_OBJS = $(addprefix $(BUILD)/platform/,$(notdir $(PLATFORM_SRCS:.c=.o)))
DIR ?= .
OUT ?= $(BUILD)/preview.ppm
# Relative OUT is relative to the invoking directory, even with make -C fdtp.
OUT_ABS = $(if $(filter /%,$(OUT)),$(OUT),$(abspath $(if $(PWD),$(PWD),$(CURDIR))/$(OUT)))
ifeq ($(UNAME),Darwin)
all: $(BUILD)/fdtp-preview
CHECK_EXE = $(BUILD)/fdtp-preview
CHECK_MODE =
else
all: $(BUILD)/fdtp
CHECK_EXE = $(BUILD)/fdtp
CHECK_MODE = --headless
endif
$(BUILD)/font24.inc: $(APP_DIR)/tools/gen_font.py $(wildcard $(FONTS_DIR)/*.pcf.gz)
	@mkdir -p $(@D) $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_font.py --fonts $(FONTS_DIR) --output $@
$(BUILD)/jis0208.inc: $(APP_DIR)/tools/gen_jis.py
	@mkdir -p $(@D) $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_jis.py --output $@
$(BUILD)/icon_data.inc: $(APP_DIR)/tools/gen_icon.py
	@mkdir -p $(@D) $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_icon.py --output $@
$(BUILD)/fdtp_main.o: $(BUILD)/icon_data.inc
$(BUILD)/jis.o: $(BUILD)/jis0208.inc
$(BUILD)/font24.o: $(BUILD)/font24.inc
$(BUILD)/%.o: $(APP_DIR)/src/%.c
	@mkdir -p $(@D) $(BUILD)/tmp
	$(CC) $(CFLAGS_ALL) $(INCLUDES) $(SYSROOT_CFLAGS) $(MODULE_CFLAGS) -c $< -o $@
$(BUILD)/fdtp_main.o: $(APP_DIR)/src/fdtp_main.c
	@mkdir -p $(@D) $(BUILD)/tmp
	$(CC) $(CFLAGS_ALL) $(INCLUDES) $(PLATFORM_CFLAGS) -c $< -o $@
$(BUILD)/platform/%.o: $(PLATFORM_DIR)/%.c
	@mkdir -p $(@D) $(BUILD)/tmp
	$(CC) $(CFLAGS_ALL) $(PLATFORM_CFLAGS) -c $< -o $@
$(BUILD)/fdtp-preview: $(COMMON_OBJS) $(BUILD)/preview_main.o
	$(CC) -o $@ $^ $(MODULE_LIBS) $(LDFLAGS)
$(BUILD)/fdtp: $(COMMON_OBJS) $(BUILD)/fdtp_main.o $(PLATFORM_OBJS)
	$(CC) -o $@ $^ $(PLATFORM_LIBS) $(SYSROOT_LIBS) $(MODULE_LIBS) $(LDFLAGS)
preview: $(BUILD)/fdtp-preview
	$(BUILD)/fdtp-preview "$(DIR)" "$(OUT_ABS)" $(COLUMNS)
clean:
	rm -rf $(BUILD)
# FIXENV: tools/pixel_compare.py's library (fixed disk usage and file times),
# set here because /usr/bin/make itself drops DYLD_* from its environment.
check: i18n-check $(CHECK_EXE)
	$(if $(FIXENV),DYLD_INSERT_LIBRARIES="$(FIXENV)") CC="$(CC)" FDTP_COMMON="$(COMMON)" FDTP_MODULE_LIBS="$(MODULE_LIBS)" python3 -B $(APP_DIR)/tests/check.py --build $(BUILD) --executable $(CHECK_EXE) $(CHECK_MODE)
	python3 -B $(APP_DIR)/tests/check_localsend_ui.py --build $(BUILD)
	python3 -B $(APP_DIR)/tests/check_mouse.py --build $(BUILD)
	$(if $(LOCALSEND_EXE),python3 -B $(APP_DIR)/tests/check_localsend_browser_e2e.py --build $(BUILD))
# T1's refactor check against stage 0 (844bd79). The screen has changed on
# purpose since (gap before sizes, ".." not hidden), so it is run by hand only.
# Not in the public copy (it has no such commit).
ifneq ($(wildcard $(APP_DIR)/tests/compare_stage0.py),)
stage0-compare: $(CHECK_EXE)
	CC="$(CC)" python3 -B $(APP_DIR)/tests/compare_stage0.py --build $(BUILD)
.PHONY: stage0-compare
endif
.PHONY: all preview check clean
-include $(COMMON_OBJS:.o=.d) $(BUILD)/preview_main.d $(BUILD)/fdtp_main.d $(PLATFORM_OBJS:.o=.d)
