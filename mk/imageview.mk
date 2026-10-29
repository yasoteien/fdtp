# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Image viewer (docs/FDTP_VIEWERS.md stage 2). Module .mk convention: append
# source names (src/NAME.c) to MODULES, flags to MODULE_CFLAGS / MODULE_LIBS.
# libheif (HEIC) is not linked: img_modern.c reads it with dlopen (-ldl; in
# libc itself from glibc 2.34); its tiles are decoded on threads (-pthread).
MODULES += imageview img_decode img_modern img_bmp img_legacy
ifeq ($(UNAME),Darwin)
MODULE_CFLAGS += $(foreach p,jpeg-turbo libpng giflib webp,-I/opt/homebrew/opt/$(p)/include)
MODULE_LIBS += $(foreach p,jpeg-turbo libpng giflib webp,-L/opt/homebrew/opt/$(p)/lib) -ljpeg -lpng -lgif -lwebp -lm
else
MODULE_CFLAGS += $(shell $(PKG_CONFIG) --cflags libjpeg libpng libwebp) -pthread
MODULE_LIBS += $(shell $(PKG_CONFIG) --libs libjpeg libpng libwebp) -lgif -lm -ldl -pthread
endif
# Test entrance (tests/imageview_harness.c): links every module object with a
# main of its own instead of preview_main. Built by tests/check_imageview.py;
# also cross-built (CROSS=...) to run those checks on the Pomera (--harness).
.SECONDEXPANSION:
$(BUILD)/imageview-harness: $$(COMMON_OBJS) $(APP_DIR)/tests/imageview_harness.c
	$(CC) $(CFLAGS_ALL) $(INCLUDES) $(SYSROOT_CFLAGS) $(MODULE_CFLAGS) -o $@ $(APP_DIR)/tests/imageview_harness.c $(COMMON_OBJS) $(SYSROOT_LIBS) $(MODULE_LIBS) $(LDFLAGS)
imageview-harness: $(BUILD)/imageview-harness
.PHONY: imageview-harness
