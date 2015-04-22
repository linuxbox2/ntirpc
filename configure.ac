AC_INIT(libtirpc, 0.2.4)
AM_INIT_AUTOMAKE([silent-rules])
AM_SILENT_RULES([yes])
AC_CONFIG_SRCDIR([src/auth_des.c])
AC_CONFIG_MACRO_DIR([m4])

AC_ARG_ENABLE(gssapi,
	[AC_HELP_STRING([--disable-gssapi], [Disable GSSAPI support @<:@default=no@:>@])],
      [],[enable_gssapi=yes])
AM_CONDITIONAL(GSS, test x$enable_gssapi = xyes)

if test x$enable_gssapi = xyes; then
	GSSAPI_CFLAGS=`krb5-config --cflags gssapi`
	GSSAPI_LIBS=`krb5-config --libs gssapi`
	AC_SUBST([GSSAPI_CFLAGS])
	AC_SUBST([GSSAPI_LIBS])
fi
AC_ARG_ENABLE(ipv6,
	[AC_HELP_STRING([--disable-ipv6], [Disable IPv6 support @<:@default=no@:>@])],
	[],[enable_ipv6=yes])
AM_CONDITIONAL(INET6, test "x$disable_ipv6" != xno)
if test "x$enable_ipv6" != xno; then
	AC_DEFINE(INET6, 1, [Define to 1 if IPv6 is available])
fi

AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_PROG_LIBTOOL
AC_HEADER_DIRENT
AC_PREFIX_DEFAULT(/usr)
AC_CHECK_HEADERS([arpa/inet.h fcntl.h libintl.h limits.h locale.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/ioctl.h sys/param.h sys/socket.h sys/time.h syslog.h unistd.h])
AC_CHECK_LIB([pthread], [pthread_create])
AC_CHECK_LIB([nsl], [yp_get_default_domain])
AC_CHECK_FUNCS([getrpcbyname getrpcbynumber])

AC_CONFIG_FILES([Makefile src/Makefile man/Makefile doc/Makefile])
AC_OUTPUT(libtirpc.pc)


