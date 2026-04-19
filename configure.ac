dnl SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
dnl SPDX-License-Identifier: AGPL-3.0-or-later

AC_INIT([reffs], [0.1.0], [loghyr@gmail.com])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign])

AC_CONFIG_SRCDIR([src/reffsd.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

AM_SILENT_RULES([yes])

# Set Clang as the default compiler
CC=clang

# Allow users to specify a compiler with --with-cc
AC_ARG_WITH([cc],
    [AS_HELP_STRING([--with-cc=COMPILER], [Specify the C compiler to use (clang or gcc)])],
    [CC="$withval"],
    [])

# Check if the specified compiler exists
AC_CHECK_PROG(CC_CHECK, $CC, yes, no)
if test "$CC_CHECK" = "no"; then
    AC_MSG_ERROR([Compiler '$CC' not found.])
fi

# Display the compiler that will be used
AC_MSG_RESULT([Using C compiler: $CC])

AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# FreeBSD installs headers and libraries under /usr/local by default
# and the base toolchain does not search there unless told to.  Add
# the standard pkg path so subsequent AC_CHECK_HEADER / AC_CHECK_LIB
# probes can find ports-installed dependencies (libuuid,
# HdrHistogram_c, etc.).
AC_CANONICAL_HOST
host_is_freebsd=no
AS_CASE([$host_os],
    [freebsd*|dragonfly*], [
        CPPFLAGS="-I/usr/local/include $CPPFLAGS"
        LDFLAGS="-L/usr/local/lib $LDFLAGS"
        # Force-include <rpc/types.h> on every compilation unit so
        # generated XDR headers (which #include <rpc/xdr.h> directly)
        # get bool_t in scope.  Linux's libtirpc pulls types.h in from
        # xdr.h itself; FreeBSD's base libc xdr.h does not.
        CPPFLAGS="-include rpc/rpc.h $CPPFLAGS"
        # EREMOTEIO is a glibc-specific errno.  FreeBSD has no
        # distinct "remote I/O error" code; alias to EIO so all
        # -EREMOTEIO return statements compile.
        CPPFLAGS="-DEREMOTEIO=EIO $CPPFLAGS"
        # ENODATA is also glibc-only ("no data available"); FreeBSD
        # has ENOATTR in the same role (especially for xattrs).
        CPPFLAGS="-DENODATA=ENOATTR $CPPFLAGS"
        # backtrace(3) / backtrace_symbols(3) are in glibc on Linux
        # but in a separate libexecinfo on FreeBSD.  Link it
        # globally so every binary gets the symbols.
        LIBS="-lexecinfo $LIBS"
        host_is_freebsd=yes
    ])
AM_CONDITIONAL([HOST_FREEBSD], [test "x$host_is_freebsd" = "xyes"])

AC_MSG_CHECKING([for C11 support])
dnl Strip out the freaking -O2 by default!
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],[])],
    [CFLAGS="-std=c11"; AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no]); HAVE_C11=no])

AC_SYS_LARGEFILE
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
LT_INIT

AM_PATH_PYTHON([3.6],, [:])

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

# SunRPC lives in glibc's legacy tree on Linux (deprecated since
# glibc 2.26) and is typically replaced by libtirpc.  FreeBSD ships
# SunRPC in base libc, with identical <rpc/*.h> include paths and no
# separate library.  Try libtirpc first; on failure, fall back to
# empty CFLAGS/LIBS which makes the RPC calls resolve against base
# libc.  Explicit detection of FreeBSD via host_os lets us skip the
# misleading "Could not configure libtirpc" error there.
AS_CASE([$host_os],
    [freebsd*|dragonfly*], [
        TIRPC_CFLAGS=""
        TIRPC_LIBS=""
        AC_SUBST([TIRPC_CFLAGS])
        AC_SUBST([TIRPC_LIBS])
    ],
    [
        PKG_CHECK_MODULES(TIRPC, libtirpc, ,
            [AC_MSG_ERROR([Could not configure libtirpc support.])])
    ])

PKG_CHECK_MODULES([CHECK], [check >= 0.15.2])

AX_PTHREAD([PTHREAD_CFLAGS=-pthread], [AC_MSG_ERROR([Could not configure pthread support])])
AC_SUBST(PTHREAD_CFLAGS)

PKG_CHECK_MODULES(XXHASH, libxxhash, , [AC_MSG_ERROR([Could not configure xxhash support.])])

# libfuse pkg-config module name differs by platform: "fuse" on
# Linux (libfuse2/libfuse3 via the fuse or fuse3 module) and "fuse3"
# on FreeBSD (fusefs-libs3).  Try both.
PKG_CHECK_MODULES(FUSE, fuse3, , [
    PKG_CHECK_MODULES(FUSE, fuse, , [
        AC_MSG_ERROR([Could not configure fuse support (tried fuse3, fuse).])
    ])
])

# Check for programs
AC_PATH_PROG([GIT], [git], [no])
if test "x$GIT" = "xno"; then
    AC_MSG_ERROR([git is required to build this project])
fi

AC_PATH_PROG([CLANG_FORMAT], [clang-format], [no])
if test "x$CLANG_FORMAT" = "xno"; then
    AC_MSG_WARN([clang-format not found - 'make style' will fail])
fi

AC_PATH_PROG([CLANG_TIDY], [clang-tidy], [no])
if test "x$CLANG_TIDY" = "xno"; then
    AC_MSG_WARN([clang-tidy not found])
fi

AC_PATH_PROG([SCAN_BUILD], [scan-build], [no])
if test "x$SCAN_BUILD" = "xno"; then
    AC_MSG_WARN([scan-build not found - 'make scanbuild' will fail])
fi

AC_PATH_PROG([XDR_PARSER], [xdr-parser], [no])
if test "x$XDR_PARSER" = "xno"; then
    AC_MSG_ERROR([xdr-parser is required (pip install reply-xdr)])
fi

AC_PATH_PROG([BEAR], [bear], [no])
if test "x$BEAR" = "xno"; then
    AC_MSG_WARN([bear not found - 'make tidy' may fail])
fi

# Check for libuuid
AC_CHECK_HEADER([uuid/uuid.h],
  [AC_CHECK_LIB([uuid], [uuid_generate],
    [UUID_LIBS="-luuid"
     AC_DEFINE([HAVE_LIBUUID], [1], [Define if you have libuuid])],
    [AC_MSG_ERROR([libuuid library not found])])],
  [AC_MSG_ERROR([uuid.h header not found])])
AC_SUBST([UUID_LIBS])

# Optional: check for uuid_generate_time_safe which is available in newer versions
AC_CHECK_LIB([uuid], [uuid_generate_time_safe],
  [AC_DEFINE([HAVE_UUID_GENERATE_TIME_SAFE], [1],
             [Define if libuuid has uuid_generate_time_safe function])])

AC_CHECK_HEADER([zlib.h], [], [AC_MSG_ERROR([zlib.h not found])])
AC_CHECK_LIB([z], [crc32], [], [AC_MSG_ERROR([zlib library not found])])

AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
if test "x$PKG_CONFIG" = "xno"; then
  AC_MSG_ERROR([pkg-config not found])
fi

PKG_CHECK_MODULES([ZSTD], [libzstd >= 1.4.0],
  [AC_DEFINE([HAVE_LIBZSTD], [1], [Define if libzstd is available])],
  [AC_MSG_ERROR([libzstd >= 1.4.0 is required])]
)

AC_SUBST([ZSTD_CFLAGS])
AC_SUBST([ZSTD_LIBS])

# Check for io_uring support
AC_ARG_ENABLE([linux-io_uring],
    [AS_HELP_STRING([--enable-linux-io_uring], [enable Linux io_uring support @<:@default=auto@:>@])],
    [],
    [enable_linux_io_uring=auto])

# Check for strict POSIX support
AC_ARG_ENABLE([strict-posix],
    [AS_HELP_STRING([--enable-strict-posix], [enable strict POSIX permissions (breaks git over NFS) @<:@default=no@:>@])],
    [],
    [enable_strict_posix=no])

AS_IF([test "x$enable_strict_posix" = "xyes"], [
    AC_DEFINE([HAVE_STRICT_POSIX], [1], [Define if strict POSIX permissions are enabled])
])

AS_IF([test "x$enable_linux_io_uring" != "xno"], [
    # Check for liburing >= 2.3 (required for IORING_ASYNC_CANCEL_ALL, etc.)
    PKG_CHECK_MODULES([URING], [liburing >= 2.3], [
        AC_DEFINE([HAVE_LINUX_IO_URING], [1], [Define if Linux io_uring is available])
        URING_LIBS="$URING_LIBS"
    ], [
        # Fallback to manual check if pkg-config fails or version is old
        AC_CHECK_HEADERS([liburing.h], [
            AC_CHECK_LIB([uring], [io_uring_queue_init], [
                AC_MSG_WARN([liburing found but version check failed - build might fail if < 2.3])
                AC_DEFINE([HAVE_LINUX_IO_URING], [1], [Define if Linux io_uring is available])
                URING_LIBS="-luring"
            ], [
                if test "x$enable_linux_io_uring" = "xyes"; then
                    AC_MSG_ERROR([io_uring support explicitly requested but liburing not found])
                fi
            ])
        ], [
            if test "x$enable_linux_io_uring" = "xyes"; then
                AC_MSG_ERROR([io_uring support explicitly requested but liburing.h not found])
            fi
        ])
    ])
])
AC_SUBST([URING_LIBS])

# Check for jemalloc
AC_ARG_WITH([jemalloc],
  [AS_HELP_STRING([--with-jemalloc], [use jemalloc for memory allocation])],
  [], [with_jemalloc=check])

AS_IF([test "x$with_jemalloc" != xno], [
  AC_CHECK_LIB([jemalloc], [malloc], [
    AC_DEFINE([HAVE_JEMALLOC], [1], [Define if jemalloc is available])
    LIBS="-ljemalloc $LIBS"
    have_jemalloc=yes
  ], [
    if test "x$with_jemalloc" != xcheck; then
      AC_MSG_FAILURE([--with-jemalloc was given, but test for jemalloc failed])
    fi
    have_jemalloc=no
  ])
], [have_jemalloc=no])

AM_CONDITIONAL([HAVE_JEMALLOC], [test "x$have_jemalloc" = xyes])

# Check if building for VM environment (enables memory optimizations)
AC_ARG_ENABLE([vm],
    [AS_HELP_STRING([--enable-vm],
        [Enable VM memory optimizations (e.g. jemalloc arena purging)])],
    [AS_IF([test "x$enableval" = "xyes"],
        [AC_DEFINE([HAVE_VM], [1], [Define if running in a VM environment])])],
    [])

# Check if testing for io_uring stress
AC_ARG_ENABLE([io-uring-stress],
    [AS_HELP_STRING([--enable-io-uring-stress],
        [Enable io_uring stress testing ])],
    [AS_IF([test "x$enableval" = "xyes"],
        [AC_DEFINE([HAVE_IO_URING_STRESS], [1], [Define if stress testing io_uring])])],
    [])

# Check if we should enable verbose debugging output
AC_ARG_ENABLE([verbose-debug],
    [AS_HELP_STRING([--enable-verbose-debug],
        [Enable verbose debugging output (high volume status dumps)])],
    [AS_IF([test "x$enableval" = "xyes"],
        [AC_DEFINE([VERBOSE_DEBUG], [1], [Define if verbose debugging is enabled])])],
    [])

# Check if we should use the destroyed context cache
AC_ARG_ENABLE([destroy-cache],
    [AS_HELP_STRING([--enable-destroy-cache],
        [Enable deferred destruction of IO contexts to avoid UAF])],
    [AS_IF([test "x$enableval" = "xyes"],
        [AC_DEFINE([HAVE_DESTROY_CACHE], [1], [Define if using destroyed context cache])])],
    [])

# Check if we should enable all trace categories by default
AC_ARG_ENABLE([all-trace-categories],
    [AS_HELP_STRING([--enable-all-trace-categories],
        [Enable all trace categories by default @<:@default=yes@:>@])],
    [enable_all_trace_categories=$enableval],
    [enable_all_trace_categories=yes])

AS_IF([test "x$enable_all_trace_categories" = "xyes"],
    [AC_DEFINE([ENABLE_ALL_TRACE_CATEGORIES], [1], [Define if all trace categories should be enabled by default])])

AC_ARG_ENABLE(asan,
[  --enable-asan      Compile with address sanitizer],
[case "${enableval}" in
        yes) asan_enabled=true ;; 
        no)  asan_enabled=false ;;
        *) AC_MSG_ERROR(bad value ${enableval} for --enable-asan) ;;
esac],[asan_enabled=false])
if test "x${enable_asan}" = "xyes"; then
        # Check if we're using Clang
        AC_CHECK_PROG(IS_CLANG, clang, yes, no)
        if test "x${IS_CLANG}" = "xyes"; then
                # For Clang, use the same flag for both compiler and linker
                ASAN_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -DASAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${ASAN_CFLAGS}"
                AC_SUBST(ASAN_CFLAGS)
                ASAN_LDFLAGS="-fsanitize=address"
        else
                # GCC needs explicit linking to libasan
                ASAN_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -DASAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${ASAN_CFLAGS}"
                AC_SUBST(ASAN_CFLAGS)
                ASAN_LDFLAGS="-lasan"
        fi
        AC_SUBST(ASAN_LDFLAGS)
        LDFLAGS="${LDFLAGS} ${ASAN_LDFLAGS}"
        ASAN_ENABLED=1
else
        ASAN_ENABLED=0
fi

AC_ARG_ENABLE(tsan,
[  --enable-tsan      Compile with thread sanitizer],
[case "${enableval}" in
        yes) tsan_enabled=true ;;
        no)  tsan_enabled=false ;;
        *) AC_MSG_ERROR(bad value ${enableval} for --enable-tsan) ;;
esac],[tsan_enabled=false])
if test "x${enable_tsan}" = "xyes"; then
        # Check if we're using Clang
        AC_CHECK_PROG(IS_CLANG, clang, yes, no)
        if test "x${IS_CLANG}" = "xyes"; then
                # For Clang, use the same flag for both compiler and linker
                TSAN_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer -DTSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${TSAN_CFLAGS}"
                AC_SUBST(TSAN_CFLAGS)
                TSAN_LDFLAGS="-fsanitize=thread"
        else
                # GCC needs explicit linking to libtsan
                TSAN_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer -DTSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${TSAN_CFLAGS}"
                AC_SUBST(TSAN_CFLAGS)
                TSAN_LDFLAGS="-ltsan"
        fi
        AC_SUBST(TSAN_LDFLAGS)
        LDFLAGS="${LDFLAGS} ${TSAN_LDFLAGS}"
        TSAN_ENABLED=1
else
        TSAN_ENABLED=0
fi

AC_ARG_ENABLE(lsan,
[  --enable-lsan      Compile with leak sanitizer],
[case "${enableval}" in
        yes) lsan_enabled=true ;;
        no)  lsan_enabled=false ;;
        *) AC_MSG_ERROR(bad value ${enableval} for --enable-lsan) ;;
esac],[lsan_enabled=false])
if test "x${enable_lsan}" = "xyes"; then
        # Check if we're using Clang
        AC_CHECK_PROG(IS_CLANG, clang, yes, no)
        if test "x${IS_CLANG}" = "xyes"; then
                # For Clang, use the same flag for both compiler and linker
                LSAN_CFLAGS="-fsanitize=leak -fno-omit-frame-pointer -DLSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${LSAN_CFLAGS}"
                AC_SUBST(LSAN_CFLAGS)
                LSAN_LDFLAGS="-fsanitize=leak"
        else
                # GCC needs explicit linking to liblsan
                LSAN_CFLAGS="-fsanitize=leak -fno-omit-frame-pointer -DLSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${LSAN_CFLAGS}"
                AC_SUBST(LSAN_CFLAGS)
                LSAN_LDFLAGS="-llsan"
        fi
        AC_SUBST(LSAN_LDFLAGS)
        LDFLAGS="${LDFLAGS} ${LSAN_LDFLAGS}"
        LSAN_ENABLED=1
else
        LSAN_ENABLED=0
fi

AC_ARG_ENABLE(ubsan,
[  --enable-ubsan      Compile with undefined behaviour sanitizer],
[case "${enableval}" in
        yes) ubsan_enabled=true ;;
        no)  ubsan_enabled=false ;;
        *) AC_MSG_ERROR(bad value ${enableval} for --enable-ubsan) ;;
esac],[ubsan_enabled=false])
if test "x${enable_ubsan}" = "xyes"; then
        # Check if we're using Clang
        AC_CHECK_PROG(IS_CLANG, clang, yes, no)
        if test "x${IS_CLANG}" = "xyes"; then
                # For Clang, use the same flag for both compiler and linker
                UBSAN_CFLAGS="-fsanitize=undefined -fno-omit-frame-pointer -DUBSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${UBSAN_CFLAGS}"
                AC_SUBST(UBSAN_CFLAGS)
                UBSAN_LDFLAGS="-fsanitize=undefined"
        else
                # GCC needs explicit linking to libubsan
                UBSAN_CFLAGS="-fsanitize=undefined -fno-omit-frame-pointer -DUBSAN_ENABLED -U_FORTIFY_SOURCE"
                CFLAGS="${CFLAGS} ${UBSAN_CFLAGS}"
                AC_SUBST(UBSAN_CFLAGS)
                UBSAN_LDFLAGS="-lubsan"
        fi
        AC_SUBST(UBSAN_LDFLAGS)
        LDFLAGS="${LDFLAGS} ${UBSAN_LDFLAGS}"
        UBSAN_ENABLED=1
else
        UBSAN_ENABLED=0
fi

# Check for HdrHistogram_c
# HdrHistogram_c is packaged on most Linux distros (HdrHistogram_c-devel)
# but not in FreeBSD ports.  Treat as optional: define HAVE_HDR_HISTOGRAM
# when present; source files gate histogram code with #ifdef.  If
# missing on Linux, emit a warning -- histograms are soft telemetry.
AC_CHECK_HEADERS([hdr/hdr_histogram.h], [
    AC_DEFINE([HAVE_HDR_HISTOGRAM], [1],
              [Define if HdrHistogram_c is available])
], [
    AC_MSG_WARN([hdr_histogram.h not found; histogram telemetry will be disabled.  On Linux, install HdrHistogram_c-devel.])
])
AS_IF([test "x$ac_cv_header_hdr_hdr_histogram_h" = "xyes"], [
    AC_CHECK_LIB([hdr_histogram], [hdr_init], [
        HDR_HISTOGRAM_LIBS="-lhdr_histogram"
    ], [
        AC_MSG_WARN([libhdr_histogram library not found; histogram telemetry will be disabled.])
        ac_cv_header_hdr_hdr_histogram_h=no
    ])
])
AC_SUBST([HDR_HISTOGRAM_LIBS])

# Check for OpenSSL
PKG_CHECK_MODULES([OPENSSL], [openssl >= 1.0.2], [],
    [AC_MSG_ERROR([OpenSSL 1.0.2 or higher required])])

# Add OpenSSL flags to CFLAGS and LDFLAGS
AC_SUBST([OPENSSL_CFLAGS])
AC_SUBST([OPENSSL_LIBS])

# Check for libgssapi_krb5 (optional — RPCSEC_GSS disabled without it)
# <rpc/auth_gss.h> is part of libtirpc on Linux (the SunRPC/GSS
# integration header).  FreeBSD has its own GSS-RPC in the
# gssrpc/ namespace with a different API and does not provide
# <rpc/auth_gss.h>.  Check separately so mds_session.c can gate
# its libtirpc-specific block on this, not on HAVE_GSSAPI_KRB5
# (which only says GSSAPI is available, not that the SunRPC
# integration glue exists).
AC_CHECK_HEADERS([rpc/auth_gss.h])

PKG_CHECK_MODULES([GSSAPI], [krb5-gssapi], [
    AC_DEFINE([HAVE_GSSAPI_KRB5], [1], [Define if libgssapi_krb5 is available])
], [
    AC_MSG_WARN([libgssapi_krb5 not found — RPCSEC_GSS/krb5 disabled])
])
AC_SUBST([GSSAPI_CFLAGS])
AC_SUBST([GSSAPI_LIBS])

# Check for libnfsidmap (optional — NFSv4 principal→uid mapping)
PKG_CHECK_MODULES([NFSIDMAP], [libnfsidmap], [
    AC_DEFINE([HAVE_LIBNFSIDMAP], [1], [Define if libnfsidmap is available])
], [
    AC_MSG_WARN([libnfsidmap not found — GSS principal mapping uses getpwnam fallback])
])
AC_SUBST([NFSIDMAP_CFLAGS])
AC_SUBST([NFSIDMAP_LIBS])

# Check for RocksDB (optional — RocksDB metadata backend)
PKG_CHECK_MODULES([ROCKSDB], [rocksdb], [
    AC_DEFINE([HAVE_ROCKSDB], [1], [Define if RocksDB is available])
    have_rocksdb=yes
], [
    AC_MSG_WARN([RocksDB not found — RocksDB metadata backend disabled])
    have_rocksdb=no
])
# FreeBSD's rocksdb.pc ships a broken LDFLAGS fragment
# ("-Wl,-rpath -Wl,") which the linker cannot parse -- it reads
# the empty argument as a library name and fails with "cannot
# open libbackends.so.0".  Strip the fragment; the -L/usr/local/lib
# that pkg-config emits alongside makes the library findable.
ROCKSDB_LIBS=`echo "$ROCKSDB_LIBS" | sed -e 's/-Wl,-rpath[[ 	]]*-Wl,[[ 	]]*//g'`
AC_SUBST([ROCKSDB_CFLAGS])
AC_SUBST([ROCKSDB_LIBS])
AM_CONDITIONAL([HAVE_ROCKSDB], [test "x$have_rocksdb" = "xyes"])

AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug], [Enable Debugging])],
    [enable_debug=$enableval], [enable_debug=no])

AC_ARG_ENABLE([noopt],
    [AS_HELP_STRING([--enable-noopt], [Enable no optimizations])],
    [enable_noopt=$enableval], [enable_noopt=no])

COMMON_FLAGS="${CFLAGS} -D_FILE_OFFSET_BITS=64 -DRCU_MEMBARRIER=1 -Wall -Wextra -Wformat=2 -Werror -fno-omit-frame-pointer -fasynchronous-unwind-tables"
if test "x${enable_config_block}" = "xyes"; then
if test "x${enable_debug}" = "xyes"; then
	DEBUG_ENABLED=1
if test "x${enable_noopt}" = "xyes"; then
	CFLAGS="${COMMON_FLAGS} -O0 -DNDEBUG"
else
	CFLAGS="${COMMON_FLAGS} -O2 -DNDEBUG"
fi
else
	DEBUG_ENABLED=0
if test "x${enable_noopt}" = "xyes"; then
	CFLAGS="${COMMON_FLAGS} -g -O0"
else
	CFLAGS="${COMMON_FLAGS} -g -Og"
fi
fi

dnl Compiler specifc flags
if test "x${CC}" = "xclang"; then
	AC_MSG_RESULT([No clang flags yet])
else
	CFLAGS="${CFLAGS} -Wall -Wextra -Wpedantic"
fi
else
	CFLAGS="${COMMON_FLAGS} -Wall -Wextra -Wpedantic -g -O0"
fi

AS_CASE([$host_os],
    [freebsd*|dragonfly*], [
        # FreeBSD's liburcu headers use two C extensions that
        # -Wpedantic flags under -std=c11:
        #   - named variadic macros (GCC extension)
        #   - zero-argument variadic macro invocations (C23, used
        #     by liburcu's _uatomic_default_mo pattern)
        # Suppress both warning classes rather than drop -Wpedantic
        # entirely.
        CFLAGS="${CFLAGS} -Wno-variadic-macros -Wno-c23-extensions -Wno-gnu"
    ])

AC_MSG_RESULT([C flags are ${CFLAGS}])

AC_SUBST([DEBUG_ENABLED])
AC_SUBST([ASAN_ENABLED])
AC_SUBST([LSAN_ENABLED])
AC_SUBST([TSAN_ENABLED])
AC_SUBST([UBSAN_ENABLED])

AC_CHECK_HEADERS([stdlib.h sys/socket.h], , [AC_MSG_ERROR([Unable to find usable header file])])

dnl --- AVX2 support for Mojette SIMD fast path ---
AVX2_CFLAGS=""
AC_MSG_CHECKING([whether $CC supports -mavx2])
saved_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -mavx2"
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM(
    [[#include <immintrin.h>]],
    [[__m256i a = _mm256_setzero_si256(); (void)a;]])],
  [AC_MSG_RESULT([yes])
   AVX2_CFLAGS="-mavx2"],
  [AC_MSG_RESULT([no])])
CFLAGS="$saved_CFLAGS"
AC_SUBST([AVX2_CFLAGS])

dnl Fully expand prefix-relative paths for the systemd unit file.
dnl AC_CONFIG_FILES only does one level of @var@ substitution, so
dnl @bindir@ expands to '${exec_prefix}/bin' which systemd rejects.
AS_IF([test "x$prefix" = xNONE], [prefix=$ac_default_prefix])
AS_IF([test "x$exec_prefix" = xNONE], [exec_prefix=$prefix])
eval "FULL_BINDIR=$bindir"
eval "FULL_SYSCONFDIR=$sysconfdir"
AC_SUBST([FULL_BINDIR])
AC_SUBST([FULL_SYSCONFDIR])

AC_CONFIG_FILES([
	Makefile
	reffs.pc
	reffs.spec
	reffsd.service
	LICENSES/Makefile
	third_party/Makefile
	third_party/tomlc99/Makefile
	lib/Makefile
	lib/config/Makefile
	lib/config/tests/Makefile
	lib/backends/Makefile
	lib/ec/Makefile
	lib/ec/tests/Makefile
	lib/nfs4/common/Makefile
	lib/nfs4/server/Makefile
	lib/nfs4/client/Makefile
	lib/nfs4/client/tests/Makefile
	lib/nfs4/dstore/Makefile
	lib/nfs4/dstore/tests/Makefile
	lib/fs/Makefile
	lib/fs/tests/Makefile
	lib/nlm/Makefile
	lib/nlm/tests/Makefile
	lib/nsm/Makefile
	lib/include/Makefile
	lib/include/reffs/Makefile
	lib/include/reffs/trace/Makefile
	lib/io/Makefile
	lib/io/tests/Makefile
	lib/mount3/Makefile
	lib/mount3/tests/Makefile
	lib/nfs3/Makefile
	lib/nfs3/tests/Makefile
	lib/nfs4/Makefile
	lib/nfs4/include/Makefile
	lib/nfs4/include/nfs4/Makefile
	lib/nfs4/include/nfs4/trace/Makefile
	lib/nfs4/tests/Makefile
	lib/rpc/Makefile
	lib/rpc/tests/Makefile
	lib/tls/Makefile
	lib/probe1/Makefile
	lib/tests/Makefile
	lib/trace/Makefile
	lib/utils/Makefile
	lib/utils/tests/Makefile
	lib/xdr/Makefile
	scripts/Makefile
	scripts/reffs/Makefile
	src/Makefile
	tools/Makefile])

AC_CONFIG_FILES([scripts/copy_and_verify.sh],[chmod +x scripts/copy_and_verify.sh])
AC_CONFIG_FILES([scripts/reffs-probe.py],[chmod +x scripts/reffs-probe.py])
AC_CONFIG_FILES([scripts/reffs/probe_client.py],[chmod +x scripts/reffs/probe_client.py])
AC_CONFIG_FILES([scripts/reffs/protocol_client.py],[chmod +x scripts/reffs/protocol_client.py])
AC_CONFIG_FILES([scripts/reffs/__init__.py])
AC_CONFIG_FILES([scripts/__init__.py])

AC_OUTPUT
