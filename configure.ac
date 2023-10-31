dnl SPDX-License-Identifier: GPL-2.0+
dnl Copyright (C) 2023 Tom Haynes <loghyr@gmail.com>

AC_PREREQ([2.69])
AC_INIT([reffs], [0.1], [loghyr@gmail.com])
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
        [AC_MSG_ERROR([Could not configure Userspace RCU support.])])

PKG_CHECK_MODULES(URCU_CDS, liburcu-cds, ,
        [AC_MSG_ERROR([Could not configure Userspace RCU CDS support.])])

AX_PTHREAD([], [AC_MSG_ERROR([Could not configure pthread support])])
AC_CHECK_LIB([pthread], [pthread_create], , [AC_MSG_ERROR([Please install libpthread])])

LIBS="$URCU_LIBS $URCU_CDS_LIBS $JEMALLOC_LIBS $LIBS"

AC_CHECK_HEADERS([stdlib.h sys/socket.h], , [AC_MSG_ERROR([Unable to find usable header file])])

AC_CONFIG_FILES([
	Makefile
	lib/Makefile
	lib/include/Makefile
	lib/xdr/Makefile
	lib/tests/Makefile])

AC_OUTPUT

