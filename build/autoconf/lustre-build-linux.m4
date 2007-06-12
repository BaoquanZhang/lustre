#
# LB_LINUX_VERSION
#
# Set things accordingly for a 2.5 kernel
#
AC_DEFUN([LB_LINUX_VERSION],
[LB_CHECK_FILE([$LINUX/include/linux/namei.h],
	[
        	linux25="yes"
		KMODEXT=".ko"
	],[
		KMODEXT=".o"
		linux25="no"
	])
AC_MSG_CHECKING([if you are using Linux 2.6])
AC_MSG_RESULT([$linux25])

MODULE_TARGET="SUBDIRS"
if test $linux25 = "yes" ; then
	makerule="$PWD/build"
	AC_MSG_CHECKING([for external module build support])
	rm -f build/conftest.i
	LB_LINUX_TRY_MAKE([],[],
		[$makerule LUSTRE_KERNEL_TEST=conftest.i],
		[test -s build/conftest.i],
		[
			AC_MSG_RESULT([no])
		],[
			makerule="_module_$makerule"
			MODULE_TARGET="M"
			LB_LINUX_TRY_MAKE([],[],
				[$makerule LUSTRE_KERNEL_TEST=conftest.i],
				[test -s build/conftest.i],
				[
					AC_MSG_RESULT([yes])
				],[
					AC_MSG_ERROR([unknown; check config.log for details])
				])
		])
else
	makerule="_dir_$PWD/build"
fi

AC_SUBST(MODULE_TARGET)
AC_SUBST(linux25)
AC_SUBST(KMODEXT)
])

#
# LB_LINUX_RELEASE
#
# get the release version of linux
#
AC_DEFUN([LB_LINUX_RELEASE],
[LINUXRELEASE=
rm -f build/conftest.i
AC_MSG_CHECKING([for Linux release])
if test -s $LINUX_OBJ/include/linux/utsrelease.h ; then
	LINUXRELEASEHEADER=utsrelease.h
else
	LINUXRELEASEHEADER=version.h
fi
LB_LINUX_TRY_MAKE([
	#include <linux/$LINUXRELEASEHEADER>
],[
	char *LINUXRELEASE;
	LINUXRELEASE=UTS_RELEASE;
],[
	$makerule LUSTRE_KERNEL_TEST=conftest.i
],[
	test -s build/conftest.i
],[
	# LINUXRELEASE="UTS_RELEASE"
	eval $(grep "LINUXRELEASE=" build/conftest.i)
],[
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not preprocess test program.  Consult config.log for details.])
])
rm -f build/conftest.i
if test x$LINUXRELEASE = x ; then
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not determine Linux release version from linux/version.h.])
fi
AC_MSG_RESULT([$LINUXRELEASE])
AC_SUBST(LINUXRELEASE)

moduledir='/lib/modules/'$LINUXRELEASE/kernel
AC_SUBST(moduledir)

modulefsdir='$(moduledir)/fs/$(PACKAGE)'
AC_SUBST(modulefsdir)

modulenetdir='$(moduledir)/net/$(PACKAGE)'
AC_SUBST(modulenetdir)

# ------------ RELEASE --------------------------------
AC_MSG_CHECKING([for Lustre release])
RELEASE="`echo ${LINUXRELEASE} | tr '-' '_'`_`date +%Y%m%d%H%M`"
AC_MSG_RESULT($RELEASE)
AC_SUBST(RELEASE)
])

#
# LB_LINUX_PATH
#
# Find paths for linux, handling kernel-source rpms
#
AC_DEFUN([LB_LINUX_PATH],
[AC_MSG_CHECKING([for Linux sources])
AC_ARG_WITH([linux],
	AC_HELP_STRING([--with-linux=path],
		       [set path to Linux source (default=/usr/src/linux)]),
	[LINUX=$with_linux],
	[LINUX=/usr/src/linux])
AC_MSG_RESULT([$LINUX])
AC_SUBST(LINUX)

# -------- check for linux --------
LB_CHECK_FILE([$LINUX],[],
	[AC_MSG_ERROR([Kernel source $LINUX could not be found.])])

# -------- linux objects (for 2.6) --
AC_MSG_CHECKING([for Linux objects dir])
AC_ARG_WITH([linux-obj],
	AC_HELP_STRING([--with-linux-obj=path],
			[set path to Linux objects dir (default=$LINUX)]),
	[LINUX_OBJ=$with_linux_obj],
	[LINUX_OBJ=$LINUX])
AC_MSG_RESULT([$LINUX_OBJ])
AC_SUBST(LINUX_OBJ)

# -------- check for .config --------
AC_ARG_WITH([linux-config],
	[AC_HELP_STRING([--with-linux-config=path],
			[set path to Linux .conf (default=$LINUX_OBJ/.config)])],
	[LINUX_CONFIG=$with_linux_config],
	[LINUX_CONFIG=$LINUX_OBJ/.config])
AC_SUBST(LINUX_CONFIG)

LB_CHECK_FILE([/boot/kernel.h],
	[KERNEL_SOURCE_HEADER='/boot/kernel.h'],
	[LB_CHECK_FILE([/var/adm/running-kernel.h],
		[KERNEL_SOURCE_HEADER='/var/adm/running-kernel.h'])])

AC_ARG_WITH([kernel-source-header],
	AC_HELP_STRING([--with-kernel-source-header=path],
			[Use a different kernel version header.  Consult build/README.kernel-source for details.]),
	[KERNEL_SOURCE_HEADER=$with_kernel_source_header])

# ------------ .config exists ----------------
LB_CHECK_FILE([$LINUX_CONFIG],[],
	[AC_MSG_ERROR([Kernel config could not be found.  If you are building from a kernel-source rpm consult build/README.kernel-source])])

# ----------- make dep run? ------------------
# at 2.6.19 # $LINUX/include/linux/config.h is removed
# and at more old has only one line
# include <autoconf.h>
LB_CHECK_FILES([$LINUX_OBJ/include/linux/autoconf.h
		$LINUX_OBJ/include/linux/version.h
		],[],
	[AC_MSG_ERROR([Run make config in $LINUX.])])
#
LB_CHECK_FILE([$LINUX_OBJ/include/linux/config.h],
	[ AC_DEFINE(HAVE_KERNEL_CONFIG_H, 1,
		[kernel modules need to include config.h])
	]
)

# ------------ rhconfig.h includes runtime-generated bits --
# red hat kernel-source checks

# we know this exists after the check above.  if the user
# tarred up the tree and ran make dep etc. in it, then
# version.h gets overwritten with a standard linux one.

if grep rhconfig $LINUX_OBJ/include/linux/version.h >/dev/null ; then
	# This is a clean kernel-source tree, we need to
	# enable extensive workarounds to get this to build
	# modules
	LB_CHECK_FILE([$KERNEL_SOURCE_HEADER],
		[if test $KERNEL_SOURCE_HEADER = '/boot/kernel.h' ; then
			AC_MSG_WARN([Using /boot/kernel.h from RUNNING kernel.])
			AC_MSG_WARN([If this is not what you want, use --with-kernel-source-header.])
			AC_MSG_WARN([Consult build/README.kernel-source for details.])
		fi],
		[AC_MSG_ERROR([$KERNEL_SOURCE_HEADER not found.  Consult build/README.kernel-source for details.])])
	EXTRA_KCFLAGS="-include $KERNEL_SOURCE_HEADER $EXTRA_KCFLAGS"
fi

# this is needed before we can build modules
LB_LINUX_UML
LB_LINUX_VERSION

# --- check that we can build modules at all
AC_MSG_CHECKING([that modules can be built at all])
LB_LINUX_TRY_COMPILE([],[],[
	AC_MSG_RESULT([yes])
],[
	AC_MSG_RESULT([no])
	AC_MSG_WARN([Consult config.log for details.])
	AC_MSG_WARN([If you are trying to build with a kernel-source rpm, consult build/README.kernel-source])
	AC_MSG_ERROR([Kernel modules cannot be build.])
])

LB_LINUX_RELEASE
])

#
# LB_LINUX_UML
#
# check for a uml kernel
#
AC_DEFUN([LB_LINUX_UML],
[ARCH_UM=
UML_CFLAGS=

AC_MSG_CHECKING([if you are running user mode linux for $target_cpu])
if test -e $LINUX/include/asm-um ; then
	if test  X`ls -id $LINUX/include/asm/ 2>/dev/null | awk '{print [$]1}'` = X`ls -id $LINUX/include/asm-um 2>/dev/null | awk '{print [$]1}'` ; then
		ARCH_UM='ARCH=um'
		# see notes in Rules.in
		UML_CFLAGS='-O0'
		AC_MSG_RESULT(yes)
    	else
		AC_MSG_RESULT([no (asm doesn't point at asm-um)])
	fi
else
	AC_MSG_RESULT([no (asm-um missing)])
fi
AC_SUBST(ARCH_UM)
AC_SUBST(UML_CFLAGS)
])

# these are like AC_TRY_COMPILE, but try to build modules against the
# kernel, inside the build directory

#
# LB_LINUX_CONFTEST
#
# create a conftest.c file
#
AC_DEFUN([LB_LINUX_CONFTEST],
[cat >conftest.c <<_ACEOF
$1
_ACEOF
])


# LB_LANG_PROGRAM(C)([PROLOGUE], [BODY])
# --------------------------------------
m4_define([LB_LANG_PROGRAM],
[$1
int
main (void)
{
dnl Do *not* indent the following line: there may be CPP directives.
dnl Don't move the `;' right after for the same reason.
$2
  ;
  return 0;
}])

#
# LB_LINUX_COMPILE_IFELSE
#
# like AC_COMPILE_IFELSE
#
AC_DEFUN([LB_LINUX_COMPILE_IFELSE],
[m4_ifvaln([$1], [LB_LINUX_CONFTEST([$1])])dnl
rm -f build/conftest.o build/conftest.mod.c build/conftest.ko
AS_IF([AC_TRY_COMMAND(cp conftest.c build && make [$2] CC="$CC" -f $PWD/build/Makefile LUSTRE_LINUX_CONFIG=$LINUX_CONFIG -o tmp_include_depends -o scripts -o include/config/MARKER -C $LINUX_OBJ EXTRA_CFLAGS="-Werror-implicit-function-declaration $EXTRA_KCFLAGS" $ARCH_UM $MODULE_TARGET=$PWD/build) >/dev/null && AC_TRY_COMMAND([$3])],
	[$4],
	[_AC_MSG_LOG_CONFTEST
m4_ifvaln([$5],[$5])dnl])dnl
rm -f build/conftest.o build/conftest.mod.c build/conftest.mod.o build/conftest.ko m4_ifval([$1], [build/conftest.c conftest.c])[]dnl
])

#
# LB_LINUX_TRY_COMPILE
#
# like AC_TRY_COMPILE
#
AC_DEFUN([LB_LINUX_TRY_COMPILE],
[LB_LINUX_COMPILE_IFELSE(
 	[AC_LANG_SOURCE([LB_LANG_PROGRAM([[$1]], [[$2]])])],
	[modules],
	[test -s build/conftest.o],
	[$3], [$4])])

#
# LB_LINUX_CONFIG
#
# check if a given config option is defined
#
AC_DEFUN([LB_LINUX_CONFIG],
[AC_MSG_CHECKING([if Linux was built with CONFIG_$1])
LB_LINUX_TRY_COMPILE([
#ifdef HAVE_KERNEL_CONFIG_H
#include <linux/config.h>
#endif
],[
#ifndef CONFIG_$1
#error CONFIG_$1 not #defined
#endif
],[
AC_MSG_RESULT([yes])
$2
],[
AC_MSG_RESULT([no])
$3
])
])

#
# LB_LINUX_CONFIG_IM
#
# check if a given config option is builtin or as module
#
AC_DEFUN([LB_LINUX_CONFIG_IM],
[AC_MSG_CHECKING([if Linux was built with CONFIG_$1 in or as module])
LB_LINUX_TRY_COMPILE([#include <linux/config.h>],[
#if !(defined(CONFIG_$1) || defined(CONFIG_$1_MODULE))
#error CONFIG_$1 and CONFIG_$1_MODULE not #defined
#endif
],[
AC_MSG_RESULT([yes])
$2
],[
AC_MSG_RESULT([no])
$3
])
])

#
# LB_LINUX_TRY_MAKE
#
# like LB_LINUX_TRY_COMPILE, but with different arguments
#
AC_DEFUN([LB_LINUX_TRY_MAKE],
[LB_LINUX_COMPILE_IFELSE([AC_LANG_SOURCE([LB_LANG_PROGRAM([[$1]], [[$2]])])], [$3], [$4], [$5], [$6])])

#
# LB_LINUX_CONFIG_BIG_STACK
#
# check for big stack patch
#
AC_DEFUN([LB_LINUX_CONFIG_BIG_STACK],
[if test "x$ARCH_UM" = "x" -a "x$linux25" = "xno" ; then
	case $target_cpu in
		i?86 | x86_64)
			LB_LINUX_CONFIG([STACK_SIZE_16KB],[],[
				LB_LINUX_CONFIG([STACK_SIZE_32KB],[],[
					LB_LINUX_CONFIG([STACK_SIZE_64KB],[],[
						AC_MSG_ERROR([Lustre requires that Linux is configured with at least a 16KB stack.])
					])
				])
			])
			;;
	esac
fi
])

#
# LB_PROG_LINUX
#
# linux tests
#
AC_DEFUN([LB_PROG_LINUX],
[LB_LINUX_PATH

LB_LINUX_CONFIG([MODULES],[],[
	AC_MSG_ERROR([module support is required to build Lustre kernel modules.])
])

LB_LINUX_CONFIG([MODVERSIONS])

LB_LINUX_CONFIG([PREEMPT],[
	AC_MSG_ERROR([Lustre does not support kernels with preempt enabled.])
])

LB_LINUX_CONFIG([KALLSYMS],[],[
if test "x$ARCH_UM" = "x" ; then
	AC_MSG_ERROR([Lustre requires that CONFIG_KALLSYMS is enabled in your kernel.])
fi
])

LB_LINUX_CONFIG([KMOD],[],[
	AC_MSG_WARN([])
	AC_MSG_WARN([Kernel module loading support is highly recommended.])
	AC_MSG_WARN([])
])

#LB_LINUX_CONFIG_BIG_STACK

])

#
# LB_LINUX_CONDITIONALS
#
# AM_CONDITIONALS for linux
#
AC_DEFUN([LB_LINUX_CONDITIONALS],
[AM_CONDITIONAL(LINUX25, test x$linux25 = xyes)
])

