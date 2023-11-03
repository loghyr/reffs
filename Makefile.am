# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: GPL-2.0+

SUBDIRS = lib src LICENSES

ACLOCAL_AMFLAGS = -Im4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = reffs.pc

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
	rpmbuild -ba ~/rpmbuild/SPECS/$(PACKAGE).spec
