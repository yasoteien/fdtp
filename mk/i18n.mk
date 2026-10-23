# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Shared by native and cross builds. Catalogs are generated on the host.
MODULES += i18n
I18N_INPUTS = $(APP_DIR)/src $(APP_DIR)/i18n/en $(APP_DIR)/tools/gen_i18n.py $(wildcard $(APP_DIR)/src/*.c) $(wildcard $(APP_DIR)/i18n/en/*.tsv)
$(BUILD)/i18n_en.inc: $(I18N_INPUTS)
	@mkdir -p $(@D) $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_i18n.py --output $@
$(BUILD)/i18n.o: $(BUILD)/i18n_en.inc

# Always revalidate, including after a catalog or source has been removed.
i18n-check: $(BUILD)/i18n_en.inc
	@mkdir -p $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_i18n.py --output $(BUILD)/i18n_en.inc
i18n-report:
	@mkdir -p $(BUILD)/tmp
	python3 $(APP_DIR)/tools/gen_i18n.py --report
.PHONY: i18n-check i18n-report
