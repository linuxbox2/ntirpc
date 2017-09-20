prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/@LIB_INSTALL_DIR@
includedir=${prefix}/include/ntirpc

Name: libntirpc
Description: New Transport Independent RPC Library
Requires:
Version: @NTIRPC_VERSION@
Libs: -lntirpc
Cflags: -I${prefix}/include/ntirpc
