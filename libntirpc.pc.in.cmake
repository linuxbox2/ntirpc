prefix=@prefix@
exec_prefix=@exec_prefix@
libdir=@libdir@
includedir=@includedir@

Name: libntirpc
Description: New Transport Independent RPC Library
Requires:
Version: @PACKAGE_VERSION@
Libs: -L@libdir@ -lintirpc
Cflags: -I@includedir@/ntirpc
