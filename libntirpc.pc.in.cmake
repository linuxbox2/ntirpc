prefix=@prefix@
exec_prefix=@exec_prefix@
libdir=@libdir@
includedir=@includedir@

Name: libntirpc
Description: New Transport Independent RPC Library
Requires:
Version: @NTIRPC_VERSION@
Libs: -L@libdir@ -lntirpc
Cflags: -I@includedir@/ntirpc
