## Process this file with automake to create Makefile.in.

## NOTE: this file doesn't really try to be complete.  In particular
## `make dist' won't work at all.  We're just aiming to get the
## program built.  We also don't bother trying to assemble code, or
## anything like that.


INCLUDES = -I../tirpc -DPORTMAP -DINET6 -DVERSION="\"$(VERSION)\"" \
		-D_GNU_SOURCE -Wall -pipe  

lib_LTLIBRARIES = libtirpc.la

libtirpc_la_LDFLAGS = -lnsl -lpthread

libtirpc_la_SOURCES = auth_none.c auth_unix.c authunix_prot.c bindresvport.c clnt_bcast.c \
        clnt_dg.c clnt_generic.c clnt_perror.c clnt_raw.c clnt_simple.c \
        clnt_vc.c rpc_dtablesize.c getnetconfig.c getnetpath.c getrpcent.c \
        getrpcport.c mt_misc.c pmap_clnt.c pmap_getmaps.c pmap_getport.c \
        pmap_prot.c pmap_prot2.c pmap_rmt.c rpc_prot.c rpc_commondata.c \
        rpc_callmsg.c rpc_generic.c rpc_soc.c rpcb_clnt.c rpcb_prot.c \
        rpcb_st_xdr.c svc.c svc_auth.c svc_dg.c svc_auth_unix.c svc_generic.c \
        svc_raw.c svc_run.c svc_simple.c svc_vc.c getpeereid.c \
        auth_time.c auth_des.c authdes_prot.c

## XDR
libtirpc_la_SOURCES += xdr.c xdr_rec.c xdr_array.c xdr_float.c xdr_mem.c xdr_reference.c xdr_stdio.c

## Secure-RPC
if GSS
    libtirpc_la_SOURCES += auth_gss.c authgss_prot.c svc_auth_gss.c
    libtirpc_la_LDFLAGS += -lgssapi
    libtirpc_la_CFLAGS = -DHAVE_RPCSEC_GSS
endif

## libtirpc_a_SOURCES += key_call.c key_prot_xdr.c getpublickey.c
## libtirpc_a_SOURCES += netname.c netnamer.c rpcdname.c \
## libtirpc_a_SOURCES += rtime.c \
## auth_time.c auth_des.c authdes_prot.c 

if GSS
$(libtirpc_la_OBJECTS) :auth_none.c auth_unix.c authunix_prot.c bindresvport.c clnt_bcast.c \
        clnt_dg.c clnt_generic.c clnt_perror.c clnt_raw.c clnt_simple.c \
        clnt_vc.c rpc_dtablesize.c getnetconfig.c getnetpath.c getrpcent.c \
        getrpcport.c mt_misc.c pmap_clnt.c pmap_getmaps.c pmap_getport.c \
        pmap_prot.c pmap_prot2.c pmap_rmt.c rpc_prot.c rpc_commondata.c \
        rpc_callmsg.c rpc_generic.c rpc_soc.c rpcb_clnt.c rpcb_prot.c \
        rpcb_st_xdr.c svc.c svc_auth.c svc_dg.c svc_auth_unix.c svc_generic.c \
        svc_raw.c svc_run.c svc_simple.c svc_vc.c \
	xdr.c xdr_rec.c xdr_array.c xdr_float.c xdr_mem.c xdr_reference.c xdr_stdio.c \
	auth_gss.c authgss_prot.c svc_auth_gss.c getpeereid.c \
	auth_time.c auth_des.c authdes_prot.c
        
else
$(libtirpc_la_OBJECTS) :auth_none.c auth_unix.c authunix_prot.c bindresvport.c clnt_bcast.c \
        clnt_dg.c clnt_generic.c clnt_perror.c clnt_raw.c clnt_simple.c \
        clnt_vc.c rpc_dtablesize.c getnetconfig.c getnetpath.c getrpcent.c \
        getrpcport.c mt_misc.c pmap_clnt.c pmap_getmaps.c pmap_getport.c \
        pmap_prot.c pmap_prot2.c pmap_rmt.c rpc_prot.c rpc_commondata.c \
        rpc_callmsg.c rpc_generic.c rpc_soc.c rpcb_clnt.c rpcb_prot.c \
        rpcb_st_xdr.c svc.c svc_auth.c svc_dg.c svc_auth_unix.c svc_generic.c \
        svc_raw.c svc_run.c svc_simple.c svc_vc.c \
	xdr.c xdr_rec.c xdr_array.c xdr_float.c xdr_mem.c xdr_reference.c xdr_stdio.c \
	getpeereid.c auth_time.c auth_des.c authdes_prot.c
endif
