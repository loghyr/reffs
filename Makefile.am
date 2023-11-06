# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

SUBDIRS = lib src LICENSES

ACLOCAL_AMFLAGS = -Im4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = reffs.pc

ASAN := 0
TSAN := 0
DEBUG := 0

EXTRA_DIST = README.md COPYING CONTRIBUTING INSTALL

TARBALL := $(PACKAGE)-$(VERSION).tar.bz2

$(TARBALL): dist-bzip2

mrproper:
	git clean -f -x -d

RPM_BUILD_DIR := ../rpmbuild

.PHONY: rpm

rpm: $(TARBALL)
	cp $(TARBALL) $(RPM_BUILD_DIR)/SOURCES/
	cp $(PACKAGE).spec $(RPM_BUILD_DIR)/SPECS/
	rpmbuild -ba 	\
		--define="_debug_enabled $(DEBUG_ENABLED)" \
		--define="_asan_enabled $(ASAN_ENABLED)" \
		--define="_tsan_enabled $(TSAN_ENABLED)" \
		--define="_lsan_enabled $(LSAN_ENABLED)" \
		--define="_ubsan_enabled $(UBSAN_ENABLED)" \
		~/rpmbuild/SPECS/$(PACKAGE).spec
