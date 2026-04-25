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

dnl AC_PROG_CC below handles compiler discovery and errors out with
dnl "no acceptable C compiler found" if CC is set to a non-existent
dnl program.  No need for a custom AC_CHECK_PROG gate here -- it
dnl tripped autoconf 2.73's strict macro-expansion rules on macOS.
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Per-host adjustments for BSDs and Darwin (macOS).  FreeBSD installs
# ports under /usr/local; Darwin installs Homebrew under
# /opt/homebrew (Apple Silicon) or /usr/local (Intel).  Neither is on
# the base compiler's default search path.
AC_CANONICAL_HOST
host_is_freebsd=no
host_is_darwin=no
AS_CASE([$host_os],
    [freebsd*|dragonfly*], [
        CPPFLAGS="-I/usr/local/include $CPPFLAGS"
        LDFLAGS="-L/usr/local/lib $LDFLAGS"
        # Force-include <rpc/types.h> on every compilation unit so
        # generated XDR headers (which #include <rpc/xdr.h> directly)
        # get bool_t in scope.  Linux's libtirpc pulls types.h in from
        # xdr.h itself; FreeBSD's base libc xdr.h does not.
        CPPFLAGS="-include rpc/rpc.h $CPPFLAGS"
        # EREMOTEIO and ENODATA are glibc-specific.  FreeBSD's
        # ELAST is 97 (EINTEGRITY), so we assign reffs-internal
        # values safely above that range — distinct from every
        # POSIX/FreeBSD errno — so internal comparisons like
        # `if (ret == -EREMOTEIO)` continue to work.  These values
        # must never leave reffs: coerce to EIO / ENOATTR at any
        # boundary (syscall return, NFS wire).  Must match the
        # fallback in lib/include/reffs/log.h.
        CPPFLAGS="-DEREMOTEIO=198 $CPPFLAGS"
        CPPFLAGS="-DENODATA=199   $CPPFLAGS"
        # backtrace(3) / backtrace_symbols(3) are in glibc on Linux
        # but in a separate libexecinfo on FreeBSD.  Link it
        # globally so every binary gets the symbols.
        LIBS="-lexecinfo $LIBS"
        host_is_freebsd=yes
    ],
    [darwin*], [
        # Homebrew paths.  Apple Silicon defaults to /opt/homebrew;
        # Intel to /usr/local.  Probe at configure time so a user on
        # an Intel Mac without /opt/homebrew does not get a broken
        # -I/opt/homebrew flag (and vice versa).
        AS_IF([test -d /opt/homebrew/include],
              [CPPFLAGS="-I/opt/homebrew/include $CPPFLAGS"])
        AS_IF([test -d /opt/homebrew/lib],
              [LDFLAGS="-L/opt/homebrew/lib $LDFLAGS"
               LDFLAGS="-Wl,-rpath,/opt/homebrew/lib $LDFLAGS"])
        AS_IF([test -d /usr/local/include],
              [CPPFLAGS="-I/usr/local/include $CPPFLAGS"])
        AS_IF([test -d /usr/local/lib],
              [LDFLAGS="-L/usr/local/lib $LDFLAGS"
               LDFLAGS="-Wl,-rpath,/usr/local/lib $LDFLAGS"])
        # Runtime dylib loading on macOS: libtool's uninstalled
        # binary rpath points at build-tree .libs/ dirs, which
        # don't contain Homebrew-installed dylibs
        # (libhdr_histogram, liburcu, etc.).  Adding -rpath to
        # /opt/homebrew/lib (and /usr/local/lib on Intel) bakes the
        # Homebrew prefix into each binary so `make check` finds
        # the dylibs without needing DYLD_LIBRARY_PATH in the
        # harness.
        # Reject dylibs with unresolved symbols at link time, matching
        # Linux's -Wl,--no-undefined default.  This used to be
        # -Wl,-undefined,dynamic_lookup to defer resolution until
        # runtime because libreffs_dstore was a noinst_LTLIBRARIES
        # static convenience archive: consumer dylibs referenced its
        # dstore_ops_* globals via LIBADD folds that worked at
        # runtime but not at strict link time.  Commit dee4a109
        # promoted libreffs_dstore to a proper shared library (to
        # fix an ASAN ODR violation on those same globals), which
        # also closed the last strict-link gap -- switch to
        # -Wl,-undefined,error so future missing LIBADDs are caught
        # at build time instead of as obscure dyld failures.  See #48.
        LDFLAGS="-Wl,-undefined,error $LDFLAGS"
        # Same EREMOTEIO/ENODATA story as FreeBSD: Darwin errno.h
        # has neither.  Pick reffs-internal sentinel values distinct
        # from every POSIX/Darwin errno.  Darwin ELAST is 106
        # (EQFULL), so 198/199 are safely above.  Must match the
        # fallback in lib/include/reffs/log.h.
        CPPFLAGS="-DEREMOTEIO=198 $CPPFLAGS"
        CPPFLAGS="-DENODATA=199   $CPPFLAGS"
        # Force-include <rpc/rpc.h> so base-libc SunRPC pulls in
        # <rpc/types.h> (bool_t) and <rpc/xdr.h> (XDR macros) for
        # every compilation unit.  Generated XDR headers include
        # <rpc/xdr.h> directly; Darwin's <rpc/xdr.h> does not
        # transitively include <rpc/types.h>, same issue as FreeBSD.
        CPPFLAGS="-include rpc/rpc.h $CPPFLAGS"
        # Six SunRPC identifier aliases used to live here as -D
        # macros (xdr_u?int{32,64}_t, RPCSEC_GSS, AUTH_SYS,
        # AUTH_DH, authsys_create).  Moved into
        # lib/include/reffs/darwin_rpc_compat.h as #ifdef __APPLE__
        # stanzas; that header is force-included here so every TU,
        # including generated XDR, sees the aliases.  Use an
        # absolute path so the `-include` works from any Makefile
        # subdir (cc's CWD is the subdir, not the build root).  See #24.
        abs_top_srcdir=`cd "$srcdir" && pwd`
        CPPFLAGS="-include $abs_top_srcdir/lib/include/reffs/darwin_rpc_compat.h $CPPFLAGS"
        # Two Darwin-specific aliases previously lived in CPPFLAGS
        # here:
        #   -Dst_{atim,mtim,ctim}=st_{a,m,c}timespec
        #   -DUUID_STR_LEN=37
        # They now have source-visible #ifdef __APPLE__ stanzas in
        # lib/include/reffs/fs.h and lib/fs/ns.c respectively, so
        # the CPPFLAGS magic is no longer needed.  See #24.
        host_is_darwin=yes
    ])
AM_CONDITIONAL([HOST_FREEBSD], [test "x$host_is_freebsd" = "xyes"])
AM_CONDITIONAL([HOST_DARWIN],  [test "x$host_is_darwin"  = "xyes"])

# POSIX extensions accept4(2) and pipe2(2) are Linux and FreeBSD
# extensions; Darwin has only accept(2) / pipe(2).  Use AC_CHECK_DECLS
# (header-only probe) instead of AC_CHECK_FUNCS (link-test probe) --
# the Darwin LDFLAGS addition of -Wl,-undefined,error above
# would cause a link test to SUCCEED even for symbols that don't
# exist, so AC_CHECK_FUNCS gives the wrong answer on Darwin.
# AC_CHECK_DECLS defines HAVE_DECL_ACCEPT4 / HAVE_DECL_PIPE2 as 0/1;
# posix_shims.h is gated on the HAVE_ACCEPT4 / HAVE_PIPE2 convention,
# so alias via AC_DEFINE when the decls exist.
AC_CHECK_DECLS([accept4], [AC_DEFINE([HAVE_ACCEPT4], [1],
    [Define to 1 if you have the accept4 function])],
    [], [[#include <sys/socket.h>]])
AC_CHECK_DECLS([pipe2], [AC_DEFINE([HAVE_PIPE2], [1],
    [Define to 1 if you have the pipe2 function])],
    [], [[#include <unistd.h>
#include <fcntl.h>]])

# fdatasync(2) is POSIX on Linux/FreeBSD but absent on Darwin (which
# offers fcntl(F_FULLFSYNC) as the moral equivalent).  Header-probe
# rather than link-test for the same reason as accept4 / pipe2 above.
AC_CHECK_DECLS([fdatasync], [], [], [[#include <unistd.h>]])

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

dnl 3-arg form -- pkg-config emits a clear default error on failure.
dnl 4-arg form with a custom error-action trips autoconf 2.73 strict
dnl mode on macOS (the error-action argument is left overquoted and
dnl its macro name survives to the output unexpanded).
PKG_CHECK_MODULES([URCU], [liburcu])
PKG_CHECK_MODULES([URCU_CDS], [liburcu-cds])

# SunRPC lives in glibc's legacy tree on Linux (deprecated since
# glibc 2.26) and is typically replaced by libtirpc.  FreeBSD and
# Darwin (macOS) ship SunRPC in base libc, with identical <rpc/*.h>
# include paths and no separate library.  Skip the libtirpc probe
# on those hosts and fall back to empty CFLAGS/LIBS so the RPC
# calls resolve against base libc.
AS_CASE([$host_os],
    [freebsd*|dragonfly*|darwin*], [
        TIRPC_CFLAGS=""
        TIRPC_LIBS=""
        AC_SUBST([TIRPC_CFLAGS])
        AC_SUBST([TIRPC_LIBS])
    ],
    [
        PKG_CHECK_MODULES([TIRPC], [libtirpc])
    ])

PKG_CHECK_MODULES([CHECK], [check >= 0.15.2])

dnl Split the ACTION-IF-NOT-FOUND into its own AS_IF -- the 2nd arg
dnl of AX_PTHREAD is stored unexpanded and trips autoconf 2.73's
dnl strict mode (same pattern as PKG_CHECK_MODULES 4th arg).
have_pthread=no
AX_PTHREAD([PTHREAD_CFLAGS=-pthread; have_pthread=yes], [have_pthread=no])
AS_IF([test "x$have_pthread" = "xno"],
      [AC_MSG_ERROR([Could not configure pthread support])])
AC_SUBST(PTHREAD_CFLAGS)

PKG_CHECK_MODULES([XXHASH], [libxxhash])

# libfuse pkg-config module name differs by platform: "fuse" on
# Linux (libfuse2/libfuse3 via the fuse or fuse3 module) and "fuse3"
# on FreeBSD (fusefs-libs3).  Try both.  Darwin does not build FUSE
# (lib/fs/Makefile.am gates fuse.c off on HOST_DARWIN), so skip the
# probe entirely there -- also avoids autoconf 2.73's issue with
# PKG_CHECK_MODULES nested fallbacks on macOS.
AS_IF([test "x$host_is_darwin" = "xyes"],
      [FUSE_CFLAGS=""
       FUSE_LIBS=""
       AC_SUBST([FUSE_CFLAGS])
       AC_SUBST([FUSE_LIBS])],
      [PKG_CHECK_MODULES([FUSE], [fuse3], [],
           [PKG_CHECK_MODULES([FUSE], [fuse])])])

# Check for programs.  Use AS_IF (not raw `if test ... then`) so that
# any error/warn macro inside the branch expands in m4 context.
# autoconf 2.73 (macOS Homebrew) enforces this strictly.
AC_PATH_PROG([GIT], [git], [no])
AS_IF([test "x$GIT" = "xno"],
      [AC_MSG_ERROR([git is required to build this project])])

AC_PATH_PROG([CLANG_FORMAT], [clang-format], [no])
AS_IF([test "x$CLANG_FORMAT" = "xno"],
      [AC_MSG_WARN([clang-format not found - 'make style' will fail])])

AC_PATH_PROG([CLANG_TIDY], [clang-tidy], [no])
AS_IF([test "x$CLANG_TIDY" = "xno"],
      [AC_MSG_WARN([clang-tidy not found])])

AC_PATH_PROG([SCAN_BUILD], [scan-build], [no])
AS_IF([test "x$SCAN_BUILD" = "xno"],
      [AC_MSG_WARN([scan-build not found - 'make scanbuild' will fail])])

AC_PATH_PROG([XDR_PARSER], [xdr-parser], [no])
AS_IF([test "x$XDR_PARSER" = "xno"],
      [AC_MSG_ERROR([xdr-parser is required (pip install reply-xdr)])])

AC_PATH_PROG([BEAR], [bear], [no])
AS_IF([test "x$BEAR" = "xno"],
      [AC_MSG_WARN([bear not found - 'make tidy' may fail])])

# Check for libuuid.  Linux and FreeBSD need -luuid; Darwin provides
# uuid_generate* in libSystem (no separate library needed), so the
# AC_CHECK_LIB probe fails there -- skip it on Darwin and leave
# UUID_LIBS empty.  uuid/uuid.h is present on Darwin via
# /usr/include/uuid/uuid.h, so the header probe still runs.
AS_IF([test "x$host_is_darwin" = "xyes"],
    [AC_CHECK_HEADER([uuid/uuid.h], [],
        [AC_MSG_ERROR([uuid.h header not found])])
     UUID_LIBS=""
     AC_DEFINE([HAVE_LIBUUID], [1], [Define if you have libuuid])],
    [AC_CHECK_HEADER([uuid/uuid.h],
        [AC_CHECK_LIB([uuid], [uuid_generate],
            [UUID_LIBS="-luuid"
             AC_DEFINE([HAVE_LIBUUID], [1], [Define if you have libuuid])],
            [AC_MSG_ERROR([libuuid library not found])])],
        [AC_MSG_ERROR([uuid.h header not found])])])
AC_SUBST([UUID_LIBS])

# Optional: check for uuid_generate_time_safe which is available in
# newer libuuid (Linux util-linux).  Darwin's libSystem does not
# provide this variant; skip the probe there.
AS_IF([test "x$host_is_darwin" != "xyes"],
    [AC_CHECK_LIB([uuid], [uuid_generate_time_safe],
        [AC_DEFINE([HAVE_UUID_GENERATE_TIME_SAFE], [1],
                   [Define if libuuid has uuid_generate_time_safe function])])])

AC_CHECK_HEADER([zlib.h], [], [AC_MSG_ERROR([zlib.h not found])])
AC_CHECK_LIB([z], [crc32], [], [AC_MSG_ERROR([zlib library not found])])

AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
AS_IF([test "x$PKG_CONFIG" = "xno"],
      [AC_MSG_ERROR([pkg-config not found])])

dnl 3-arg form: on failure, PKG_CHECK_MODULES emits its default
dnl "Package requirements were not met" error.  The 4-arg form with
dnl a custom error action trips autoconf 2.73's strict mode on macOS.
PKG_CHECK_MODULES([ZSTD], [libzstd >= 1.4.0],
  [AC_DEFINE([HAVE_LIBZSTD], [1], [Define if libzstd is available])])

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

dnl Check for liburing.  Split pkg-config and manual-check logic into
dnl flat AS_IF blocks -- nesting an error-macro call inside the 4th
dnl arg of PKG_CHECK_MODULES (or inside a raw `if test ... fi` block
dnl inside that 4th arg) trips autoconf 2.73's strict mode.  Each
dnl error path is now its own top-level AS_IF.
have_liburing=no
AS_IF([test "x$enable_linux_io_uring" != "xno"], [
    PKG_CHECK_MODULES([URING], [liburing >= 2.3],
        [AC_DEFINE([HAVE_LINUX_IO_URING], [1], [Define if Linux io_uring is available])
         have_liburing=yes],
        [have_liburing=no])
])
AS_IF([test "x$have_liburing" = "xno" && test "x$enable_linux_io_uring" != "xno"], [
    dnl pkg-config did not find liburing >= 2.3.  Fall back to a manual
    dnl header+lib probe -- accepts any liburing but warns that the
    dnl build may fail if < 2.3.
    AC_CHECK_HEADERS([liburing.h],
        [AC_CHECK_LIB([uring], [io_uring_queue_init],
            [AC_MSG_WARN([liburing found but version check failed - build might fail if < 2.3])
             AC_DEFINE([HAVE_LINUX_IO_URING], [1], [Define if Linux io_uring is available])
             URING_LIBS="-luring"
             have_liburing=yes],
            [have_liburing=no])],
        [have_liburing=no])
])
AS_IF([test "x$have_liburing" = "xno" && test "x$enable_linux_io_uring" = "xyes"],
      [AC_MSG_ERROR([io_uring support explicitly requested but liburing/liburing.h not found])])
AC_SUBST([URING_LIBS])

# Check for FreeBSD aio + kqueue support
#
# This backend provides the same async I/O semantics as io_uring on
# Linux, using POSIX aio for file operations and kqueue for socket
# readiness, timers, and completion notification.  See
# docs/io-backend-port-plan.md.
AC_ARG_ENABLE([freebsd-kqueue],
    [AS_HELP_STRING([--enable-freebsd-kqueue],
        [enable FreeBSD aio+kqueue I/O backend @<:@default=auto@:>@])],
    [],
    [enable_freebsd_kqueue=auto])

have_freebsd_kqueue=no
AS_IF([test "x$enable_freebsd_kqueue" != "xno"], [
    AC_CHECK_HEADERS([sys/event.h sys/aio.h])
    dnl accept4 is probed separately above via AC_CHECK_DECLS; do not
    dnl include it here -- AC_CHECK_FUNCS does a link test that wrongly
    dnl succeeds on Darwin under -Wl,-undefined,error.
    AC_CHECK_FUNCS([kqueue kevent aio_read aio_write aio_cancel aio_error aio_return])
    AS_IF([test "x$ac_cv_header_sys_event_h" = "xyes" && \
           test "x$ac_cv_header_sys_aio_h" = "xyes" && \
           test "x$ac_cv_func_kqueue" = "xyes" && \
           test "x$ac_cv_func_kevent" = "xyes" && \
           test "x$ac_cv_func_aio_read" = "xyes" && \
           test "x$ac_cv_func_aio_write" = "xyes" && \
           test "x$ac_cv_func_aio_cancel" = "xyes"], [
        AC_DEFINE([HAVE_FREEBSD_KQUEUE], [1],
                  [Define if the FreeBSD aio+kqueue backend is available])
        have_freebsd_kqueue=yes
    ], [
        AS_IF([test "x$enable_freebsd_kqueue" = "xyes"], [
            AC_MSG_ERROR([FreeBSD kqueue backend explicitly requested but required headers/functions are missing])
        ])
    ])
])

# Exactly one I/O backend is compiled; liburing is preferred on Linux,
# kqueue is used on FreeBSD.  If neither is available, configure
# continues (the build will fail with a clear error in lib/io/
# Makefile.am -- configure-time isn't the right place to diagnose
# platform-specific build infrastructure gaps).
have_linux_io_uring=no
AS_IF([test "x$ac_cv_header_liburing_h" = "xyes" || \
       test -n "$URING_LIBS"], [have_linux_io_uring=yes])

AM_CONDITIONAL([IO_BACKEND_LIBURING],
    [test "x$have_linux_io_uring" = "xyes"])
# IO_BACKEND_KQUEUE is the FreeBSD aio+kqueue backend.  The probes
# (kqueue, kevent, aio_read, aio_write) all succeed on Darwin too, so
# a naive "if kqueue probes then enable" condition misdetects macOS.
# Positive host_is_freebsd=yes fence required to route Darwin to the
# thread-pool backend below.
AM_CONDITIONAL([IO_BACKEND_KQUEUE],
    [test "x$have_freebsd_kqueue" = "xyes" && \
     test "x$have_linux_io_uring" != "xyes" && \
     test "x$host_is_freebsd" = "xyes"])
# IO_BACKEND_DARWIN is the macOS thread-pool file-I/O backend (shares
# the kqueue socket code with IO_BACKEND_KQUEUE).
AM_CONDITIONAL([IO_BACKEND_DARWIN],
    [test "x$host_is_darwin" = "xyes"])

# Defensive check: if we routed to no backend at all, fail configure
# explicitly rather than producing a link-time mystery later.
AS_IF([test "x$have_linux_io_uring" != "xyes" && \
       test "x$have_freebsd_kqueue" != "xyes" && \
       test "x$host_is_darwin" != "xyes"], [
    AC_MSG_ERROR([no I/O backend selected: need liburing (Linux), kqueue+aio (FreeBSD), or Darwin (macOS).])
])

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
    AC_MSG_WARN([hdr_histogram.h not found; histogram telemetry will be disabled.  On Linux, install HdrHistogram_c-devel.  On FreeBSD, no port exists -- build from source: see docs/freebsd-install.md.])
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

# Check for OpenSSL.  3-arg form — PKG_CHECK_MODULES's default
# "Package requirements not met" error is clear enough; the 4-arg
# form with a custom error action trips autoconf 2.73's strict mode.
PKG_CHECK_MODULES([OPENSSL], [openssl >= 1.0.2])

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

dnl Split the warn-on-failure into a separate AS_IF -- passing a
dnl warn/error macro call as the 4th arg to PKG_CHECK_MODULES trips
dnl autoconf 2.73's strict mode on macOS (the arg is left overquoted
dnl and the macro name survives unexpanded).
have_gssapi_krb5=no
PKG_CHECK_MODULES([GSSAPI], [krb5-gssapi],
    [AC_DEFINE([HAVE_GSSAPI_KRB5], [1], [Define if libgssapi_krb5 is available])
     have_gssapi_krb5=yes],
    [have_gssapi_krb5=no])
AS_IF([test "x$have_gssapi_krb5" = "xno"],
      [AC_MSG_WARN([libgssapi_krb5 not found -- RPCSEC_GSS/krb5 disabled])])
AC_SUBST([GSSAPI_CFLAGS])
AC_SUBST([GSSAPI_LIBS])

# Check for libnfsidmap (optional — NFSv4 principal→uid mapping)
have_libnfsidmap=no
PKG_CHECK_MODULES([NFSIDMAP], [libnfsidmap],
    [AC_DEFINE([HAVE_LIBNFSIDMAP], [1], [Define if libnfsidmap is available])
     have_libnfsidmap=yes],
    [have_libnfsidmap=no])
AS_IF([test "x$have_libnfsidmap" = "xno"],
      [AC_MSG_WARN([libnfsidmap not found -- GSS principal mapping uses getpwnam fallback])])
AC_SUBST([NFSIDMAP_CFLAGS])
AC_SUBST([NFSIDMAP_LIBS])

# Check for RocksDB (optional — RocksDB metadata backend)
have_rocksdb=no
PKG_CHECK_MODULES([ROCKSDB], [rocksdb],
    [AC_DEFINE([HAVE_ROCKSDB], [1], [Define if RocksDB is available])
     have_rocksdb=yes],
    [have_rocksdb=no])
AS_IF([test "x$have_rocksdb" = "xno"],
      [AC_MSG_WARN([RocksDB not found -- RocksDB metadata backend disabled])])
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
    ],
    [darwin*], [
        # Silence the xdrproc_t cast warning on XDR-generator output.
        # The flag name differs across clang versions:
        #   - Apple CLT clang 20+ / upstream 20+: -Wcast-function-type-mismatch
        #   - Upstream clang 19 and earlier: -Wcast-function-type-strict
        #   - GCC (and older clangs): no such warning
        # Probe each variant and add whichever the compiler accepts.
        # Must come AFTER the -Wextra in the common CFLAGS block
        # since -Wextra re-enables the grouped warning.
        AX_CHECK_COMPILE_FLAG([-Wno-cast-function-type-mismatch],
            [CFLAGS="${CFLAGS} -Wno-cast-function-type-mismatch"],
            [AX_CHECK_COMPILE_FLAG([-Wno-cast-function-type-strict],
                [CFLAGS="${CFLAGS} -Wno-cast-function-type-strict"],
                [])])
        # Same liburcu GNU-extension issue as FreeBSD: named
        # variadic macros + zero-argument variadic macro
        # invocations.  Suppress under -Wpedantic rather than drop
        # -Wpedantic entirely.
        CFLAGS="${CFLAGS} -Wno-variadic-macros -Wno-c23-extensions -Wno-gnu"
        # Apple CLT clang's -Wextra enables -Wsign-compare and
        # -Wpointer-sign; GCC's -Wextra does not.  The warnings
        # came entirely from lib/utils/identity.c interacting with
        # Darwin's base-libc struct authunix_parms, which declares
        # aup_uid/aup_gid as `int` and aup_gids as `int *` while
        # Linux libtirpc uses uid_t/gid_t (unsigned int).  Fixed
        # in identity.c via AUP_UID/AUP_GID/AUP_GIDS cast macros;
        # -Wno-sign-compare / -Wno-pointer-sign no longer needed.
        # See #20.
        # Darwin's base-libc SunRPC declares clnt_create(char *, ...)
        # rather than clnt_create(const char *, ...) -- a pre-C89
        # signature that fires
        # -Wincompatible-pointer-types-discards-qualifiers when
        # reffs passes const-qualified strings.  Likewise the
        # auth_destroy macro expands to a K&R-style indirect call
        # that clang flags as -Wdeprecated-non-prototype under C23.
        # Both are vendor-header artifacts, not reffs bugs.
        CFLAGS="${CFLAGS} -Wno-incompatible-pointer-types-discards-qualifiers"
        CFLAGS="${CFLAGS} -Wno-deprecated-non-prototype"
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
	lib/nfs4/ps/Makefile
	lib/nfs4/ps/tests/Makefile
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
	lib/io/include/Makefile
	lib/io/include/trace/Makefile
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
