# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# Optional, independent helper: no OpenSSL flags or objects enter FDTP itself.
ifeq ($(UNAME),Darwin)
ifneq ($(wildcard /opt/homebrew/opt/openssl@3/include/openssl/ssl.h),)
LOCALSEND_CFLAGS ?= -I/opt/homebrew/opt/openssl@3/include
LOCALSEND_LIBS ?= -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
endif
endif
LOCALSEND_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
LOCALSEND_LIBS ?= $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo '-lssl -lcrypto')
# Compile and link only; this also works for cross compilers. Compiler
# temporaries use the build tree; the probe never runs a target executable.
HAVE_LOCALSEND ?= $(shell mkdir -p '$(BUILD)/tmp'; printf '\043include <openssl/ssl.h>\n\043if !defined(OPENSSL_VERSION_MAJOR) || OPENSSL_VERSION_MAJOR < 3\n\043error OpenSSL 3 required\n\043endif\nint main(void){return OPENSSL_init_ssl(0,0)!=1;}\n' | TMPDIR='$(BUILD)/tmp' $(CC) $(SYSROOT_CFLAGS) $(LOCALSEND_CFLAGS) -x c - $(SYSROOT_LIBS) $(LOCALSEND_LIBS) -o /dev/null >/dev/null 2>&1 && echo yes)
ifeq ($(HAVE_LOCALSEND),yes)
LOCALSEND_EXE := $(BUILD)/fdtp-localsend
LOCALSEND_NAMES := localsend_main localsend localsend_send localsend_serve localsend_transport localsend_json zipfile i18n
LOCALSEND_OBJS := $(addprefix $(BUILD)/localsend/,$(addsuffix .o,$(LOCALSEND_NAMES)))
all: $(LOCALSEND_EXE)
check: localsend-check
$(BUILD)/localsend/%.o: $(APP_DIR)/src/%.c
	@mkdir -p $(@D) $(BUILD)/tmp
	$(CC) $(CFLAGS_ALL) $(INCLUDES) $(SYSROOT_CFLAGS) $(LOCALSEND_CFLAGS) -pthread -c $< -o $@
$(BUILD)/localsend/i18n.o: $(BUILD)/i18n_en.inc
$(LOCALSEND_EXE): $(LOCALSEND_OBJS)
	$(CC) -o $@ $^ $(SYSROOT_LIBS) $(LOCALSEND_LIBS) -lz -pthread $(LDFLAGS)
localsend: $(LOCALSEND_EXE)
localsend-check: $(LOCALSEND_EXE)
	python3 -B $(APP_DIR)/tests/check_localsend.py --executable $(LOCALSEND_EXE) --build $(BUILD)
-include $(LOCALSEND_OBJS:.o=.d)
else
$(warning OpenSSL 3 development files unavailable: skipping fdtp-localsend)
localsend localsend-check:
	@echo "OpenSSL 3 development files unavailable: fdtp-localsend skipped"
endif
.PHONY: localsend localsend-check
