
add_definitions(
  -DPORTMAP
  -DINET6
  -D_GNU_SOURCE
)

# XXX FreeBSD?
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

include_directories(
  ${PROJECT_SOURCE_DIR}/tirpc
)

########### next target ###############

SET(ntirpc_common_SRCS
  auth_none.c
  auth_time.c
  auth_unix.c
  authunix_prot.c
  bindresvport.c
  bsd_epoll.c
  city.c
  clnt_bcast.c
  clnt_dg.c
  clnt_generic.c
  clnt_perror.c
  clnt_raw.c
  clnt_simple.c
  clnt_vc.c
  clnt_msk.c
  getnetconfig.c
  getnetpath.c
  getpeereid.c
  getrpcent.c
  getrpcport.c
  mt_misc.c
  pmap_clnt.c
  pmap_getmaps.c
  pmap_getport.c
  pmap_prot.c
  pmap_prot2.c
  pmap_rmt.c
  rbtree.c
  rbtree_x.c
  rpc_dplx.c
  rpc_prot.c
  rpc_callmsg.c
  rpc_commondata.c
  rpc_crc32.c
  rpc_ctx.c
  rpc_dplx_msg.c
  rpc_dtablesize.c
  rpc_generic.c
  rpc_soc.c
  rpcb_clnt.c
  rpcb_prot.c
  rpcb_st_xdr.c
  strlcpy.c
  svc.c
  svc_auth.c
  svc_auth_unix.c
  svc_auth_none.c
  svc_dg.c
  svc_generic.c
  svc_raw.c
  svc_rqst.c
  svc_run.c
  svc_simple.c
  svc_vc.c
  svc_xprt.c
  svc_msk.c
  vc_generic.c
  xdr.c
  xdr_array.c
  xdr_float.c
  xdr_mem.c
  xdr_rec.c
  xdr_reference.c
  xdr_stdio.c
  xdr_inrec.c
  xdr_ioq.c
  svc_ioq.c
  thrdpool.c
  xdr_msk.c
)

if(USE_DES)
  SET(ntirpc_des_SRCS
  auth_des.c
  authdes_prot.c
  )
endif(USE_DES)

if(USE_GSS)
  SET(ntirpc_gss_SRCS
    auth_gss.c authgss_hash.c
    authgss_prot.c
    svc_auth_gss.c
    )

  # fixme
  # libntirpc_la_LDFLAGS += $(GSSGLUE_LIBS)
  # libntirpc_la_CFLAGS = -DHAVE_RPCSEC_GSS $(GSSGLUE_CFLAGS)

endif(USE_GSS)

# declares the library
add_library(ntirpc STATIC
  ${ntirpc_common_SRCS}
  ${ntirpc_des_SRCS}
  ${ntirpc_gss_SRCS}
  )

########### next target ###############

target_link_libraries(ntirpc ${CMAKE_THREAD_LIBS_INIT})

# fixme!
set_target_properties(ntirpc PROPERTIES VERSION 4.2.0 SOVERSION 4)
install(TARGETS ntirpc DESTINATION bin)

########### install files ###############

# We are still missing the install of docs and stuff
