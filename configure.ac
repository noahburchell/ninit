AC_PREREQ([2.69])
AC_INIT([ninit], [1.0.3], [ninit@nburch.org], [ninit],
	[https://github.com/noahburchell/ninit])
AC_CONFIG_SRCDIR([src/ninit.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.16 foreign subdir-objects no-dist-gzip dist-xz -Wall -Wno-portability])
AM_SILENT_RULES([yes])

AM_MAINTAINER_MODE([disable])

: ${CFLAGS=""}
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MKDIR_P
AC_CANONICAL_HOST

AS_CASE([$host_os], [linux*], [], [AC_MSG_ERROR([ninit requires linux, not $host_os])])

AC_CACHE_CHECK([for the C compiler vendor], [ninit_cv_cc_name], [
  AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __clang__
#error not clang
#endif
  ]])], [ninit_cv_cc_name=clang], [
    AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __GNUC__
#error not gcc
#endif
    ]])], [ninit_cv_cc_name=gcc], [ninit_cv_cc_name=cc])
  ])
])
CC_NAME=$ninit_cv_cc_name
AC_SUBST([CC_NAME])
AM_CONDITIONAL([CC_IS_CLANG], [test "x$CC_NAME" = xclang])

ninit_std=
NINIT_TRY_CFLAG([-std=gnu23], [ninit_std])
AS_IF([test -z "$ninit_std"], [NINIT_TRY_CFLAG([-std=gnu2x], [ninit_std])])
AS_IF([test -z "$ninit_std"],
      [AC_MSG_ERROR([$CC does not support C23, gcc 14+ or clang 18+ is required])])

NINIT_OPT_CFLAGS=
NINIT_OPT_LDFLAGS=
NINIT_CFLAGS="$ninit_std"
NINIT_CPPFLAGS=
NINIT_LDFLAGS=

NINIT_TRY_CFLAGS([
	-Wall
	-Wextra
	-Wpedantic
	-Wno-unused-parameter
	-Wshadow
	-Wundef
	-Wcast-qual
	-Wcast-align
	-Wwrite-strings
	-Wpointer-arith
	-Wmissing-prototypes
	-Wmissing-declarations
	-Wstrict-prototypes
	-Wold-style-definition
	-Wredundant-decls
	-Wnested-externs
	-Wswitch-enum
	-Wformat=2
	-Wvla
	-Wdouble-promotion
	-Wfloat-equal
	-Winit-self
	-Wmissing-include-dirs
	-Wimplicit-fallthrough
	-Wnull-dereference
	-Wdate-time
	-Wmissing-variable-declarations
	-Wstrict-aliasing=3
], [NINIT_CFLAGS])

AS_IF([test "x$CC_NAME" = xclang], [
  NINIT_TRY_CFLAGS([
	-Wshadow-all
	-Wassign-enum
	-Wcomma
	-Wconditional-uninitialized
	-Wloop-analysis
	-Wunreachable-code-aggressive
	-Wextra-semi
	-Wover-aligned
	-Wformat-type-confusion
	-Wtautological-constant-in-range-compare
	-Wbitfield-enum-conversion
	-Wcast-function-type-strict
	-Wthread-safety
	-Wheader-guard
	-Wnewline-eof
	-Wstrict-prototypes
  ], [NINIT_CFLAGS])
], [
  NINIT_TRY_CFLAGS([
	-Wlogical-op
	-Wduplicated-cond
	-Wduplicated-branches
	-Wjump-misses-init
	-Wtrampolines
	-Walloc-zero
	-Walloca
	-Warray-bounds=2
	-Wshift-overflow=2
	-Wstringop-overflow=4
	-Wstringop-truncation
	-Wformat-overflow=2
	-Wformat-truncation=2
	-Wuse-after-free=3
	-Wflex-array-member-not-at-end
	-Wbidi-chars=any
	-Wdisabled-optimization
	-Wcalloc-transposed-args
	-Wmultistatement-macros
	-Wsuggest-attribute=format
	-Wsuggest-attribute=noreturn
	-Wattribute-alias=2
	-Wtrivial-auto-var-init
  ], [NINIT_CFLAGS])
])

AC_ARG_ENABLE([werror],
  [AS_HELP_STRING([--enable-werror], [turn warnings into errors])],
  [], [enable_werror=no])
AS_IF([test "x$enable_werror" = xyes], [NINIT_TRY_CFLAG([-Werror], [NINIT_CFLAGS])])

AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [build unoptimised with ASan and UBSan, not for booting])],
  [], [enable_debug=no])

AC_ARG_ENABLE([hardened],
  [AS_HELP_STRING([--enable-hardened],
    [_FORTIFY_SOURCE=3, control-flow protection and further hardening])],
  [], [enable_hardened=no])

ninit_have_lto=no
AS_IF([test "x$enable_debug" = xyes], [
  NINIT_TRY_CFLAGS([-O0 -g3], [NINIT_OPT_CFLAGS])
  NINIT_TRY_CFLAGS([-fno-omit-frame-pointer -fno-common], [NINIT_CFLAGS])
  ninit_san=
  NINIT_TRY_CFLAGS([
	-fsanitize=address
	-fsanitize=undefined
	-fsanitize-address-use-after-scope
	-fno-sanitize-recover=all
  ], [ninit_san])
  AS_IF([test "x$CC_NAME" = xclang], [
    NINIT_TRY_CFLAGS([-fsanitize=integer -fsanitize=local-bounds -fno-sanitize=unsigned-integer-overflow],
		     [ninit_san])
  ])
  NINIT_CFLAGS="$NINIT_CFLAGS$ninit_san"
  NINIT_LDFLAGS="$NINIT_LDFLAGS$ninit_san"
], [
  NINIT_TRY_CFLAGS([-O2 -pipe -fno-plt], [NINIT_OPT_CFLAGS])
  NINIT_TRY_LDFLAG([-Wl,-O2], [NINIT_OPT_LDFLAGS])
  NINIT_TRY_LDFLAG([-Wl,--as-needed], [NINIT_OPT_LDFLAGS])
  NINIT_TRY_LDFLAG([-Wl,--hash-style=gnu], [NINIT_OPT_LDFLAGS])
  NINIT_TRY_LDFLAG_RUN([-Wl,-z,pack-relative-relocs], [NINIT_OPT_LDFLAGS])

  ninit_lto=
  ninit_lto_CFLAGS=$CFLAGS
  CFLAGS=
  AS_IF([test "x$CC_NAME" = xclang], [
    NINIT_TRY_LDFLAG([-flto=thin], [ninit_lto])
    AS_IF([test -z "$ninit_lto"], [NINIT_TRY_LDFLAG([-flto], [ninit_lto])])
  ], [
    NINIT_TRY_LDFLAG([-flto=auto], [ninit_lto])
    AS_IF([test -z "$ninit_lto"], [NINIT_TRY_LDFLAG([-flto], [ninit_lto])])
    AS_IF([test -n "$ninit_lto"], [
      NINIT_TRY_CFLAG([-fno-fat-lto-objects], [ninit_lto])
      NINIT_TRY_CFLAG([-flto-partition=one], [ninit_lto])
      AC_CHECK_TOOLS([AR], [gcc-ar ar], [ar])
      AC_CHECK_TOOLS([RANLIB], [gcc-ranlib ranlib], [ranlib])
    ])
  ])
  CFLAGS=$ninit_lto_CFLAGS
  AS_IF([test -n "$ninit_lto"], [ninit_have_lto=yes])
  NINIT_OPT_CFLAGS="$NINIT_OPT_CFLAGS$ninit_lto"
])

AS_IF([test "x$enable_debug" != xyes], [
  AS_IF([test "x$enable_hardened" = xyes], [ninit_fortify=3], [ninit_fortify=2])
  NINIT_CPPFLAGS="$NINIT_CPPFLAGS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=$ninit_fortify"
])
NINIT_TRY_CFLAGS([-fstack-protector-strong -fstack-clash-protection -fPIE], [NINIT_CFLAGS])
AS_CASE([" $LDFLAGS "], [*" -static "*|*" -static-pie "*], [],
	[NINIT_TRY_LDFLAG([-pie], [NINIT_LDFLAGS])])
NINIT_TRY_LDFLAG([-Wl,-z,relro], [NINIT_LDFLAGS])
NINIT_TRY_LDFLAG([-Wl,-z,now], [NINIT_LDFLAGS])
NINIT_TRY_LDFLAG([-Wl,-z,noexecstack], [NINIT_LDFLAGS])
AS_IF([test "x$enable_hardened" = xyes], [
  NINIT_TRY_CFLAGS([
	-fcf-protection=full
	-mbranch-protection=standard
	-ftrivial-auto-var-init=zero
	-fstrict-flex-arrays=3
	-fno-delete-null-pointer-checks
	-fno-strict-overflow
  ], [NINIT_CFLAGS])
])

AC_ARG_ENABLE([quiet],
  [AS_HELP_STRING([--enable-quiet], [print only warnings and failures while booting])],
  [], [enable_quiet=no])
AS_IF([test "x$enable_quiet" = xyes],
      [AC_DEFINE([NINIT_QUIET], [1], [Only print warnings and failures on the console.])])

AC_ARG_WITH([sulogin],
  [AS_HELP_STRING([--with-sulogin], [run sulogin before the emergency shell])],
  [], [with_sulogin=no])
AS_CASE([$with_sulogin],
  [yes], [AC_DEFINE([NINIT_SULOGIN], [1], [Try sulogin before the emergency shell.])],
  [no], [],
  [AC_MSG_ERROR([--with-sulogin takes no argument, not $with_sulogin])])

AC_ARG_WITH([busybox],
  [AS_HELP_STRING([--with-busybox@<:@=PATH@:>@],
    [busybox to try before /bin/sh for the emergency shell @<:@/bin/busybox@:>@])],
  [], [with_busybox=no])
AS_CASE([$with_busybox],
  [no], [],
  [yes], [with_busybox=/bin/busybox])
AS_IF([test "x$with_busybox" != xno], [
  AS_CASE([$with_busybox], [/*], [],
	  [AC_MSG_ERROR([--with-busybox requires an absolute path, not $with_busybox])])
  AC_DEFINE_UNQUOTED([NINIT_BUSYBOX], ["$with_busybox"],
		     [busybox to try first for the emergency shell])
])

AC_ARG_WITH([shell],
  [AS_HELP_STRING([--with-shell=PATH],
    [interpreter for service scripts @<:@/bin/bash@:>@])],
  [], [with_shell=/bin/bash])
AS_CASE([$with_shell], [/*], [],
	[AC_MSG_ERROR([--with-shell requires an absolute path, not $with_shell])])

AC_ARG_WITH([shell-name],
  [AS_HELP_STRING([--with-shell-name=NAME],
    [argv@<:@0@:>@ for that interpreter @<:@basename of --with-shell@:>@])],
  [], [with_shell_name=`echo "$with_shell" | sed 's|.*/||'`])
AC_DEFINE_UNQUOTED([NG_SHELL], ["$with_shell"], [Interpreter for service scripts.])
AC_DEFINE_UNQUOTED([NG_SHELL_ARGV0], ["$with_shell_name"],
		   [argv[0] to hand that interpreter.])
ninit_shell=$with_shell
AC_SUBST([ninit_shell])

AC_ARG_WITH([service-dir],
  [AS_HELP_STRING([--with-service-dir=DIR], [directory holding service files @<:@/etc/ninit.d@:>@])],
  [], [with_service_dir=/etc/ninit.d])
AS_CASE([$with_service_dir], [/*], [],
	[AC_MSG_ERROR([--with-service-dir requires an absolute path])])
AC_DEFINE_UNQUOTED([NG_DEFAULT_DIR], ["$with_service_dir"], [Service directory.])
AC_DEFINE_UNQUOTED([NG_DEFAULT_FILE], ["$with_service_dir/depgraph"], [Compiled graph.])
ninit_svcdir=$with_service_dir
AC_SUBST([ninit_svcdir])

AC_ARG_ENABLE([man],
  [AS_HELP_STRING([--disable-man], [do not install the manual pages])],
  [], [enable_man=yes])
AM_CONDITIONAL([ENABLE_MAN], [test "x$enable_man" != xno])

AC_CHECK_HEADERS([stdckdint.h linux/close_range.h sys/signalfd.h sys/statfs.h sys/swap.h])
AC_CHECK_FUNCS([renameat2 memfd_create close_range mkostemp accept4 pipe2 \
		signalfd scandir sigtimedwait swapoff statfs])
AC_CHECK_DECLS([RENAME_NOREPLACE], [], [], [[
#include <stdio.h>
#include <fcntl.h>
]])
AC_CHECK_DECLS([CLOSE_RANGE_CLOEXEC], [], [], [[
#include <unistd.h>
#ifdef HAVE_LINUX_CLOSE_RANGE_H
#include <linux/close_range.h>
#endif
]])

AC_MSG_CHECKING([for a usable renameat2])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/syscall.h>
#include <unistd.h>
]], [[
	return (int)SYS_renameat2;
]])], [
  AC_MSG_RESULT([yes, via syscall])
  AC_DEFINE([HAVE_SYS_RENAMEAT2], [1], [Define if SYS_renameat2 is known.])
], [
  AS_IF([test "x$ac_cv_func_renameat2" = xyes],
	[AC_MSG_RESULT([yes, via libc])],
	[AC_MSG_ERROR([no usable renameat2])])
])

AC_SUBST([NINIT_OPT_CFLAGS])
AC_SUBST([NINIT_OPT_LDFLAGS])
AC_SUBST([NINIT_CFLAGS])
AC_SUBST([NINIT_CPPFLAGS])
AC_SUBST([NINIT_LDFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
  ninit $VERSION

    compiler ........ $CC ($CC_NAME)
    sbindir ......... $(eval echo $(eval echo $sbindir))
    services ........ $with_service_dir
    interpreter ..... $with_shell (argv[0] $with_shell_name)

    lto ............. $ninit_have_lto
    hardened ........ $enable_hardened
    sanitisers ...... $enable_debug
    quiet boot ...... $enable_quiet
    sulogin ......... $with_sulogin
    busybox shell ... $with_busybox
    man pages ....... $enable_man

    CFLAGS ..........$NINIT_OPT_CFLAGS $CFLAGS$NINIT_CPPFLAGS$NINIT_CFLAGS
    LDFLAGS .........$NINIT_OPT_LDFLAGS $LDFLAGS$NINIT_LDFLAGS
])
