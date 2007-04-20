SUBDIRS = src

nobase_include_HEADERS = tirpc/un-namespace.h \
			 tirpc/spinlock.h \
			 tirpc/reentrant.h \
			 tirpc/nss_tls.h \
			 tirpc/netconfig.h \
			 tirpc/namespace.h \
			 tirpc/libc_private.h \
			 tirpc/getpeereid.h \
			 tirpc/fpmath.h \
			 tirpc/rpcsvc/crypt.x \
			 tirpc/rpcsvc/crypt.h \
			 tirpc/misc/event.h \
			 tirpc/misc/socket.h \
			 tirpc/misc/queue.h \
			 tirpc/rpc/xdr.h \
			 tirpc/rpc/types.h \
			 tirpc/rpc/svc_soc.h \
			 tirpc/rpc/svc.h \
			 tirpc/rpc/svc_dg.h \
			 tirpc/rpc/svc_auth.h \
			 tirpc/rpc/rpc_msg.h \
			 tirpc/rpc/rpc.h \
			 tirpc/rpc/rpcent.h \
			 tirpc/rpc/rpc_com.h \
			 tirpc/rpc/rpcb_prot.x \
			 tirpc/rpc/rpcb_prot.h \
			 tirpc/rpc/rpcb_clnt.h \
			 tirpc/rpc/raw.h \
			 tirpc/rpc/pmap_rmt.h \
			 tirpc/rpc/pmap_prot.h \
			 tirpc/rpc/pmap_clnt.h \
			 tirpc/rpc/nettype.h \
			 tirpc/rpc/des.h \
			 tirpc/rpc/des_crypt.h \
			 tirpc/rpc/clnt_stat.h \
			 tirpc/rpc/clnt_soc.h \
			 tirpc/rpc/clnt.h \
			 tirpc/rpc/auth_unix.h \
			 tirpc/rpc/auth_kerb.h \
			 tirpc/rpc/auth.h \
			 tirpc/rpc/auth_gss.h \
			 tirpc/rpc/auth_des.h
    
install-exec-local:
	cp -p ./doc/etc_netconfig /etc/netconfig
	chmod 0644 /etc/netconfig
