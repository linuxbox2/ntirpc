prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=@CMAKE_INSTALL_PREFIX@
libdir=@CMAKE_INSTALL_PREFIX@/@LIB_INSTALL_DIR@
includedir=@CMAKE_INSTALL_PREFIX@/include

Name: libntirpc
Description: New Transport Independent RPC Library
Requires:
Version: @NTIRPC_VERSION@
Libs: -L${libdir} -lintirpc
Cflags: -I${includedir}/ntirpc
