

struct ntirpc_ops
{
    /* auth_des.c */
    AUTH * (*authdes_pk_seccreate)(const char *, netobj *, u_int, const char *,
                                   const des_block *, nis_server *);
    AUTH * (*authdes_seccreate)(const char *, const u_int, const char *,
                                const des_block *);
    /* authdes_prot.c */
    bool_t (*xdr_authdes_cred)(XDR *xdrs, struct authdes_cred *cred);
    bool_t (*xdr_authdes_verf)(XDR *xdrs, struct authdes_verf *verf);`
    /* auth_gss.c */
    AUTH * (*authgss_create)(CLIENT *, gss_name_t, struct rpc_gss_sec *);
    AUTH * (*authgss_create_default)(CLIENT *, char *, struct rpc_gss_sec *);
    bool_t (*authgss_get_private_data)(AUTH *, struct authgss_private_data *);
    bool_t (*authgss_service)(AUTH *, int);
    bool_t (*authgss_wrap)(AUTH *, XDR *, xdrproc_t, caddr_t);
    bool_t (*authgss_unwrap)(AUTH *, XDR *, xdrproc_t, caddr_t);
    /* authgss_prot.c */
    bool_t (*xdr_rpc_gss_buf)(XDR *, gss_buffer_t, u_int);
    bool_t (*xdr_rpc_gss_cred)(XDR *, struct rpc_gss_cred *);
    bool_t (*xdr_rpc_gss_init_args)(XDR *, gss_buffer_desc *);
    bool_t (*xdr_rpc_gss_init_res)(XDR *, struct rpc_gss_init_res *);
    bool_t (*xdr_rpc_gss_wrap_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t,
                                    gss_qop_t, rpc_gss_svc_t, u_int);
    bool_t (*xdr_rpc_gss_unwrap_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t,
                                      gss_qop_t, rpc_gss_svc_t, u_int);
    bool_t (*xdr_rpc_gss_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t, gss_qop_t,
                               rpc_gss_svc_t, u_int);
    /*  auth_none.c */
    AUTH * (*authnone_create)(void);
    /* auth_time.c */
    int __rpc_get_time_offset(struct timeval *, nis_server *, char *, char **,
                              struct sockaddr_in *);
    /* auth_unix.c */
    
    
/*
authunix_prot.c
bindresvport.c
clnt_bcast.c
clnt_dg.c
clnt_generic.c
clnt_perror.c
clnt_raw.c
clnt_simple.c
clnt_vc.c
clnt_vc_dplx.c
crypt_client.c
des_crypt.c
des_soft.c
epoll_sub.c
getnetconfig.c
getnetpath.c
getpeereid.c
getpublickey.c
getrpcent.c
getrpcport.c
key_call.c
key_prot_xdr.c
mt_misc.c
netname.c
netnamer.c
ntirpc_module.c
pmap_clnt.c
pmap_getmaps.c
pmap_getport.c
pmap_prot2.c
pmap_prot.c
pmap_rmt.c
rbtree.c
rbtree_x.c
rpcb_clnt.c
rpcb_prot.c
rpcb_st_xdr.c
rpc_callmsg.c
rpc_commondata.c
rpc_ctx.c
rpcdname.c
rpc_dplx.c
rpc_dtablesize.c
rpc_generic.c
rpc_prot.c
rpc_soc.c
rtime.c
svc_auth.c
svc_auth_des.c
svc_auth_gss.c
svc_auth_none.c
svc_auth_unix.c
svc.c
svc_dg.c
svc_dplx.c
svc_generic.c
svc_raw.c
svc_rqst.c
svc_run.c
svc_shim.c
svc_simple.c
svc_vc.c
svc_xprt.c
vc_lock.c
xdr_array.c
xdr.c
xdr_float.c
xdr_mem.c
xdr_rec.c
xdr_reference.c
xdr_sizeof.c
xdr_stdio.c
    */
};
