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

AC_ARG_ENABLE(asan,
[  --enable-asan      Compile with address sanitizer],
[case "${enableval}" in
	yes) asan_enabled=true ;;
	no)  asan_enabled=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-asan) ;;
esac],[asan_enabled=false])

if test "x${enable_asan}" = "xyes"; then
	ASAN_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -DASAN_ENABLED -U_FORTIFY_SOURCE"
	CFLAGS="${CFLAGS} ${ASAN_CFLAGS}"
	AC_SUBST(ASAN_CFLAGS)
	ASAN_LDFLAGS="-lasan"
	AC_SUBST(ASAN_LDFLAGS)
	LDFLAGS="${LDFLAGS} ${ASAN_LDFLAGS}"
fi

AC_ARG_ENABLE(tsan,
[  --enable-tsan      Compile with thread sanitizer],
[case "${enableval}" in
	yes) tsan_enabled=true ;;
	no)  tsan_enabled=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-tsan) ;;
esac],[tsan_enabled=false])

if test "x${enable_tsan}" = "xyes"; then
	TSAN_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer -DTSAN_ENABLED -U_FORTIFY_SOURCE"
	CFLAGS="${CFLAGS} ${TSAN_CFLAGS}"
	AC_SUBST(TSAN_CFLAGS)
	TSAN_LDFLAGS="-ltsan"
	AC_SUBST(TSAN_LDFLAGS)
	LDFLAGS="${LDFLAGS} ${TSAN_LDFLAGS}"
fi

AC_ARG_ENABLE(lsan,
[  --enable-lsan      Compile with leak sanitizer],
[case "${enableval}" in
	yes) lsan_enabled=true ;;
	no)  lsan_enabled=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-lsan) ;;
esac],[lsan_enabled=false])

if test "x${enable_lsan}" = "xyes"; then
	LSAN_CFLAGS="-fsanitize=leak -fno-omit-frame-pointer -DLSAN_ENABLED -U_FORTIFY_SOURCE"
	CFLAGS="${CFLAGS} ${LSAN_CFLAGS}"
	AC_SUBST(LSAN_CFLAGS)
	LSAN_LDFLAGS="-llsan"
	AC_SUBST(LSAN_LDFLAGS)
	LDFLAGS="${LDFLAGS} ${LSAN_LDFLAGS}"
fi

AC_ARG_ENABLE(ubsan,
[  --enable-ubsan      Compile with undefined behaviour sanitizer],
[case "${enableval}" in
	yes) ubsan_enabled=true ;;
	no)  ubsan_enabled=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-ubsan) ;;
esac],[ubsan_enabled=false])

if test "x${enable_ubsan}" = "xyes"; then
	UBSAN_CFLAGS="-fsanitize=undefined -fno-omit-frame-pointer -DUBSAN_ENABLED -U_FORTIFY_SOURCE"
	CFLAGS="${CFLAGS} ${UBSAN_CFLAGS}"
	AC_SUBST(UBSAN_CFLAGS)
	UBSAN_LDFLAGS="-lubsan"
	AC_SUBST(UBSAN_LDFLAGS)
	LDFLAGS="${LDFLAGS} ${UBSAN_LDFLAGS}"
fi

AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug], [Enable Debugging])],
    [enable_debug=$enableval], [enable_debug=no])

if test "x$enable_debug}" = "xyes"; then
	CFLAGS="${CFLAGS} -DRCU_MEMBARRIER=1 -Wall -Wextra -Werror -fno-omit-frame-pointer -fasynchronous-unwind-tables"
else
	CFLAGS="${CFLAGS} -DRCU_MEMBARRIER=1 -Wall -Wextra -Werror -g -Og -fno-omit-frame-pointer -fasynchronous-unwind-tables"
fi

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
	lib/fs/Makefile
	lib/nfs3/Makefile
	lib/nfs4/Makefile
	lib/xdr/Makefile
	lib/utils/Makefile
	lib/tests/Makefile])

AC_OUTPUT

