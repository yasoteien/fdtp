# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Novel editor and Japanese input (docs/FDTP_NOVEL.md, stage E).
# Module .mk convention: source names (src/NAME.c) go to MODULES, flags to
# MODULE_CFLAGS / MODULE_LIBS.
NOVEL_MODULES := noveledit novel_doc novel_ruby novel_layout novel_glyph novel_format novel_docx font12
MODULES += $(NOVEL_MODULES)
# NOVEL_IME=ime_none builds a Linux binary without libuim (alphanumerics only).
ifeq ($(UNAME),Darwin)
NOVEL_IME ?= ime_none
else
NOVEL_IME ?= ime_uim
endif
ifeq ($(NOVEL_IME),ime_uim)
# libuim (uim 1.9.6 + Mozc on the Pomera). With CROSS the headers and
# libraries come from the sysroot (SYSROOT_CFLAGS / SYSROOT_LIBS).
MODULE_LIBS += -luim -luim-scm
endif
MODULES += $(NOVEL_IME)
$(BUILD)/font12.inc: $(APP_DIR)/tools/gen_font12.py $(APP_DIR)/tools/gen_font.py $(wildcard $(FONTS_DIR)/shnm*12*.pcf.gz)
	@mkdir -p $(@D) $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_font12.py --fonts $(FONTS_DIR) --output $@
$(BUILD)/font12.o: $(BUILD)/font12.inc
# For tests/check_novel.py: the objects and libraries its harness links with.
novel-link-info:
	@echo "OBJS $(COMMON_OBJS)"
	@echo "LIBS $(MODULE_LIBS) $(LDFLAGS)"
.PHONY: novel-link-info
