# aclocal.m4 generated automatically by aclocal 1.6.3 -*- Autoconf -*-

# Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002
# Free Software Foundation, Inc.
# This file is free software; the Free Software Foundation
# gives unlimited permission to copy and/or distribute it,
# with or without modifications, as long as this notice is preserved.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY, to the extent permitted by law; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.

# $PostgreSQL: pgsql/config/general.m4,v 1.3 2003/11/29 19:51:17 pgsql Exp $

# This file defines new macros to process configure command line
# arguments, to replace the brain-dead AC_ARG_WITH and AC_ARG_ENABLE.
# The flaw in these is particularly that they only differentiate
# between "given" and "not given" and do not provide enough help to
# process arguments that only accept "yes/no", that require an
# argument (other than "yes/no"), etc.
#
# The point of this implementation is to reduce code size and
# redundancy in configure.in and to improve robustness and consistency
# in the option evaluation code.


# Convert type and name to shell variable name (e.g., "enable_long_strings")
m4_define([pgac_arg_to_variable],
          [$1[]_[]patsubst($2, -, _)])


# PGAC_ARG(TYPE, NAME, HELP-STRING,
#          [ACTION-IF-YES], [ACTION-IF-NO], [ACTION-IF-ARG],
#          [ACTION-IF-OMITTED])
# ----------------------------------------------------------
# This is the base layer. TYPE is either "with" or "enable", depending
# on what you like. NAME is the rest of the option name, HELP-STRING
# as usual. ACTION-IF-YES is executed if the option is given without
# and argument (or "yes", which is the same); similar for ACTION-IF-NO.

AC_DEFUN([PGAC_ARG],
[
m4_case([$1],

enable, [
AC_ARG_ENABLE([$2], [$3], [
  case [$]enableval in
    yes)
      m4_default([$4], :)
      ;;
    no)
      m4_default([$5], :)
      ;;
    *)
      $6
      ;;
  esac
],
[$7])[]dnl AC_ARG_ENABLE
],

with, [
AC_ARG_WITH([$2], [$3], [
  case [$]withval in
    yes)
      m4_default([$4], :)
      ;;
    no)
      m4_default([$5], :)
      ;;
    *)
      $6
      ;;
  esac
],
[$7])[]dnl AC_ARG_WITH
],

[m4_fatal([first argument of $0 must be 'enable' or 'with', not '$1'])]
)
])# PGAC_ARG


# PGAC_ARG_BOOL(TYPE, NAME, DEFAULT, HELP-STRING, 
#               [ACTION-IF-YES], [ACTION-IF-NO])
# -----------------------------------------------
# Accept a boolean option, that is, one that only takes yes or no.
# ("no" is equivalent to "disable" or "without"). DEFAULT is what
# should be done if the option is omitted; it should be "yes" or "no".
# (Consequently, one of ACTION-IF-YES and ACTION-IF-NO will always
# execute.)

AC_DEFUN([PGAC_ARG_BOOL],
[PGAC_ARG([$1], [$2], [$4], [$5], [$6], 
          [AC_MSG_ERROR([no argument expected for --$1-$2 option])],
          [m4_case([$3],
                   yes, [pgac_arg_to_variable([$1], [$2])=yes
$5],
                   no,  [pgac_arg_to_variable([$1], [$2])=no
$6],
                   [m4_fatal([third argument of $0 must be 'yes' or 'no', not '$3'])])])[]dnl
])# PGAC_ARG_BOOL


# PGAC_ARG_REQ(TYPE, NAME, HELP-STRING, [ACTION-IF-GIVEN], [ACTION-IF-NOT-GIVEN])
# -------------------------------------------------------------------------------
# This option will require an argument; "yes" or "no" will not be
# accepted.

AC_DEFUN([PGAC_ARG_REQ],
[PGAC_ARG([$1], [$2], [$3],
          [AC_MSG_ERROR([argument required for --$1-$2 option])],
          [AC_MSG_ERROR([argument required for --$1-$2 option])],
          [$4],
          [$5])])# PGAC_ARG_REQ


# PGAC_ARG_OPTARG(TYPE, NAME, HELP-STRING, [DEFAULT-ACTION], [ARG-ACTION]
#                 [ACTION-ENABLED], [ACTION-DISABLED])
# -----------------------------------------------------------------------
# This will create an option that behaves as follows: If omitted, or
# called with "no", then set the enable_variable to "no" and do
# nothing else. If called with "yes", then execute DEFAULT-ACTION. If
# called with argument, set enable_variable to "yes" and execute
# ARG-ACTION. Additionally, execute ACTION-ENABLED if we ended up with
# "yes" either way, else ACTION-DISABLED.
#
# The intent is to allow enabling a feature, and optionally pass an
# additional piece of information.

AC_DEFUN([PGAC_ARG_OPTARG],
[PGAC_ARG([$1], [$2], [$3], [$4], [],
          [pgac_arg_to_variable([$1], [$2])=yes
$5],
          [pgac_arg_to_variable([$1], [$2])=no])
dnl Add this code only if there's a ACTION-ENABLED or ACTION-DISABLED.
m4_ifval([$6[]$7],
[
if test "[$]pgac_arg_to_variable([$1], [$2])" = yes; then
  m4_default([$6], :)
m4_ifval([$7],
[else
  $7
])[]dnl
fi
])[]dnl
])# PGAC_ARG_OPTARG

# Macros to detect C compiler features
# $PostgreSQL: pgsql/config/c-compiler.m4,v 1.14 2004/12/16 17:48:25 momjian Exp $


# PGAC_C_SIGNED
# -------------
# Check if the C compiler understands signed types.
AC_DEFUN([PGAC_C_SIGNED],
[AC_CACHE_CHECK(for signed types, pgac_cv_c_signed,
[AC_TRY_COMPILE([],
[signed char c; signed short s; signed int i;],
[pgac_cv_c_signed=yes],
[pgac_cv_c_signed=no])])
if test x"$pgac_cv_c_signed" = xno ; then
  AC_DEFINE(signed,, [Define to empty if the C compiler does not understand signed types.])
fi])# PGAC_C_SIGNED



# PGAC_TYPE_64BIT_INT(TYPE)
# -------------------------
# Check if TYPE is a working 64 bit integer type. Set HAVE_TYPE_64 to
# yes or no respectively, and define HAVE_TYPE_64 if yes.
AC_DEFUN([PGAC_TYPE_64BIT_INT],
[define([Ac_define], [translit([have_$1_64], [a-z *], [A-Z_P])])dnl
define([Ac_cachevar], [translit([pgac_cv_type_$1_64], [ *], [_p])])dnl
AC_CACHE_CHECK([whether $1 is 64 bits], [Ac_cachevar],
[AC_TRY_RUN(
[typedef $1 ac_int64;

/*
 * These are globals to discourage the compiler from folding all the
 * arithmetic tests down to compile-time constants.
 */
ac_int64 a = 20000001;
ac_int64 b = 40000005;

int does_int64_work()
{
  ac_int64 c,d;

  if (sizeof(ac_int64) != 8)
    return 0;			/* definitely not the right size */

  /* Do perfunctory checks to see if 64-bit arithmetic seems to work */
  c = a * b;
  d = (c + b) / b;
  if (d != a+1)
    return 0;
  return 1;
}
main() {
  exit(! does_int64_work());
}],
[Ac_cachevar=yes],
[Ac_cachevar=no],
[# If cross-compiling, check the size reported by the compiler and
# trust that the arithmetic works.
AC_COMPILE_IFELSE([AC_LANG_BOOL_COMPILE_TRY([], [sizeof($1) == 8])],
                  Ac_cachevar=yes,
                  Ac_cachevar=no)])])

Ac_define=$Ac_cachevar
if test x"$Ac_cachevar" = xyes ; then
  AC_DEFINE(Ac_define,, [Define to 1 if `]$1[' works and is 64 bits.])
fi
undefine([Ac_define])dnl
undefine([Ac_cachevar])dnl
])# PGAC_TYPE_64BIT_INT



# PGAC_CHECK_ALIGNOF(TYPE, [INCLUDES = DEFAULT-INCLUDES])
# -----------------------------------------------------
# Find the alignment requirement of the given type. Define the result
# as ALIGNOF_TYPE.  This macro works even when cross compiling.
# (Modelled after AC_CHECK_SIZEOF.)

AC_DEFUN([PGAC_CHECK_ALIGNOF],
[AS_LITERAL_IF([$1], [],
               [AC_FATAL([$0: requires literal arguments])])dnl
AC_CHECK_TYPE([$1], [], [], [$2])
AC_CACHE_CHECK([alignment of $1], [AS_TR_SH([pgac_cv_alignof_$1])],
[if test "$AS_TR_SH([ac_cv_type_$1])" = yes; then
  _AC_COMPUTE_INT([((char*) & pgac_struct.field) - ((char*) & pgac_struct)],
                  [AS_TR_SH([pgac_cv_alignof_$1])],
                  [AC_INCLUDES_DEFAULT([$2])
struct { char filler; $1 field; } pgac_struct;],
                  [AC_MSG_ERROR([cannot compute alignment of $1, 77])])
else
  AS_TR_SH([pgac_cv_alignof_$1])=0
fi])dnl
AC_DEFINE_UNQUOTED(AS_TR_CPP(alignof_$1),
                   [$AS_TR_SH([pgac_cv_alignof_$1])],
                   [The alignment requirement of a `$1'.])
])# PGAC_CHECK_ALIGNOF


# PGAC_C_FUNCNAME_SUPPORT
# -----------------------
# Check if the C compiler understands __func__ (C99) or __FUNCTION__ (gcc).
# Define HAVE_FUNCNAME__FUNC or HAVE_FUNCNAME__FUNCTION accordingly.
AC_DEFUN([PGAC_C_FUNCNAME_SUPPORT],
[AC_CACHE_CHECK(for __func__, pgac_cv_funcname_func_support,
[AC_TRY_COMPILE([#include <stdio.h>],
[printf("%s\n", __func__);],
[pgac_cv_funcname_func_support=yes],
[pgac_cv_funcname_func_support=no])])
if test x"$pgac_cv_funcname_func_support" = xyes ; then
AC_DEFINE(HAVE_FUNCNAME__FUNC, 1,
          [Define to 1 if your compiler understands __func__.])
else
AC_CACHE_CHECK(for __FUNCTION__, pgac_cv_funcname_function_support,
[AC_TRY_COMPILE([#include <stdio.h>],
[printf("%s\n", __FUNCTION__);],
[pgac_cv_funcname_function_support=yes],
[pgac_cv_funcname_function_support=no])])
if test x"$pgac_cv_funcname_function_support" = xyes ; then
AC_DEFINE(HAVE_FUNCNAME__FUNCTION, 1,
          [Define to 1 if your compiler understands __FUNCTION__.])
fi
fi])# PGAC_C_FUNCNAME_SUPPORT

# PGAC_PROG_CC_CFLAGS_OPT
# -----------------------
# Given a string, check if the compiler supports the string as a
# command-line option. If it does, add the string to CFLAGS.
AC_DEFUN([PGAC_PROG_CC_CFLAGS_OPT],
[AC_MSG_CHECKING([if $CC supports $1])
pgac_save_CFLAGS=$CFLAGS
CFLAGS="$pgac_save_CFLAGS $1"
_AC_COMPILE_IFELSE([AC_LANG_PROGRAM()],
                   AC_MSG_RESULT(yes),
                   [CFLAGS="$pgac_save_CFLAGS"
                    AC_MSG_RESULT(no)])
])# PGAC_PROG_CC_CFLAGS_OPT

# The below backpatches the following Autoconf change:
#
# 2002-03-28  Kevin Ryde  <user42@zip.com.au>
#
#         * lib/autoconf/c.m4 (AC_C_INLINE): Test with a typedef return value,
#         to avoid versions of HP C which don't allow that.
#
# When we upgrade to Autoconf >= 2.53a then we can drop this and rely
# on the standard macro.

# AC_C_INLINE
# -----------
# Do nothing if the compiler accepts the inline keyword.
# Otherwise define inline to __inline__ or __inline if one of those work,
# otherwise define inline to be empty.
AC_DEFUN([AC_C_INLINE],
[AC_REQUIRE([AC_PROG_CC_STDC])dnl
AC_CACHE_CHECK([for inline], ac_cv_c_inline,
[ac_cv_c_inline=no
for ac_kw in inline __inline__ __inline; do
  AC_COMPILE_IFELSE([AC_LANG_SOURCE(
[#ifndef __cplusplus
typedef int foo_t;
static $ac_kw foo_t static_foo () {return 0; }
$ac_kw int foo () {return 0; }
#endif
])],
                    [ac_cv_c_inline=$ac_kw; break])
done
])
case $ac_cv_c_inline in
  inline | yes) ;;
  no) AC_DEFINE(inline,,
                [Define as `__inline' if that's what the C compiler calls it,
                 or to nothing if it is not supported.]) ;;
  *)  AC_DEFINE_UNQUOTED(inline, $ac_cv_c_inline) ;;
esac
])# AC_C_INLINE

# $PostgreSQL: pgsql/config/programs.m4,v 1.18 2004/12/02 20:04:19 tgl Exp $


# PGAC_PATH_FLEX
# --------------
# Look for Flex, set the output variable FLEX to its path if found.
# Avoid the buggy version 2.5.3. Also find Flex if its installed
# under `lex', but do not accept other Lex programs.

AC_DEFUN([PGAC_PATH_FLEX],
[AC_CACHE_CHECK([for flex], pgac_cv_path_flex,
[# Let the user override the test
if test -n "$FLEX"; then
  pgac_cv_path_flex=$FLEX
else
  pgac_save_IFS=$IFS
  IFS=$PATH_SEPARATOR
  for pgac_dir in $PATH; do
    IFS=$pgac_save_IFS
    if test -z "$pgac_dir" || test x"$pgac_dir" = x"."; then
      pgac_dir=`pwd`
    fi
    for pgac_prog in flex lex; do
      pgac_candidate="$pgac_dir/$pgac_prog"
      if test -f "$pgac_candidate" \
        && $pgac_candidate --version </dev/null >/dev/null 2>&1
      then
        echo '%%'  > conftest.l
        if $pgac_candidate -t conftest.l 2>/dev/null | grep FLEX_SCANNER >/dev/null 2>&1; then
          if $pgac_candidate --version | grep ' 2\.5\.3$' >/dev/null 2>&1; then
            pgac_broken_flex=$pgac_candidate
            continue
          fi

          pgac_cv_path_flex=$pgac_candidate
          break 2
        fi
      fi
    done
  done
  rm -f conftest.l lex.yy.c
  : ${pgac_cv_path_flex=no}
fi
])[]dnl AC_CACHE_CHECK

if test x"$pgac_cv_path_flex" = x"no"; then
  if test -n "$pgac_broken_flex"; then
    AC_MSG_WARN([
*** The Flex version 2.5.3 you have at $pgac_broken_flex contains a bug. You
*** should get version 2.5.4 or later.])
  fi

  AC_MSG_WARN([
*** Without Flex you will not be able to build PostgreSQL from CVS or
*** change any of the scanner definition files.  You can obtain Flex from
*** a GNU mirror site.  (If you are using the official distribution of
*** PostgreSQL then you do not need to worry about this because the Flex
*** output is pre-generated.)])
fi

if test x"$pgac_cv_path_flex" = x"no"; then
  FLEX=
else
  FLEX=$pgac_cv_path_flex
fi

AC_SUBST(FLEX)
AC_SUBST(FLEXFLAGS)
])# PGAC_PATH_FLEX



# PGAC_CHECK_READLINE
# -------------------
# Check for the readline library and dependent libraries, either
# termcap or curses.  Also try libedit, since NetBSD's is compatible.
# Add the required flags to LIBS, define HAVE_LIBREADLINE.

AC_DEFUN([PGAC_CHECK_READLINE],
[AC_REQUIRE([AC_CANONICAL_HOST])
AC_MSG_CHECKING([for readline])

AC_CACHE_VAL([pgac_cv_check_readline],
[pgac_cv_check_readline=no
pgac_save_LIBS=$LIBS
for pgac_rllib in -lreadline -ledit ; do
  for pgac_lib in "" " -ltermcap" " -lncurses" " -lcurses" ; do
    LIBS="${pgac_rllib}${pgac_lib} $pgac_save_LIBS"
    AC_TRY_LINK_FUNC([readline], [[
      # Older NetBSD, OpenBSD, and Irix have a broken linker that does not
      # recognize dependent libraries; assume curses is needed if we didn't
      # find any dependency.
      case $host_os in
        netbsd* | openbsd* | irix*)
          if test x"$pgac_lib" = x"" ; then
            pgac_lib=" -lcurses"
          fi ;;
      esac

      pgac_cv_check_readline="${pgac_rllib}${pgac_lib}"
      break 2
    ]])
  done
done
LIBS=$pgac_save_LIBS
])[]dnl AC_CACHE_VAL

if test "$pgac_cv_check_readline" != no ; then
  AC_DEFINE(HAVE_LIBREADLINE, 1, [Define if you have a function readline library])
  LIBS="$pgac_cv_check_readline $LIBS"
  AC_MSG_RESULT([yes ($pgac_cv_check_readline)])
else
  AC_MSG_RESULT(no)
fi])# PGAC_CHECK_READLINE



# PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER
# ---------------------------------------
# Readline versions < 2.1 don't have rl_completion_append_character

AC_DEFUN([PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER],
[AC_MSG_CHECKING([for rl_completion_append_character])
AC_TRY_LINK([#include <stdio.h>
#ifdef HAVE_READLINE_READLINE_H
# include <readline/readline.h>
#elif defined(HAVE_READLINE_H)
# include <readline.h>
#endif
],
[rl_completion_append_character = 'x';],
[AC_MSG_RESULT(yes)
AC_DEFINE(HAVE_RL_COMPLETION_APPEND_CHARACTER, 1,
          [Define to 1 if you have the global variable 'rl_completion_append_character'.])],
[AC_MSG_RESULT(no)])])# PGAC_VAR_RL_COMPLETION_APPEND_CHARACTER



# PGAC_CHECK_GETTEXT
# ------------------

AC_DEFUN([PGAC_CHECK_GETTEXT],
[
  AC_SEARCH_LIBS(gettext, intl, [],
                 [AC_MSG_ERROR([a gettext implementation is required for NLS])])
  AC_CHECK_HEADER([libintl.h], [],
                  [AC_MSG_ERROR([header file <libintl.h> is required for NLS])])
  AC_CHECK_PROGS(MSGFMT, msgfmt)
  if test -z "$MSGFMT"; then
    AC_MSG_ERROR([msgfmt is required for NLS])
  fi
  AC_CHECK_PROGS(MSGMERGE, msgmerge)
dnl FIXME: We should probably check for version >=0.10.36.
  AC_CHECK_PROGS(XGETTEXT, xgettext)

  # Note: share/locale is always the default, independent of $datadir
  localedir='${prefix}/share/locale'
  if test x"$prefix" = x"NONE"; then
    exp_localedir="$ac_default_prefix/share/locale"
  else
    exp_localedir="$prefix/share/locale"
  fi

  AC_SUBST(localedir)
  AC_DEFINE_UNQUOTED(LOCALEDIR, ["$exp_localedir"],
                     [Define to the location of locale files.])
])# PGAC_CHECK_GETTEXT



# PGAC_CHECK_STRIP
# ----------------
# Check for a 'strip' program, and figure out if that program can
# strip libraries.

AC_DEFUN([PGAC_CHECK_STRIP],
[
  AC_CHECK_TOOL(STRIP, strip, :)

  AC_MSG_CHECKING([whether it is possible to strip libraries])
  if test x"$STRIP" != x"" && "$STRIP" -V 2>&1 | grep "GNU strip" >/dev/null; then
    STRIP_STATIC_LIB="$STRIP -x"
    STRIP_SHARED_LIB="$STRIP --strip-unneeded"
    AC_MSG_RESULT(yes)
  else
    STRIP_STATIC_LIB=:
    STRIP_SHARED_LIB=:
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(STRIP_STATIC_LIB)
  AC_SUBST(STRIP_SHARED_LIB)
])# PGAC_CHECK_STRIP


# No, PostgreSQL doesn't use libtool (yet), we just borrow stuff from it.
# This file was taken on 2000-10-20 from the multi-language branch (since
# that is the branch that PostgreSQL would most likely adopt anyway).
# --petere

# ... bunch of stuff removed here ...

# PGAC_PROG_LD - find the path to the GNU or non-GNU linker
AC_DEFUN([PGAC_PROG_LD],
[AC_ARG_WITH(gnu-ld,
[  --with-gnu-ld           assume the C compiler uses GNU ld [[default=no]]],
test "$withval" = no || with_gnu_ld=yes, with_gnu_ld=no)
AC_REQUIRE([AC_PROG_CC])dnl
AC_REQUIRE([AC_CANONICAL_HOST])dnl
dnl ###not for PostgreSQL### AC_REQUIRE([AC_CANONICAL_BUILD])dnl
ac_prog=ld
if test "$GCC" = yes; then
  # Check if gcc -print-prog-name=ld gives a path.
  AC_MSG_CHECKING([for ld used by GCC])
  case $host in
  *-*-mingw*)
    # gcc leaves a trailing carriage return which upsets mingw
    ac_prog=`($CC -print-prog-name=ld) 2>&5 | tr -d '\015'` ;;
  *)
    ac_prog=`($CC -print-prog-name=ld) 2>&5` ;;
  esac
  case "$ac_prog" in
    # Accept absolute paths.
changequote(,)dnl
    [\\/]* | [A-Za-z]:[\\/]*)
      re_direlt='/[^/][^/]*/\.\./'
changequote([,])dnl
      # Canonicalize the path of ld
      ac_prog=`echo $ac_prog| sed 's%\\\\%/%g'`
      while echo $ac_prog | grep "$re_direlt" > /dev/null 2>&1; do
	ac_prog=`echo $ac_prog| sed "s%$re_direlt%/%"`
      done
      test -z "$LD" && LD="$ac_prog"
      ;;
  "")
    # If it fails, then pretend we aren't using GCC.
    ac_prog=ld
    ;;
  *)
    # If it is relative, then search for the first ld in PATH.
    with_gnu_ld=unknown
    ;;
  esac
elif test "$with_gnu_ld" = yes; then
  AC_MSG_CHECKING([for GNU ld])
else
  AC_MSG_CHECKING([for non-GNU ld])
fi
AC_CACHE_VAL(ac_cv_path_LD,
[if test -z "$LD"; then
  IFS="${IFS= 	}"; ac_save_ifs="$IFS"; IFS="${IFS}${PATH_SEPARATOR-:}"
  for ac_dir in $PATH; do
    test -z "$ac_dir" && ac_dir=.
    if test -f "$ac_dir/$ac_prog" || test -f "$ac_dir/$ac_prog$ac_exeext"; then
      ac_cv_path_LD="$ac_dir/$ac_prog"
      # Check to see if the program is GNU ld.  I'd rather use --version,
      # but apparently some GNU ld's only accept -v.
      # Break only if it was the GNU/non-GNU ld that we prefer.
      if "$ac_cv_path_LD" -v 2>&1 < /dev/null | egrep '(GNU|with BFD)' > /dev/null; then
	test "$with_gnu_ld" != no && break
      else
	test "$with_gnu_ld" != yes && break
      fi
    fi
  done
  IFS="$ac_save_ifs"
else
  ac_cv_path_LD="$LD" # Let the user override the test with a path.
fi])
LD="$ac_cv_path_LD"
if test -n "$LD"; then
  AC_MSG_RESULT($LD)
else
  AC_MSG_RESULT(no)
fi
test -z "$LD" && AC_MSG_ERROR([no acceptable ld found in \$PATH])
PGAC_PROG_LD_GNU
])

AC_DEFUN([PGAC_PROG_LD_GNU],
[AC_CACHE_CHECK([if the linker ($LD) is GNU ld], ac_cv_prog_gnu_ld,
[# I'd rather use --version here, but apparently some GNU ld's only accept -v.
if $LD -v 2>&1 </dev/null | egrep '(GNU|with BFD)' 1>&5; then
  ac_cv_prog_gnu_ld=yes
else
  ac_cv_prog_gnu_ld=no
fi])
with_gnu_ld=$ac_cv_prog_gnu_ld
])

# ... more stuff removed ...

# $PostgreSQL: pgsql/config/perl.m4,v 1.3 2003/11/29 19:51:17 pgsql Exp $


# PGAC_PATH_PERL
# --------------
AC_DEFUN([PGAC_PATH_PERL],
[AC_PATH_PROG(PERL, perl)])


# PGAC_CHECK_PERL_CONFIG(NAME)
# ----------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIG],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING([for Perl $1])
perl_$1=`$PERL -MConfig -e 'print $Config{$1}'`
AC_SUBST(perl_$1)dnl
AC_MSG_RESULT([$perl_$1])])


# PGAC_CHECK_PERL_CONFIGS(NAMES)
# ------------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIGS],
[m4_foreach([pgac_item], [$1], [PGAC_CHECK_PERL_CONFIG(pgac_item)])])


# PGAC_CHECK_PERL_EMBED_LDFLAGS
# -----------------------------
AC_DEFUN([PGAC_CHECK_PERL_EMBED_LDFLAGS],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING(for flags to link embedded Perl)
pgac_tmp1=`$PERL -MExtUtils::Embed -e ldopts`
pgac_tmp2=`$PERL -MConfig -e 'print $Config{ccdlflags}'`
perl_embed_ldflags=`echo X"$pgac_tmp1" | sed "s/^X//;s%$pgac_tmp2%%"`
AC_SUBST(perl_embed_ldflags)dnl
AC_MSG_RESULT([$perl_embed_ldflags])])

#
# Autoconf macros for configuring the build of Python extension modules
#
# $PostgreSQL: pgsql/config/python.m4,v 1.11 2004/10/11 19:32:16 tgl Exp $
#

# PGAC_PATH_PYTHON
# ----------------
# Look for Python and set the output variable 'PYTHON'
# to 'python' if found, empty otherwise.
AC_DEFUN([PGAC_PATH_PYTHON],
[AC_PATH_PROG(PYTHON, python)
if test x"$PYTHON" = x""; then
  AC_MSG_ERROR([Python not found])
fi
])


# _PGAC_CHECK_PYTHON_DIRS
# -----------------------
# Determine the name of various directories of a given Python installation.
AC_DEFUN([_PGAC_CHECK_PYTHON_DIRS],
[AC_REQUIRE([PGAC_PATH_PYTHON])
AC_MSG_CHECKING([for Python distutils module])
if "${PYTHON}" 2>&- -c 'import distutils'
then
    AC_MSG_RESULT(yes)
else
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([distutils module not found])
fi
AC_MSG_CHECKING([Python configuration directory])
python_version=`${PYTHON} -c "import sys; print sys.version[[:3]]"`
python_configdir=`${PYTHON} -c "from distutils.sysconfig import get_python_lib as f; import os; print os.path.join(f(plat_specific=1,standard_lib=1),'config')"`
python_includespec=`${PYTHON} -c "import distutils.sysconfig; print '-I'+distutils.sysconfig.get_python_inc()"`

AC_SUBST(python_version)[]dnl
AC_SUBST(python_configdir)[]dnl
AC_SUBST(python_includespec)[]dnl
# This should be enough of a message.
AC_MSG_RESULT([$python_configdir])
])# _PGAC_CHECK_PYTHON_DIRS


# PGAC_CHECK_PYTHON_EMBED_SETUP
# -----------------------------
#
# Note: selecting libpython from python_configdir works in all Python
# releases, but it generally finds a non-shared library, which means
# that we are binding the python interpreter right into libplpython.so.
# In Python 2.3 and up there should be a shared library available in
# the main library location.
AC_DEFUN([PGAC_CHECK_PYTHON_EMBED_SETUP],
[AC_REQUIRE([_PGAC_CHECK_PYTHON_DIRS])
AC_MSG_CHECKING([how to link an embedded Python application])

python_libdir=`${PYTHON} -c "import distutils.sysconfig,string; print string.join(filter(None,distutils.sysconfig.get_config_vars('LIBDIR')))"`
python_ldlibrary=`${PYTHON} -c "import distutils.sysconfig,string; print string.join(filter(None,distutils.sysconfig.get_config_vars('LDLIBRARY')))"`
python_so=`${PYTHON} -c "import distutils.sysconfig,string; print string.join(filter(None,distutils.sysconfig.get_config_vars('SO')))"`
ldlibrary=`echo "${python_ldlibrary}" | sed "s/${python_so}$//"`

if test x"${python_libdir}" != x"" -a x"${python_ldlibrary}" != x"" -a x"${python_ldlibrary}" != x"${ldlibrary}"
then
	# New way: use the official shared library
	ldlibrary=`echo "${ldlibrary}" | sed "s/^lib//"`
	python_libspec="-L${python_libdir} -l${ldlibrary}"
else
	# Old way: use libpython from python_configdir
	python_libdir="${python_configdir}"
	python_libspec="-L${python_libdir} -lpython${python_version}"
fi

python_additional_libs=`${PYTHON} -c "import distutils.sysconfig,string; print string.join(filter(None,distutils.sysconfig.get_config_vars('LIBS','LIBC','LIBM','LOCALMODLIBS','BASEMODLIBS')))"`

AC_MSG_RESULT([${python_libspec} ${python_additional_libs}])

AC_SUBST(python_libdir)[]dnl
AC_SUBST(python_libspec)[]dnl
AC_SUBST(python_additional_libs)[]dnl
])# PGAC_CHECK_PYTHON_EMBED_SETUP

dnl @synopsis ACX_MPI([ACTION-IF-FOUND[, ACTION-IF-NOT-FOUND]])
dnl
dnl This macro tries to find out how to compile programs that
dnl use MPI (Message Passing Interface), a standard API for
dnl parallel process communication (see http://www-unix.mcs.anl.gov/mpi/)
dnl
dnl On success, it sets the MPICC, MPICXX, or MPIF77 output variable to
dnl the name of the MPI compiler, depending upon the current language.
dnl (This may just be $CC/$CXX/$F77, but is more often something like
dnl mpicc/mpiCC/mpif77.)  It also sets MPILIBS to any libraries that are
dnl needed for linking MPI (e.g. -lmpi, if a special MPICC/MPICXX/MPIF77
dnl was not found).
dnl
dnl If you want to compile everything with MPI, you should set:
dnl
dnl     CC="$MPICC" #OR# CXX="$MPICXX" #OR# F77="$MPIF77"
dnl     LIBS="$MPILIBS $LIBS"
dnl
dnl The user can force a particular library/compiler by setting the
dnl MPICC/MPICXX/MPIF77 and/or MPILIBS environment variables.
dnl
dnl ACTION-IF-FOUND is a list of shell commands to run if an MPI
dnl library is found, and ACTION-IF-NOT-FOUND is a list of commands
dnl to run it if it is not found.  If ACTION-IF-FOUND is not specified,
dnl the default action will define HAVE_MPI.
dnl
dnl @version $Id$
dnl @author Steven G. Johnson <stevenj@alum.mit.edu>

AC_DEFUN([ACX_MPI], [
AC_PREREQ(2.50) dnl for AC_LANG_CASE

AC_LANG_CASE([C], [
	AC_REQUIRE([AC_PROG_CC])
	AC_ARG_VAR(MPICC,[MPI C compiler command])
	AC_CHECK_PROGS(MPICC, mpicc hcc mpcc mpcc_r mpxlc, $CC)
	acx_mpi_save_CC="$CC"
	CC="$MPICC"
	AC_SUBST(MPICC)
],
[C++], [
	AC_REQUIRE([AC_PROG_CXX])
	AC_ARG_VAR(MPICXX,[MPI C++ compiler command])
	AC_CHECK_PROGS(MPICXX, mpiCC mpCC, $CXX)
	acx_mpi_save_CXX="$CXX"
	CXX="$MPICXX"
	AC_SUBST(MPICXX)
],
[Fortran 77], [
	AC_REQUIRE([AC_PROG_F77])
	AC_ARG_VAR(MPIF77,[MPI Fortran compiler command])
	AC_CHECK_PROGS(MPIF77, mpif77 hf77 mpxlf mpf77 mpif90 mpf90 mpxlf90 mpxlf95 mpxlf_r, $F77)
	acx_mpi_save_F77="$F77"
	F77="$MPIF77"
	AC_SUBST(MPIF77)
])

if test x = x"$MPILIBS"; then
	AC_LANG_CASE([C], [AC_CHECK_FUNC(MPI_Init, [MPILIBS=" "])],
		[C++], [AC_CHECK_FUNC(MPI_Init, [MPILIBS=" "])],
		[Fortran 77], [AC_MSG_CHECKING([for MPI_Init])
			AC_TRY_LINK([],[      call MPI_Init], [MPILIBS=" "
				AC_MSG_RESULT(yes)], [AC_MSG_RESULT(no)])])
fi
if test x = x"$MPILIBS"; then
	AC_CHECK_LIB(mpi, MPI_Init, [MPILIBS="-lmpi"])
fi
if test x = x"$MPILIBS"; then
	AC_CHECK_LIB(mpich, MPI_Init, [MPILIBS="-lmpich"])
fi

dnl We have to use AC_TRY_COMPILE and not AC_CHECK_HEADER because the
dnl latter uses $CPP, not $CC (which may be mpicc).
AC_LANG_CASE([C], [if test x != x"$MPILIBS"; then
	AC_MSG_CHECKING([for mpi.h])
	AC_TRY_COMPILE([#include <mpi.h>],[],[AC_MSG_RESULT(yes)], [MPILIBS=""
		AC_MSG_RESULT(no)])
fi],
[C++], [if test x != x"$MPILIBS"; then
	AC_MSG_CHECKING([for mpi.h])
	AC_TRY_COMPILE([#include <mpi.h>],[],[AC_MSG_RESULT(yes)], [MPILIBS=""
		AC_MSG_RESULT(no)])
fi])

AC_LANG_CASE([C], [CC="$acx_mpi_save_CC"],
	[C++], [CXX="$acx_mpi_save_CXX"],
	[Fortran 77], [F77="$acx_mpi_save_F77"])

AC_SUBST(MPILIBS)

# Finally, execute ACTION-IF-FOUND/ACTION-IF-NOT-FOUND:
if test x = x"$MPILIBS"; then
        $2
        :
else
        ifelse([$1],,[AC_DEFINE(HAVE_MPI,1,[Define if you have the MPI library.])],[$1])
        :
fi
])dnl ACX_MPI

# Macros that test various C library quirks
# $PostgreSQL: pgsql/config/c-library.m4,v 1.29 2004/12/16 17:48:26 momjian Exp $


# PGAC_VAR_INT_TIMEZONE
# ---------------------
# Check if the global variable `timezone' exists. If so, define
# HAVE_INT_TIMEZONE.
AC_DEFUN([PGAC_VAR_INT_TIMEZONE],
[AC_CACHE_CHECK(for int timezone, pgac_cv_var_int_timezone,
[AC_TRY_LINK([#include <time.h>
int res;],
  [#ifndef __CYGWIN__
res = timezone / 60;
#else
res = _timezone / 60;
#endif],
  [pgac_cv_var_int_timezone=yes],
  [pgac_cv_var_int_timezone=no])])
if test x"$pgac_cv_var_int_timezone" = xyes ; then
  AC_DEFINE(HAVE_INT_TIMEZONE,, [Define to 1 if you have the global variable 'int timezone'.])
fi])# PGAC_VAR_INT_TIMEZONE


# PGAC_STRUCT_TIMEZONE
# ------------------
# Figure out how to get the current timezone.  If `struct tm' has a
# `tm_zone' member, define `HAVE_TM_ZONE'.  Also, if the
# external array `tzname' is found, define `HAVE_TZNAME'.
# This is the same as the standard macro AC_STRUCT_TIMEZONE, except that
# tzname[] is checked for regardless of whether we find tm_zone.
AC_DEFUN([PGAC_STRUCT_TIMEZONE],
[AC_REQUIRE([AC_STRUCT_TM])dnl
AC_CHECK_MEMBERS([struct tm.tm_zone],,,[#include <sys/types.h>
#include <$ac_cv_struct_tm>
])
if test "$ac_cv_member_struct_tm_tm_zone" = yes; then
  AC_DEFINE(HAVE_TM_ZONE, 1,
            [Define to 1 if your `struct tm' has `tm_zone'. Deprecated, use
             `HAVE_STRUCT_TM_TM_ZONE' instead.])
fi
AC_CACHE_CHECK(for tzname, ac_cv_var_tzname,
[AC_TRY_LINK(
[#include <time.h>
#ifndef tzname /* For SGI.  */
extern char *tzname[]; /* RS6000 and others reject char **tzname.  */
#endif
],
[atoi(*tzname);], ac_cv_var_tzname=yes, ac_cv_var_tzname=no)])
if test $ac_cv_var_tzname = yes; then
    AC_DEFINE(HAVE_TZNAME, 1,
              [Define to 1 if you have the external array `tzname'.])
fi
])# PGAC_STRUCT_TIMEZONE


# PGAC_FUNC_GETTIMEOFDAY_1ARG
# ---------------------------
# Check if gettimeofday() has only one arguments. (Normal is two.)
# If so, define GETTIMEOFDAY_1ARG.
AC_DEFUN([PGAC_FUNC_GETTIMEOFDAY_1ARG],
[AC_CACHE_CHECK(whether gettimeofday takes only one argument,
pgac_cv_func_gettimeofday_1arg,
[AC_TRY_COMPILE([#include <sys/time.h>],
[struct timeval *tp;
struct timezone *tzp;
gettimeofday(tp,tzp);],
[pgac_cv_func_gettimeofday_1arg=no],
[pgac_cv_func_gettimeofday_1arg=yes])])
if test x"$pgac_cv_func_gettimeofday_1arg" = xyes ; then
  AC_DEFINE(GETTIMEOFDAY_1ARG,, [Define to 1 if gettimeofday() takes only 1 argument.])
fi
AH_VERBATIM(GETTIMEOFDAY_1ARG_,
[@%:@ifdef GETTIMEOFDAY_1ARG
@%:@ define gettimeofday(a,b) gettimeofday(a)
@%:@endif])dnl
])# PGAC_FUNC_GETTIMEOFDAY_1ARG


# PGAC_FUNC_GETPWUID_R_5ARG
# ---------------------------
# Check if getpwuid_r() takes a fifth argument (later POSIX standard, not draft version)
# If so, define GETPWUID_R_5ARG
AC_DEFUN([PGAC_FUNC_GETPWUID_R_5ARG],
[AC_CACHE_CHECK(whether getpwuid_r takes a fifth argument,
pgac_func_getpwuid_r_5arg,
[AC_TRY_COMPILE([#include <sys/types.h>
#include <pwd.h>],
[uid_t uid;
struct passwd *space;
char *buf;
size_t bufsize;
struct passwd **result;
getpwuid_r(uid, space, buf, bufsize, result);],
[pgac_func_getpwuid_r_5arg=yes],
[pgac_func_getpwuid_r_5arg=no])])
if test x"$pgac_func_getpwuid_r_5arg" = xyes ; then
  AC_DEFINE(GETPWUID_R_5ARG,, [Define to 1 if getpwuid_r() takes a 5th argument.])
fi
])# PGAC_FUNC_GETPWUID_R_5ARG


# PGAC_FUNC_STRERROR_R_INT
# ---------------------------
# Check if strerror_r() returns an int (SUSv3) rather than a char * (GNU libc)
# If so, define STRERROR_R_INT
AC_DEFUN([PGAC_FUNC_STRERROR_R_INT],
[AC_CACHE_CHECK(whether strerror_r returns int,
pgac_func_strerror_r_int,
[AC_TRY_COMPILE([#include <string.h>],
[#ifndef _AIX
int strerror_r(int, char *, size_t);
#else
/* Older AIX has 'int' for the third argument so we don't test the args. */
int strerror_r();
#endif],
[pgac_func_strerror_r_int=yes],
[pgac_func_strerror_r_int=no])])
if test x"$pgac_func_strerror_r_int" = xyes ; then
  AC_DEFINE(STRERROR_R_INT,, [Define to 1 if strerror_r() returns a int.])
fi
])# PGAC_FUNC_STRERROR_R_INT


# PGAC_UNION_SEMUN
# ----------------
# Check if `union semun' exists. Define HAVE_UNION_SEMUN if so.
# If it doesn't then one could define it as
# union semun { int val; struct semid_ds *buf; unsigned short *array; }
AC_DEFUN([PGAC_UNION_SEMUN],
[AC_CHECK_TYPES([union semun], [], [],
[#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>])])# PGAC_UNION_SEMUN


# PGAC_STRUCT_SOCKADDR_UN
# -----------------------
# If `struct sockaddr_un' exists, define HAVE_UNIX_SOCKETS.
# (Requires test for <sys/un.h>!)
AC_DEFUN([PGAC_STRUCT_SOCKADDR_UN],
[AC_CHECK_TYPES([struct sockaddr_un], [AC_DEFINE(HAVE_UNIX_SOCKETS, 1, [Define to 1 if you have unix sockets.])], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_UN


# PGAC_STRUCT_SOCKADDR_STORAGE
# ----------------------------
# If `struct sockaddr_storage' exists, define HAVE_STRUCT_SOCKADDR_STORAGE.
# If it is missing then one could define it.
AC_DEFUN([PGAC_STRUCT_SOCKADDR_STORAGE],
[AC_CHECK_TYPES([struct sockaddr_storage], [], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_STORAGE

# PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS
# --------------------------------------
# Check the members of `struct sockaddr_storage'.  We need to know about
# ss_family and ss_len.  (Some platforms follow RFC 2553 and call them
# __ss_family and __ss_len.)  We also check struct sockaddr's sa_len;
# if we have to define our own `struct sockaddr_storage', this tells us
# whether we need to provide an ss_len field.
AC_DEFUN([PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS],
[AC_CHECK_MEMBERS([struct sockaddr_storage.ss_family,
		   struct sockaddr_storage.__ss_family,
		   struct sockaddr_storage.ss_len,
		   struct sockaddr_storage.__ss_len,
		   struct sockaddr.sa_len], [], [],
[#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
])])# PGAC_STRUCT_SOCKADDR_STORAGE_MEMBERS


# PGAC_STRUCT_ADDRINFO
# -----------------------
# If `struct addrinfo' exists, define HAVE_STRUCT_ADDRINFO.
AC_DEFUN([PGAC_STRUCT_ADDRINFO],
[AC_CHECK_TYPES([struct addrinfo], [], [],
[#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
])])# PGAC_STRUCT_ADDRINFO


# PGAC_FUNC_POSIX_SIGNALS
# -----------------------
# Check to see if the machine has the POSIX signal interface. Define
# HAVE_POSIX_SIGNALS if so. Also set the output variable HAVE_POSIX_SIGNALS
# to yes or no.
#
# Note that this test only compiles a test program, it doesn't check
# whether the routines actually work. If that becomes a problem, make
# a fancier check.
AC_DEFUN([PGAC_FUNC_POSIX_SIGNALS],
[AC_CACHE_CHECK(for POSIX signal interface, pgac_cv_func_posix_signals,
[AC_TRY_LINK([#include <signal.h>
],
[struct sigaction act, oact;
sigemptyset(&act.sa_mask);
act.sa_flags = SA_RESTART;
sigaction(0, &act, &oact);],
[pgac_cv_func_posix_signals=yes],
[pgac_cv_func_posix_signals=no])])
if test x"$pgac_cv_func_posix_signals" = xyes ; then
  AC_DEFINE(HAVE_POSIX_SIGNALS,, [Define to 1 if you have the POSIX signal interface.])
fi
HAVE_POSIX_SIGNALS=$pgac_cv_func_posix_signals
AC_SUBST(HAVE_POSIX_SIGNALS)])# PGAC_FUNC_POSIX_SIGNALS


# PGAC_FUNC_SNPRINTF_LONG_LONG_INT_FORMAT
# ---------------------------------------
# Determine which format snprintf uses for long long int.  We handle
# %lld, %qd, %I64d.  The result is in shell variable
# LONG_LONG_INT_FORMAT.
#
# MinGW uses '%I64d', though gcc throws an warning with -Wall,
# while '%lld' doesn't generate a warning, but doesn't work.
#
AC_DEFUN([PGAC_FUNC_SNPRINTF_LONG_LONG_INT_FORMAT],
[AC_MSG_CHECKING([snprintf format for long long int])
AC_CACHE_VAL(pgac_cv_snprintf_long_long_int_format,
[for pgac_format in '%lld' '%qd' '%I64d'; do
AC_TRY_RUN([#include <stdio.h>
typedef long long int ac_int64;
#define INT64_FORMAT "$pgac_format"

ac_int64 a = 20000001;
ac_int64 b = 40000005;

int does_int64_snprintf_work()
{
  ac_int64 c;
  char buf[100];

  if (sizeof(ac_int64) != 8)
    return 0;			/* doesn't look like the right size */

  c = a * b;
  snprintf(buf, 100, INT64_FORMAT, c);
  if (strcmp(buf, "800000140000005") != 0)
    return 0;			/* either multiply or snprintf is busted */
  return 1;
}
main() {
  exit(! does_int64_snprintf_work());
}],
[pgac_cv_snprintf_long_long_int_format=$pgac_format; break],
[],
[pgac_cv_snprintf_long_long_int_format=cross; break])
done])dnl AC_CACHE_VAL

LONG_LONG_INT_FORMAT=''

case $pgac_cv_snprintf_long_long_int_format in
  cross) AC_MSG_RESULT([cannot test (not on host machine)]);;
  ?*)    AC_MSG_RESULT([$pgac_cv_snprintf_long_long_int_format])
         LONG_LONG_INT_FORMAT=$pgac_cv_snprintf_long_long_int_format;;
  *)     AC_MSG_RESULT(none);;
esac])# PGAC_FUNC_SNPRINTF_LONG_LONG_INT_FORMAT

# $PostgreSQL: pgsql/config/ac_func_accept_argtypes.m4,v 1.6 2003/11/29 19:51:17 pgsql Exp $
# This comes from the official Autoconf macro archive at
# <http://research.cys.de/autoconf-archive/>
# (I removed the $ before the Id CVS keyword below.)


dnl @synopsis AC_FUNC_ACCEPT_ARGTYPES
dnl
dnl Checks the data types of the three arguments to accept(). Results are
dnl placed into the symbols ACCEPT_TYPE_RETURN and ACCEPT_TYPE_ARG[123], 
dnl consistent with the following example:
dnl
dnl       #define ACCEPT_TYPE_RETURN int
dnl       #define ACCEPT_TYPE_ARG1 int
dnl       #define ACCEPT_TYPE_ARG2 struct sockaddr *
dnl       #define ACCEPT_TYPE_ARG3 socklen_t
dnl
dnl This macro requires AC_CHECK_HEADERS to have already verified the
dnl presence or absence of sys/types.h and sys/socket.h.
dnl
dnl NOTE: This is just a modified version of the AC_FUNC_SELECT_ARGTYPES
dnl macro. Credit for that one goes to David MacKenzie et. al.
dnl
dnl @version Id: ac_func_accept_argtypes.m4,v 1.1 1999/12/03 11:29:29 simons Exp $
dnl @author Daniel Richard G. <skunk@mit.edu>
dnl

# PostgreSQL local changes: In the original version ACCEPT_TYPE_ARG3
# is a pointer type. That's kind of useless because then you can't
# use the macro to define a corresponding variable. We also make the
# reasonable(?) assumption that you can use arg3 for getsocktype etc.
# as well (i.e., anywhere POSIX.2 has socklen_t).
#
# arg2 can also be `const' (e.g., RH 4.2). Change the order of tests
# for arg3 so that `int' is first, in case there is no prototype at all.
#
# Solaris 7 and 8 have arg3 as 'void *' (disguised as 'Psocklen_t'
# which is *not* 'socklen_t *').  If we detect that, then we assume
# 'int' as the result, because that ought to work best.
#
# On Win32, accept() returns 'unsigned int PASCAL' 

AC_DEFUN([AC_FUNC_ACCEPT_ARGTYPES],
[AC_MSG_CHECKING([types of arguments for accept()])
 AC_CACHE_VAL(ac_cv_func_accept_return,dnl
 [AC_CACHE_VAL(ac_cv_func_accept_arg1,dnl
  [AC_CACHE_VAL(ac_cv_func_accept_arg2,dnl
   [AC_CACHE_VAL(ac_cv_func_accept_arg3,dnl
    [for ac_cv_func_accept_return in 'int' 'unsigned int PASCAL'; do
      for ac_cv_func_accept_arg1 in 'int' 'unsigned int'; do
       for ac_cv_func_accept_arg2 in 'struct sockaddr *' 'const struct sockaddr *' 'void *'; do
        for ac_cv_func_accept_arg3 in 'int' 'size_t' 'socklen_t' 'unsigned int' 'void'; do
         AC_TRY_COMPILE(
[#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
extern $ac_cv_func_accept_return accept ($ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *);],
         [], [ac_not_found=no; break 4], [ac_not_found=yes])
       done
      done
     done
    done
    if test "$ac_not_found" = yes; then
      AC_MSG_ERROR([could not determine argument types])
    fi
    if test "$ac_cv_func_accept_arg3" = "void"; then
      ac_cv_func_accept_arg3=int
    fi
    ])dnl AC_CACHE_VAL
   ])dnl AC_CACHE_VAL
  ])dnl AC_CACHE_VAL
 ])dnl AC_CACHE_VAL
 AC_MSG_RESULT([$ac_cv_func_accept_return, $ac_cv_func_accept_arg1, $ac_cv_func_accept_arg2, $ac_cv_func_accept_arg3 *])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_RETURN, $ac_cv_func_accept_return,
                    [Define to the return type of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG1, $ac_cv_func_accept_arg1,
                    [Define to the type of arg 1 of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG2, $ac_cv_func_accept_arg2,
                    [Define to the type of arg 2 of 'accept'])
 AC_DEFINE_UNQUOTED(ACCEPT_TYPE_ARG3, $ac_cv_func_accept_arg3,
                    [Define to the type of arg 3 of 'accept'])
])

dnl PGSGL:  When updating, comment out port-specific part below;
dnl see the comment below with the word "PostgreSQL".
dnl
dnl Available from the GNU Autoconf Macro Archive at:
dnl http://www.gnu.org/software/ac-archive/htmldoc/acx_pthread.html
dnl
AC_DEFUN([ACX_PTHREAD], [
AC_REQUIRE([AC_CANONICAL_HOST])
AC_LANG_SAVE
AC_LANG_C
acx_pthread_ok=no

# We used to check for pthread.h first, but this fails if pthread.h
# requires special compiler flags (e.g. on True64 or Sequent).
# It gets checked for in the link test anyway.

# First of all, check if the user has set any of the PTHREAD_LIBS,
# etcetera environment variables, and if threads linking works using
# them:
if test x"$PTHREAD_LIBS$PTHREAD_CFLAGS" != x; then
        save_CFLAGS="$CFLAGS"
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
        save_LIBS="$LIBS"
        LIBS="$PTHREAD_LIBS $LIBS"
        AC_MSG_CHECKING([for pthread_join in LIBS=$PTHREAD_LIBS with CFLAGS=$PTHREAD_CFLAGS])
        AC_TRY_LINK_FUNC(pthread_join, acx_pthread_ok=yes)
        AC_MSG_RESULT($acx_pthread_ok)
        if test x"$acx_pthread_ok" = xno; then
                PTHREAD_LIBS=""
                PTHREAD_CFLAGS=""
        fi
        LIBS="$save_LIBS"
        CFLAGS="$save_CFLAGS"
fi

# We must check for the threads library under a number of different
# names; the ordering is very important because some systems
# (e.g. DEC) have both -lpthread and -lpthreads, where one of the
# libraries is broken (non-POSIX).

# Create a list of thread flags to try.  Items starting with a "-" are
# C compiler flags, and other items are library names, except for "none"
# which indicates that we try without any flags at all, and "pthread-config"
# which is a program returning the flags for the Pth emulation library.

acx_pthread_flags="pthreads none -Kthread -kthread lthread -pthread -pthreads -mthreads pthread --thread-safe -mt pthread-config"

# The ordering *is* (sometimes) important.  Some notes on the
# individual items follow:

# pthreads: AIX (must check this before -lpthread)
# none: in case threads are in libc; should be tried before -Kthread and
#       other compiler flags to prevent continual compiler warnings
# -Kthread: Sequent (threads in libc, but -Kthread needed for pthread.h)
# -kthread: FreeBSD kernel threads (preferred to -pthread since SMP-able)
# lthread: LinuxThreads port on FreeBSD (also preferred to -pthread)
# -pthread: Linux/gcc (kernel threads), BSD/gcc (userland threads)
# -pthreads: Solaris/gcc
# -mthreads: Mingw32/gcc, Lynx/gcc
# -mt: Sun Workshop C (may only link SunOS threads [-lthread], but it
#      doesn't hurt to check since this sometimes defines pthreads too;
#      also defines -D_REENTRANT)
# pthread: Linux, etcetera
# --thread-safe: KAI C++
# pthread-config: use pthread-config program (for GNU Pth library)

case "${host_cpu}-${host_os}" in
        *solaris*)

        # On Solaris (at least, for some versions), libc contains stubbed
        # (non-functional) versions of the pthreads routines, so link-based
        # tests will erroneously succeed.  (We need to link with -pthread or
        # -lpthread.)  (The stubs are missing pthread_cleanup_push, or rather
        # a function called by this macro, so we could check for that, but
        # who knows whether they'll stub that too in a future libc.)  So,
        # we'll just look for -pthreads and -lpthread first:

        acx_pthread_flags="-pthread -pthreads pthread -mt $acx_pthread_flags"
        ;;
esac

if test x"$acx_pthread_ok" = xno; then
for flag in $acx_pthread_flags; do

        tryPTHREAD_CFLAGS=""
        tryPTHREAD_LIBS=""
        case $flag in
                none)
                AC_MSG_CHECKING([whether pthreads work without any flags])
                ;;

                -*)
                AC_MSG_CHECKING([whether pthreads work with $flag])
                tryPTHREAD_CFLAGS="$flag"
                ;;

                pthread-config)
                # skip this if we already have flags defined, for PostgreSQL
                if test x"$PTHREAD_CFLAGS" != x -o x"$PTHREAD_LIBS" != x; then continue; fi
                AC_CHECK_PROG(acx_pthread_config, pthread-config, yes, no)
                if test x"$acx_pthread_config" = xno; then continue; fi
                tryPTHREAD_CFLAGS="`pthread-config --cflags`"
                tryPTHREAD_LIBS="`pthread-config --ldflags` `pthread-config --libs`"
                ;;

                *)
                AC_MSG_CHECKING([for the pthreads library -l$flag])
                tryPTHREAD_LIBS="-l$flag"
                ;;
        esac

        save_LIBS="$LIBS"
        save_CFLAGS="$CFLAGS"
        LIBS="$tryPTHREAD_LIBS $PTHREAD_LIBS $LIBS"
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS $tryPTHREAD_CFLAGS"

        # Check for various functions.  We must include pthread.h,
        # since some functions may be macros.  (On the Sequent, we
        # need a special flag -Kthread to make this header compile.)
        # We check for pthread_join because it is in -lpthread on IRIX
        # while pthread_create is in libc.  We check for pthread_attr_init
        # due to DEC craziness with -lpthreads.  We check for
        # pthread_cleanup_push because it is one of the few pthread
        # functions on Solaris that doesn't have a non-functional libc stub.
        # We try pthread_create on general principles.
        AC_TRY_LINK([#include <pthread.h>],
                    [pthread_t th; pthread_join(th, 0);
                     pthread_attr_init(0); pthread_cleanup_push(0, 0);
                     pthread_create(0,0,0,0); pthread_cleanup_pop(0); ],
                    [acx_pthread_ok=yes], [acx_pthread_ok=no])

        if test "x$acx_pthread_ok" = xyes; then
            # Don't use options that are ignored by the compiler.
            # We find them by checking stderror.
            cat >conftest.$ac_ext <<_ACEOF
int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;
  return 0;
}
_ACEOF
            rm -f conftest.$ac_objext conftest$ac_exeext
            if test "`(eval $ac_link 2>&1 1>&5)`" = ""; then
                # we continue with more flags because Linux needs -lpthread
                # for libpq builds on PostgreSQL.  The test above only
                # tests for building binaries, not shared libraries.
                PTHREAD_LIBS=" $tryPTHREAD_LIBS $PTHREAD_LIBS"
                PTHREAD_CFLAGS="$PTHREAD_CFLAGS $tryPTHREAD_CFLAGS"
            else   acx_pthread_ok=no
            fi
        fi

        LIBS="$save_LIBS"
        CFLAGS="$save_CFLAGS"

        AC_MSG_RESULT($acx_pthread_ok)
done
fi

# Various other checks:
if test "x$acx_pthread_ok" = xyes; then
        save_LIBS="$LIBS"
        LIBS="$PTHREAD_LIBS $LIBS"
        save_CFLAGS="$CFLAGS"
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS"

        # Detect AIX lossage: threads are created detached by default
        # and the JOINABLE attribute has a nonstandard name (UNDETACHED).
        AC_MSG_CHECKING([for joinable pthread attribute])
        AC_TRY_LINK([#include <pthread.h>],
                    [int attr=PTHREAD_CREATE_JOINABLE;],
                    ok=PTHREAD_CREATE_JOINABLE, ok=unknown)
        if test x"$ok" = xunknown; then
                AC_TRY_LINK([#include <pthread.h>],
                            [int attr=PTHREAD_CREATE_UNDETACHED;],
                            ok=PTHREAD_CREATE_UNDETACHED, ok=unknown)
        fi
        if test x"$ok" != xPTHREAD_CREATE_JOINABLE; then
                AC_DEFINE(PTHREAD_CREATE_JOINABLE, $ok,
                          [Define to the necessary symbol if this constant
                           uses a non-standard name on your system.])
        fi
        AC_MSG_RESULT(${ok})
        if test x"$ok" = xunknown; then
                AC_MSG_WARN([we do not know how to create joinable pthreads])
        fi

        AC_MSG_CHECKING([if more special flags are required for pthreads])
        flag=no
# We always add these in PostgreSQL
#       case "${host_cpu}-${host_os}" in
#               *-aix* | *-freebsd* | *-darwin*) flag="-D_THREAD_SAFE";;
#               *solaris* | *-osf* | *-hpux*) flag="-D_REENTRANT";;
#       esac
        AC_MSG_RESULT(${flag})
        if test "x$flag" != xno; then
                PTHREAD_CFLAGS="$flag $PTHREAD_CFLAGS"
        fi

        LIBS="$save_LIBS"
        CFLAGS="$save_CFLAGS"

# Supporting cc_r would require a special CC in all places that
# use libpq, and that is ugly, so we don't do it.  Users can still
# define their compiler as cc_r to do thread builds of everything.
        # More AIX lossage: must compile with cc_r
        AC_CHECK_PROG(PTHREAD_CC, cc_r, cc_r, ${CC})
else
        PTHREAD_CC="$CC"
fi

AC_SUBST(PTHREAD_LIBS)
AC_SUBST(PTHREAD_CFLAGS)
AC_SUBST(PTHREAD_CC)

# Finally, execute ACTION-IF-FOUND/ACTION-IF-NOT-FOUND:
if test x"$acx_pthread_ok" = xyes; then
        ifelse([$1],,AC_DEFINE(HAVE_PTHREAD,1,[Define if you have POSIX threads libraries and header files.]),[$1])
        :
else
        acx_pthread_ok=no
        $2
fi
AC_LANG_RESTORE
])dnl ACX_PTHREAD

# $PostgreSQL: pgsql/config/tcl.m4,v 1.5 2003/11/29 19:51:17 pgsql Exp $

# Autoconf macros to check for Tcl related things


AC_DEFUN([PGAC_PATH_TCLSH],
         [AC_PATH_PROGS(TCLSH, [tclsh tcl])])


# PGAC_PATH_TCLCONFIGSH([SEARCH-PATH])
# ------------------------------------
AC_DEFUN([PGAC_PATH_TCLCONFIGSH],
[AC_REQUIRE([PGAC_PATH_TCLSH])[]dnl
AC_BEFORE([$0], [PGAC_PATH_TKCONFIGSH])[]dnl
AC_MSG_CHECKING([for tclConfig.sh])
# Let user override test
if test -z "$TCL_CONFIG_SH"; then
    pgac_test_dirs="$1"

    set X $pgac_test_dirs; shift
    if test $[#] -eq 0; then
        test -z "$TCLSH" && AC_MSG_ERROR([unable to locate tclConfig.sh because no Tcl shell was found])
        set X `echo 'puts $auto_path' | $TCLSH`; shift
    fi

    for pgac_dir do
        if test -r "$pgac_dir/tclConfig.sh"; then
            TCL_CONFIG_SH=$pgac_dir/tclConfig.sh
            break
        fi
    done
fi

if test -z "$TCL_CONFIG_SH"; then
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([file 'tclConfig.sh' is required for Tcl])
else
    AC_MSG_RESULT([$TCL_CONFIG_SH])
fi

AC_SUBST([TCL_CONFIG_SH])
])# PGAC_PATH_TCLCONFIGSH


# PGAC_PATH_TKCONFIGSH([SEARCH-PATH])
# ------------------------------------
AC_DEFUN([PGAC_PATH_TKCONFIGSH],
[AC_REQUIRE([PGAC_PATH_TCLSH])[]dnl
AC_MSG_CHECKING([for tkConfig.sh])
# Let user override test
if test -z "$TK_CONFIG_SH"; then
    pgac_test_dirs="$1"

    set X $pgac_test_dirs; shift
    if test $[#] -eq 0; then
        test -z "$TCLSH" && AC_MSG_ERROR([unable to locate tkConfig.sh because no Tcl shell was found])
        set X `echo 'puts $auto_path' | $TCLSH`; shift
    fi

    for pgac_dir do
        if test -r "$pgac_dir/tkConfig.sh"; then
            TK_CONFIG_SH=$pgac_dir/tkConfig.sh
            break
        fi
    done
fi

if test -z "$TK_CONFIG_SH"; then
    AC_MSG_RESULT(no)
    AC_MSG_ERROR([file 'tkConfig.sh' is required for Tk])
else
    AC_MSG_RESULT([$TK_CONFIG_SH])
fi

AC_SUBST([TK_CONFIG_SH])
])# PGAC_PATH_TKCONFIGSH


# PGAC_EVAL_TCLCONFIGSH(FILE, WANTED-VARS)
# ----------------------------------------
# Assigns variables listed in WANTED-VARS by reading FILE and
# evaluating it according to the quoting scheme of tclConfig.sh and
# tkConfig.sh.  Calls AC_SUBST for each variable.

AC_DEFUN([PGAC_EVAL_TCLCONFIGSH],
[. "$1"
m4_foreach([pgac_item], [$2],
[eval pgac_item=\"[$]pgac_item\"
AC_SUBST(pgac_item)])])

# $PostgreSQL: pgsql/config/docbook.m4,v 1.7 2003/12/13 20:25:18 momjian Exp $

# PGAC_PROG_JADE
# --------------
AC_DEFUN([PGAC_PROG_JADE],
[AC_CHECK_PROGS([JADE], [openjade jade])])


# PGAC_PROG_NSGMLS
# ----------------
AC_DEFUN([PGAC_PROG_NSGMLS],
[AC_CHECK_PROGS([NSGMLS], [onsgmls nsgmls])])


# PGAC_CHECK_DOCBOOK(VERSION)
# ---------------------------
AC_DEFUN([PGAC_CHECK_DOCBOOK],
[AC_REQUIRE([PGAC_PROG_NSGMLS])
AC_CACHE_CHECK([for DocBook V$1], [pgac_cv_check_docbook],
[cat >conftest.sgml <<EOF
<!doctype book PUBLIC "-//OASIS//DTD DocBook V$1//EN">
<book>
 <title>test</title>
 <chapter>
  <title>random</title>
   <sect1>
    <title>testsect</title>
    <para>text</para>
  </sect1>
 </chapter>
</book>
EOF

pgac_cv_check_docbook=no

if test -n "$NSGMLS"; then
  $NSGMLS -s conftest.sgml 1>&AS_MESSAGE_LOG_FD 2>&1
  if test $? -eq 0; then
    pgac_cv_check_docbook=yes
  fi
fi
rm -f conftest.sgml])

have_docbook=$pgac_cv_check_docbook
AC_SUBST([have_docbook])
])# PGAC_CHECK_DOCBOOK


# PGAC_PATH_DOCBOOK_STYLESHEETS
# -----------------------------
AC_DEFUN([PGAC_PATH_DOCBOOK_STYLESHEETS],
[AC_ARG_VAR(DOCBOOKSTYLE, [location of DocBook stylesheets])dnl
AC_MSG_CHECKING([for DocBook stylesheets])
AC_CACHE_VAL([pgac_cv_path_stylesheets],
[if test -n "$DOCBOOKSTYLE"; then
  pgac_cv_path_stylesheets=$DOCBOOKSTYLE
else
  for pgac_prefix in /usr /usr/local /opt; do
    for pgac_infix in share lib; do
      for pgac_postfix in \
        sgml/stylesheets/nwalsh-modular \
        sgml/stylesheets/docbook \
		sgml/docbook-dsssl \
        sgml/docbook/dsssl/modular \
        sgml/docbook/stylesheet/dsssl/modular \
        sgml/docbook/dsssl-stylesheets
      do
        pgac_candidate=$pgac_prefix/$pgac_infix/$pgac_postfix
        if test -r "$pgac_candidate/html/docbook.dsl" \
           && test -r "$pgac_candidate/print/docbook.dsl"
        then
          pgac_cv_path_stylesheets=$pgac_candidate
          break 3
        fi
      done
    done
  done
fi])
DOCBOOKSTYLE=$pgac_cv_path_stylesheets
AC_SUBST([DOCBOOKSTYLE])
if test -n "$DOCBOOKSTYLE"; then
  AC_MSG_RESULT([$DOCBOOKSTYLE])
else
  AC_MSG_RESULT(no)
fi])# PGAC_PATH_DOCBOOK_STYLESHEETS


# PGAC_PATH_COLLATEINDEX
# ----------------------
AC_DEFUN([PGAC_PATH_COLLATEINDEX],
[AC_REQUIRE([PGAC_PATH_DOCBOOK_STYLESHEETS])dnl
if test -n "$DOCBOOKSTYLE"; then
  AC_PATH_PROGS(COLLATEINDEX, collateindex.pl, [],
                [$DOCBOOKSTYLE/bin $PATH])
else
  AC_PATH_PROGS(COLLATEINDEX, collateindex.pl)
fi])# PGAC_PATH_COLLATEINDEX

