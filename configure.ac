dnl SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
dnl SPDX-License-Identifier: GPL-2.0+

AC_PREREQ([2.69])
AC_INIT([reffs], [0.0.1], [loghyr@gmail.com])
AM_INIT_AUTOMAKE([foreign])

AC_CONFIG_SRCDIR([src/reffs.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_SILENT_RULES([yes])

AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE
LT_INIT

AC_CHECK_HEADER_STDBOOL

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_C_TYPEOF
AC_TYPE_INT32_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

PKG_CHECK_MODULES(URCU, liburcu, ,
        [AC_MSG_ERROR([Could not configure liburcu support.])])

PKG_CHECK_MODULES(URCU_CDS, liburcu-cds, ,
        [AC_MSG_ERROR([Could not configure liburcu-cds support.])])

PKG_CHECK_MODULES(TIRPC, libtirpc, ,
        [AC_MSG_ERROR([Could not configure libtirpc support.])])

AX_PTHREAD([], [AC_MSG_ERROR([Could not configure pthread support])])
AC_CHECK_LIB([pthread], [pthread_create], , [AC_MSG_ERROR([Please install libpthread])])

LIBS="${PTHREAD_LIBS} ${TIRPC_LIBS} ${URCU_LIBS} ${URCU_CDS_LIBS} ${JEMALLOC_LIBS} ${LIBS}"
CFLAGS="${CFLAGS} ${PTHREAD_CFLAGS} ${TIRPC_CFLAGS}"

dnl Enable Address Sanitizer, if supported by gcc (4.8+) or clang.
dnl http://clang.llvm.org/docs/AddressSanitizer.html
dnl https://code.google.com/p/address-sanitizer/wiki/HowToBuild
AC_ARG_ENABLE([asan],
    [AS_HELP_STRING([--enable-asan], [Enable Address Sanitizer])],
    [enable_asan=$enableval], [enable_asan=no])
  AS_IF([test "x$enable_asan" != xno], [
    AX_CHECK_COMPILE_FLAG([-fsanitize=address],
                          [CFLAGS="$CFLAGS -fsanitize=address"],
                          [enable_asan=no], [])
    AS_IF([test "x$enable_asan" = xno], [
        AC_MSG_FAILURE(
            [--enable-asan is given, but not supported on this platform.
 Check out https://code.google.com/p/address-sanitizer/wiki/HowToBuild])])
    dnl Keep error messages nice. Also consider:
    dnl export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
    dnl export ASAN_OPTIONS=symbolize=1
    AX_CHECK_COMPILE_FLAG([-fno-omit-frame-pointer],
                          [CFLAGS="$CFLAGS -fno-omit-frame-pointer"],
                          [], [])
  ])

dnl Enable Thread Sanitizer, if supported by clang.
dnl http://clang.llvm.org/docs/ThreadSanitizer.html
dnl https://code.google.com/p/address-sanitizer/wiki/HowToBuild
AC_ARG_ENABLE([tsan],
    [AS_HELP_STRING([--enable-tsan], [Enable Thread Sanitizer])],
    [enable_tsan=$enableval], [enable_tsan=no])
  AS_IF([test "x$enable_tsan" != xno], [
    AX_CHECK_COMPILE_FLAG([-fsanitize=thread],
                          [CFLAGS="$CFLAGS -fsanitize=thread"],
                          [enable_tsan=no], [])
    AS_IF([test "x$enable_tsan" = xno], [
        AC_MSG_FAILURE(
            [--enable-tsan is given, but not supported on this platform.
 Check out https://code.google.com/p/address-sanitizer/wiki/HowToBuild])])
    dnl Keep error messages nice.
    AX_CHECK_COMPILE_FLAG([-fno-omit-frame-pointer],
                          [CFLAGS="$CFLAGS -fno-omit-frame-pointer"],
                          [], [])
    dnl -fsanitize=thread linking must be done with -pie or -shared
    dnl We can't do anything about this message in GCC; use clang.
  ])

AC_CHECK_HEADERS([stdlib.h sys/socket.h], , [AC_MSG_ERROR([Unable to find usable header file])])

AC_CONFIG_FILES([
	Makefile
	reffs.pc
	reffs.spec
	LICENSES/Makefile
	src/Makefile
	lib/Makefile
	lib/include/Makefile
	lib/include/reffs/Makefile
	lib/xdr/Makefile
	lib/tests/Makefile])

AC_OUTPUT

