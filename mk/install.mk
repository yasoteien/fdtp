# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
# make install / make uninstall for those who build from the source
# (docs/FDTP_CONFIGURE.md). ./configure writes config.mk next to Makefile:
# PREFIX, FDTP_LANGUAGE, POMERA_STABILITY, POMERA_INTERNAL, HAVE_* and,
# without libuim, NOVEL_IME = ime_none (this file comes before mk/novel.mk,
# whose NOVEL_IME ?= then keeps it). Without config.mk the build is as
# before. Cross builds (CROSS=, tools/deploy.sh) never read it.
#
#   sudo make install     $(PREFIX)/bin/fdtp, the menu entry and icons,
#                         $(PREFIX)/share/doc/fdtp, LANGUAGE in /etc/fdtprc,
#                         and the Pomera settings answered yes in ./configure
#   sudo make uninstall   removes those files (not the Pomera settings: it
#                         shows how to undo them)
# DESTDIR= installs under another root (then /etc/fdtprc is $(DESTDIR)/etc/
# fdtprc and nothing outside DESTDIR is run). INSTALL_EXE= installs another
# program file (the tests; on macOS, where make install is refused).
FDTP_CONFIG_MK ?= $(APP_DIR)/config.mk
ifeq ($(CROSS),)
-include $(FDTP_CONFIG_MK)
endif
PREFIX ?= /usr/local
DESTDIR ?=
FDTPRC ?= /etc/fdtprc
POMERA_STABILITY ?= no
POMERA_PDCHARGE ?= keep
POMERA_INTERNAL_RO ?= no
# keep: not answered (an old config.mk's POMERA_INTERNAL_RO = no says only
# that it was not set up then), so an earlier setting is left alone.
POMERA_INTERNAL ?= $(if $(filter yes,$(POMERA_INTERNAL_RO)),ro,keep)
INSTALL_EXE ?= $(BUILD)/fdtp
BINDIR = $(PREFIX)/bin
APPSDIR = $(PREFIX)/share/applications
ICONDIR = $(PREFIX)/share/icons/hicolor
DOCDIR = $(PREFIX)/share/doc/fdtp
ICON_SIZES = 16 32 48 64 128
# README, NOTICE and LICENSE: fdtp/public in the private tree, the top of
# the public one (tools/make_public.sh). The manual's PDFs, when there are
# any: manual/*.pdf into the doc directory; docs/manual/LANG/*.pdf (the
# public tree) or public/manual/LANG/*.pdf (here) into its LANG/.
DOC_NAMES = README.md README.en.md NOTICE.md LICENSE
DOC_FILES = $(foreach n,$(DOC_NAMES),$(firstword $(wildcard $(APP_DIR)/public/$(n) $(APP_DIR)/$(n)))) \
	$(sort $(wildcard $(APP_DIR)/manual/*.pdf $(APP_DIR)/public/manual/*.pdf))
MANUAL_DIR ?= $(firstword $(wildcard $(APP_DIR)/docs/manual $(APP_DIR)/public/manual))
MANUAL_LANGS = $(sort $(foreach f,$(wildcard $(MANUAL_DIR)/*/*.pdf),$(notdir $(patsubst %/,%,$(dir $(f))))))
FDTPRC_MARK = \# Written by make install of FDTP; make uninstall removes it.
# The language of the messages of the Pomera part.
INSTALL_MSG_LANG = $(if $(filter ja en,$(FDTP_LANGUAGE)),$(FDTP_LANGUAGE),$(firstword $(LC_ALL) $(LC_MESSAGES) $(LANG) en))

ifeq ($(UNAME)/$(origin INSTALL_EXE),Darwin/file)
install uninstall:
	@echo "make $@: for Linux (macOS builds fdtp-preview only)" >&2; exit 1
else
.SECONDEXPANSION:
install: $(INSTALL_EXE) $$(LOCALSEND_EXE)
	@case "$(FDTP_LANGUAGE)" in ''|auto|ja|en) ;; *) echo "FDTP_LANGUAGE must be auto, ja or en" >&2; exit 1 ;; esac
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(APPSDIR)" "$(DESTDIR)$(DOCDIR)"
	install -m 755 "$(INSTALL_EXE)" "$(DESTDIR)$(BINDIR)/fdtp"
	$(if $(LOCALSEND_EXE),install -m 755 "$(LOCALSEND_EXE)" "$(DESTDIR)$(BINDIR)/fdtp-localsend")
	sed 's|^Exec=.*|Exec=$(BINDIR)/fdtp|' "$(ICONS_DIR)/fdtp.desktop" > "$(DESTDIR)$(APPSDIR)/fdtp.desktop"
	chmod 644 "$(DESTDIR)$(APPSDIR)/fdtp.desktop"
	for s in $(ICON_SIZES); do \
		install -d "$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps" && \
		install -m 644 "$(ICONS_DIR)/fdtp-$$s.png" "$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/fdtp.png" || exit 1; \
	done
	$(if $(strip $(DOC_FILES)),install -m 644 $(foreach f,$(DOC_FILES),"$(f)") "$(DESTDIR)$(DOCDIR)/")
	$(foreach l,$(MANUAL_LANGS),install -d "$(DESTDIR)$(DOCDIR)/$(l)" && install -m 644 $(foreach f,$(wildcard $(MANUAL_DIR)/$(l)/*.pdf),"$(f)") "$(DESTDIR)$(DOCDIR)/$(l)/" || exit 1;)
	@if [ -z "$(DESTDIR)" ]; then \
		touch "$(ICONDIR)"; \
		if [ -f "$(ICONDIR)/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null; then gtk-update-icon-cache -f -t "$(ICONDIR)" >/dev/null 2>&1 || true; fi; \
		if command -v update-desktop-database >/dev/null; then update-desktop-database "$(APPSDIR)" >/dev/null 2>&1 || true; fi; \
	fi
	@# LANGUAGE in /etc/fdtprc (only after ./configure): FDTP then skips
	@# its first-run question. An existing file keeps everything else.
	@if [ "$(FDTP_CONFIGURED)" = yes ] && [ -n "$(FDTP_LANGUAGE)" ]; then \
		f="$(DESTDIR)$(FDTPRC)"; mkdir -p "$$(dirname "$$f")" || exit 1; \
		if [ ! -f "$$f" ]; then printf '%s\n%s\n' "$(FDTPRC_MARK)" "LANGUAGE=$(FDTP_LANGUAGE)" > "$$f" || exit 1; \
		elif grep -q '^[[:space:]]*LANGUAGE[[:space:]]*=' "$$f"; then \
			sed 's/^[[:space:]]*LANGUAGE[[:space:]]*=.*/LANGUAGE=$(FDTP_LANGUAGE)/' "$$f" > "$$f.tmp" && cat "$$f.tmp" > "$$f" && rm -f "$$f.tmp" || exit 1; \
		else \
			if [ -n "$$(tail -c 1 "$$f")" ]; then echo >> "$$f"; fi; \
			echo "LANGUAGE=$(FDTP_LANGUAGE)" >> "$$f" || exit 1; \
		fi; \
		chmod 644 "$$f"; echo "$$f: LANGUAGE=$(FDTP_LANGUAGE)"; \
	fi
	@if [ -z "$(DESTDIR)" ] && { [ "$(POMERA_PDCHARGE)" = yes ] || [ "$(POMERA_PDCHARGE)" = no ] || [ "$(POMERA_STABILITY)" = yes ] || { [ "$(POMERA_INTERNAL)" = ro ] || [ "$(POMERA_INTERNAL)" = rw ] || [ "$(POMERA_INTERNAL)" = no ]; }; }; then \
		POMERA_PDCHARGE="$(POMERA_PDCHARGE)" POMERA_STABILITY="$(POMERA_STABILITY)" POMERA_INTERNAL="$(POMERA_INTERNAL)" FDTP_MSG_LANG="$(INSTALL_MSG_LANG)" \
			sh "$(POMERA_SCRIPTS)/install_pomera.sh"; \
	fi
	@echo "installed: $(BINDIR)/fdtp$(if $(DESTDIR), (under $(DESTDIR)))"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/fdtp" "$(DESTDIR)$(BINDIR)/fdtp-localsend" "$(DESTDIR)$(APPSDIR)/fdtp.desktop"
	for s in $(ICON_SIZES); do rm -f "$(DESTDIR)$(ICONDIR)/$${s}x$${s}/apps/fdtp.png"; done
	rm -rf "$(DESTDIR)$(PREFIX)/share/doc/fdtp"
	@# /etc/fdtprc only when make install made it and it has nothing else.
	@f="$(DESTDIR)$(FDTPRC)"; \
	if [ -f "$$f" ]; then \
		if [ "$$(head -n 1 "$$f")" = "$(FDTPRC_MARK)" ] && \
			[ -z "$$(sed -e 1d -e '/^[[:space:]]*LANGUAGE[[:space:]]*=/d' -e '/^[[:space:]]*$$/d' "$$f")" ]; then \
			rm -f "$$f" && echo "removed $$f"; \
		else echo "$$f is kept (it has settings of its own)"; fi; \
	fi
	@if [ "$(POMERA_PDCHARGE)" = yes ] || [ "$(POMERA_STABILITY)" = yes ] || { [ "$(POMERA_INTERNAL)" = ro ] || [ "$(POMERA_INTERNAL)" = rw ]; }; then \
		echo "The Pomera settings are not undone. To undo them:"; \
		if [ "$(POMERA_STABILITY)" = yes ]; then \
			echo "  sudo sh $(POMERA_SCRIPTS)/pomera_stability.sh remove"; \
			echo "  sudo sh $(POMERA_SCRIPTS)/pomera_dma_pio.sh restore     (if the SD was rewritten)"; \
		fi; \
		if { [ "$(POMERA_INTERNAL)" = ro ] || [ "$(POMERA_INTERNAL)" = rw ]; }; then echo "  sudo sh $(POMERA_SCRIPTS)/pomera_mount_internal.sh unpersist"; fi; \
		if [ "$(POMERA_PDCHARGE)" = yes ]; then echo "  sudo sh $(POMERA_SCRIPTS)/pomera_pdcharge.sh remove"; fi; \
		echo "and reboot."; \
	fi
endif
.PHONY: install uninstall
