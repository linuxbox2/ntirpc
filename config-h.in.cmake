/* config.h file expanded by Cmake for build */

#ifndef CONFIG_H
#define CONFIG_H

#define NTIRPC_VERSION_MAJOR @NTIRPC_MAJOR_VERSION@
#define NTIRPC_VERSION_MINOR @NTIRPC_MINOR_VERSION@
#define NTIRPC_PATCH_LEVEL @NTIRPC_PATCH_LEVEL@

#define VERSION "${NTIRPC_VERSION_MAJOR}.${NTIRPC_VERSION_MINOR}.${NTIRPC_PATCH_LEVEL}"
#define VERSION_COMMENT "@VERSION_COMMENT@"
#define _GIT_HEAD_COMMIT "@_GIT_HEAD_COMMIT@"
#define _GIT_DESCRIBE "@_GIT_DESCRIBE@"

/* Build controls */

#cmakedefine _MSPAC_SUPPORT 1
#cmakedefine HAVE_STDBOOL_H 1
#cmakedefine HAVE_KRB5 1
#cmakedefine KRB5_VERSION @KRB5_VERSION@
#cmakedefine LINUX 1
#cmakedefine FREEBSD 1
#cmakedefine _HAVE_GSSAPI 1
#cmakedefine HAVE_STRING_H 1
#cmakedefine HAVE_STRINGS_H 1
#cmakedefine LITTLEEND 1
#cmakedefine BIGEND 1
#cmakedefine TIRPC_EPOLL 1

/* Package stuff */
#define PACKAGE "libntirpc"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "libntirpc"
#define PACKAGE_STRING "libntirpc ${NTIRPC_VERSION_MAJOR}.${NTIRPC_VERSION_MINOR}.${NTIRPC_PATCH_LEVEL}"
#define PACKAGE_TARNAME "libntirpc"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "${NTIRPC_VERSION_MAJOR}.${NTIRPC_VERSION_MINOR}.${NTIRPC_PATCH_LEVEL}"

#endif /* CONFIG_H */
