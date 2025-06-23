dnl #
dnl # Check for statx() function and STATX_MNT_ID availability
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_STATX], [
	AC_CHECK_FUNC([statx], [
		AC_DEFINE([HAVE_STATX], [1], [statx() is available])

		dnl Check for STATX_MNT_ID availability
		AC_MSG_CHECKING([for STATX_MNT_ID])
		AC_COMPILE_IFELSE([
			AC_LANG_PROGRAM([[
				#include <sys/stat.h>
				#include <fcntl.h>
				#include <linux/stat.h>
			]], [[
				struct statx stx;
				int mask = STATX_MNT_ID;
				(void)mask;
				(void)stx.stx_mnt_id;
			]])
		], [
			AC_MSG_RESULT([yes])
			AC_DEFINE([HAVE_STATX_MNT_ID], [1], [STATX_MNT_ID is available])
		], [
			AC_MSG_RESULT([no])
		])
	])
])
