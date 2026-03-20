# SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

SUBDIRS = third_party lib scripts src tools LICENSES

ACLOCAL_AMFLAGS = -Im4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = reffs.pc

EXTRA_DIST = README.md COPYING CONTRIBUTING INSTALL reffsd.service.in

systemddir = $(prefix)/lib/systemd/system
systemd_DATA = reffsd.service

TARBALL = $(PACKAGE)-$(VERSION).tar.bz2

$(TARBALL): dist-bzip2

mrproper:
	git clean -f -x -d

style:
	find . \( -name xdr -o -name third_party \) -prune -o \( ! -name 'config.h' -name '*.[ch]' \) -print | tr '\n' '\0' | xargs -0 -n1 clang-format --dry-run -Werror

fix-style:
	find . \( -name xdr -o -name third_party \) -prune -o \( ! -name 'config.h' -name '*.[ch]' \) -print | tr '\n' '\0' | xargs -0 -n1 clang-format -i

scanbuild:
	scan-build make

tidy:
	bear -- make
	find . \( -name xdr -o -name third_party \) -prune -o \( -name "*.c" -o -name "*.cpp" \) -print | xargs clang-tidy -p . -header-filter=.*

RPM_BUILD_DIR = ../rpmbuild

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
		--define "__python python3" \
		~/rpmbuild/SPECS/$(PACKAGE).spec
