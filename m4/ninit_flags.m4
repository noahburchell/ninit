AC_DEFUN([NINIT_TRY_CFLAG], [
  AS_VAR_PUSHDEF([ninit_cv], [ninit_cv_cflag_$1])
  AC_CACHE_CHECK([whether $CC accepts $1], [ninit_cv], [
    ninit_save_CFLAGS=$CFLAGS
    CFLAGS="-Werror $1"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AS_VAR_SET([ninit_cv], [yes])], [AS_VAR_SET([ninit_cv], [no])])
    CFLAGS=$ninit_save_CFLAGS
  ])
  AS_VAR_IF([ninit_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([ninit_cv])
])

AC_DEFUN([NINIT_TRY_CFLAGS], [
  m4_foreach_w([ninit_flag], [$1], [NINIT_TRY_CFLAG(ninit_flag, [$2])])
])

AC_DEFUN([NINIT_TRY_LDFLAG], [
  AS_VAR_PUSHDEF([ninit_cv], [ninit_cv_ldflag_$1])
  AC_CACHE_CHECK([whether the linker accepts $1], [ninit_cv], [
    ninit_save_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS $1"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AS_VAR_SET([ninit_cv], [yes])], [AS_VAR_SET([ninit_cv], [no])])
    LDFLAGS=$ninit_save_LDFLAGS
  ])
  AS_VAR_IF([ninit_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([ninit_cv])
])

AC_DEFUN([NINIT_TRY_LDFLAG_RUN], [
  AS_VAR_PUSHDEF([ninit_cv], [ninit_cv_ldrun_$1])
  AC_CACHE_CHECK([whether $1 produces a program that runs], [ninit_cv], [
    ninit_save_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS $1"
    AC_RUN_IFELSE([AC_LANG_PROGRAM([[]], [[return 0;]])],
      [AS_VAR_SET([ninit_cv], [yes])],
      [AS_VAR_SET([ninit_cv], [no])],
      [AS_VAR_SET([ninit_cv], [no])])
    LDFLAGS=$ninit_save_LDFLAGS
  ])
  AS_VAR_IF([ninit_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([ninit_cv])
])
