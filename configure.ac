AC_INIT(libtirpc, 0.2.3)
AM_INIT_AUTOMAKE([silent-rules])
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE
AC_CONFIG_SRCDIR([src/auth_des.c])
AC_CONFIG_MACRO_DIR([m4])

AC_ARG_ENABLE(gssapi,
	[AC_HELP_STRING([--disable-gssapi], [Disable GSSAPI support @<:@default=no@:>@])],
      [],[enable_gssapi=yes])
AM_CONDITIONAL(GSS, test x$enable_gssapi = xyes)

AC_ARG_WITH(gssglue,
		[  --with-gssglue        Use libgssglue],
		 [case "${enableval}" in
		  yes) gssglue=true ;;
		  no)  gssglue=false ;;
		  *) AC_MSG_ERROR(bad value ${enableval} for --with-gssglue) ;;
		  esac],
		[gssglue=false])
AM_CONDITIONAL(USEGSSGLUE, test x$gssglue = xtrue)

if test x$enable_gssapi = xyes; then
	if test x$gssglue = xtrue; then
		PKG_CHECK_MODULES(GSSAPI, libgssglue, [],
		AC_MSG_ERROR([Unable to locate information required to use libgssglue.]))
	else
		GSSAPI_CFLAGS=`krb5-config --cflags gssapi`
		GSSAPI_LIBS=`krb5-config --libs gssapi`
		AC_SUBST([GSSAPI_CFLAGS])
		AC_SUBST([GSSAPI_LIBS])
	fi
fi
AC_ARG_ENABLE(ipv6,
	[AC_HELP_STRING([--disable-ipv6], [Disable IPv6 support @<:@default=no@:>@])],
	[],[enable_ipv6=yes])
AM_CONDITIONAL(INET6, test "x$disable_ipv6" != xno)
if test "x$enable_ipv6" != xno; then
	AC_DEFINE(INET6, 1, [Define to 1 if IPv6 is available])
fi

AC_PROG_CC
m4_pattern_allow(AM_CONFIG_HEADERS(config.h))
AC_PROG_LIBTOOL
AC_HEADER_DIRENT
AC_PREFIX_DEFAULT(/usr)
AC_CHECK_HEADERS([arpa/inet.h fcntl.h libintl.h limits.h locale.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/ioctl.h sys/param.h sys/socket.h sys/time.h syslog.h unistd.h])
AC_CHECK_LIB([pthread], [pthread_create])
AC_CHECK_LIB([nsl], [yp_get_default_domain])


AC_CONFIG_FILES([Makefile src/Makefile man/Makefile doc/Makefile])
AC_OUTPUT(libtirpc.pc)
