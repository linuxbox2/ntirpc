NTIRPC_${NTIRPC_VERSION} {
  global:
    # __*
    __ntirpc_pkg_params;
    __rpc_address_port;
    __rpc_address_set_length;
    __rpc_createerr;
    __rpc_dtbsize;
    __rpc_endconf;
    __rpc_fd2sockinfo;
    __rpc_fixup_addr;
    __rpc_get_a_size;
    __rpc_get_local_uid;
    __rpc_get_t_size;
    __rpc_getconf;
    __rpc_getconfip;
    __rpc_nconf2fd;
    __rpc_nconf2fd_flags;
    __rpc_nconf2sockinfo;
    __rpc_rawcombuf;
    __rpc_seman2socktype;
    __rpc_setconf;
    __rpc_sockinfo2netid;
    __rpc_sockisbound;
    __rpc_socktype2seman;
    __rpc_taddr2uaddr_af;
    __rpc_uaddr2taddr_af;
    __rpcgettp;

    # _*
    _authenticate;
    _get_next_token;
    _gss_authenticate;
    _svcauth_gss;
    _null_auth;
    _rpc_dtablesize;
    _seterr_reply;
    _svcauth_none;
    _svcauth_short;
    _svcauth_unix;

    # a*
    authgss_ncreate;
    authgss_ncreate_default;
    authgss_get_private_data;
    authgss_service;
    authnone_ncreate;
    authunix_ncreate;
    authunix_ncreate_default;

    # b*
    bindresvport;
    bindresvport_sa;

    # c*
    callrpc;
    cbc_crypt;
    clnt_broadcast;
    clnt_ncreate;
    clnt_ncreate_timed;
    clnt_ncreate_vers;
    clnt_ncreate_vers_timed;
    clnt_dg_ncreatef;
    clnt_pcreateerror;
    clnt_perrno;
    clnt_perror;
    clnt_raw_ncreate;
    clnt_spcreateerror;
    clnt_sperrno;
    clnt_sperror;
    clnt_tli_create;
    clnt_tp_ncreate;
    clnt_tp_ncreate_timed;
    clnt_vc_ncreatef;
    clnt_vc_ncreate_svc;
    clntraw_ncreate;
    clnttcp_ncreate;
    clntudp_nbufcreate;
    clntudp_ncreate;
    clntunix_ncreate;

    # e*
    endnetconfig;
    endnetpath;
    endrpcent;

    # f*
    freenetconfigent;

    # g*
    get_myaddress;
    getnetconfig;
    getnetconfigent;
    getnetpath;
    getrpcent;
    getrpcbynumber;
    getrpcbyname;
    getrpcport;

    # n*
    nc_perror;
    nc_sperror;

    # o*
    opr_rbtree_first;
    opr_rbtree_init;
    opr_rbtree_insert;
    opr_rbtree_insert_at;
    opr_rbtree_last;
    opr_rbtree_lookup;
    opr_rbtree_next;
    opr_rbtree_prev;
    opr_rbtree_remove;
    opr_rbtree_replace;

    # p*
    pmap_getmaps;
    pmap_getport;
    pmap_rmtcall;
    pmap_set;
    pmap_unset;

    # r*
    rbtx_init;
    registerrpc;
    rpc_broadcast;
    rpc_broadcast_exp;
    rpc_call;
    rpc_control;
    rpc_createerr;
    rpc_nullproc;
    rpc_rdma_create;
    rpc_reg;
    rpcb_find_mapped_addr;
    rpcb_getaddr;
    rpcb_getmaps;
    rpcb_gettime;
    rpcb_rmtcall;
    rpcb_set;
    rpcb_taddr2uaddr;
    rpcb_uaddr2taddr;
    rpcb_unset;

    # s*
    setnetconfig;
    setnetpath;
    setrpcent;
    svc_auth_authenticate;
    svc_auth_reg;
    svc_dg_ncreatef;
    svc_exit;
    svc_fd_ncreatef;
    svc_init;
    svc_ncreate;
    svc_raw_ncreate;
    svc_rdma_ncreate;
    svc_reg;
    svc_register;
    svc_rqst_new_evchan;
    svc_rqst_evchan_reg;
    svc_rqst_evchan_unreg;
    svc_rqst_rearm_events;
    svc_rqst_thrd_run;
    svc_rqst_thrd_signal;
    svc_run;
    svc_run_epoll;
    svc_sendreply;
    svc_shutdown;
    svc_tli_ncreate;
    svc_tp_ncreate;
    svc_unreg;
    svc_unregister;
    svc_validate_xprt_list;
    svc_vc_ncreate;
    svc_vc_ncreate2;
    svc_xprt_trace;
    svcauth_gss_acquire_cred;
    svcauth_gss_destroy;
    svcauth_gss_get_principal;
    svcauth_gss_import_name;
    svcauth_gss_nextverf;
    svcauth_gss_release_cred;
    svcauth_gss_set_svc_name;
    svcerr_auth;
    svcerr_decode;
    svcerr_noproc;
    svcerr_noprog;
    svcerr_progvers;
    svcerr_systemerr;
    svcerr_weakauth;
    svcfd_ncreate;
    svcraw_ncreate;
    svctcp_ncreate;
    svcudp_nbufcreate;
    svcudp_ncreate;
    svcunix_ncreate;
    svcunixfd_ncreate;

    # t*
    taddr2uaddr;
    tirpc_control;

    # u*
    uaddr2taddr;

    # x*
    xdr_array;
    xdr_authunix_parms;
    xdr_bool;
    xdr_bytes;
    xdr_call_decode;
    xdr_call_encode;
    xdr_char;
    xdr_double;
    xdr_dplx_decode;
    xdr_dplx_msg;
    xdr_enum;
    xdr_float;
    xdr_free_null_stream;
    xdr_hyper;
    xdr_inrec_cksum;
    xdr_inrec_create;
    xdr_inrec_eof;
    xdr_inrec_readahead;
    xdr_inrec_skiprecord;
    xdr_int;
    xdr_int8_t;
    xdr_int16_t;
    xdr_int32_t;
    xdr_int64_t;
    xdr_long;
    xdr_longlong_t;
    xdr_naccepted_reply;
    xdr_ncallhdr;
    xdr_ncallmsg;
    xdr_netbuf;
    xdr_nnetobj;
    xdr_nrejected_reply;
    xdr_nreplymsg;
    xdr_opaque;
    xdr_opaque_auth;
    xdr_pmap;
    xdr_pmaplist;
    xdr_pmaplist_ptr;
    xdr_pointer;
    xdr_quad_t;
    xdr_reference;
    xdr_rmtcall_args;
    xdr_rmtcallres;
    xdr_rpc_gss_buf;
    xdr_rpc_gss_cred;
    xdr_rpc_gss_data;
    xdr_rpc_gss_init_args;
    xdr_rpc_gss_init_res;
    xdr_rpc_gss_unwrap_data;
    xdr_rpc_gss_wrap_data;
    xdr_rpcb;
    xdr_rpcb_entry;
    xdr_rpcb_entry_list_ptr;
    xdr_rpcb_rmtcallargs;
    xdr_rpcb_rmtcallres;
    xdr_rpcb_stat;
    xdr_rpcb_stat_byvers;
    xdr_rpcblist;
    xdr_rpcblist_ptr;
    xdr_rpcbs_addrlist;
    xdr_rpcbs_addrlist_ptr;
    xdr_rpcbs_proc;
    xdr_rpcbs_rmtcalllist;
    xdr_rpcbs_rmtcalllist_ptr;
    xdr_short;
    xdr_string;
    xdr_u_char;
    xdr_u_hyper;
    xdr_u_int8_t;
    xdr_u_int16_t;
    xdr_u_int32_t;
    xdr_u_int64_t;
    xdr_u_int;
    xdr_u_long;
    xdr_u_longlong_t;
    xdr_u_quad_t;
    xdr_u_short;
    xdr_uint8_t;
    xdr_uint16_t;
    xdr_uint32_t;
    xdr_uint64_t;
    xdr_union;
    xdr_vector;
    xdr_void;
    xdr_wrapstring;
    xdrmem_ncreate;
    xdrrec_create;
    xdrrec_endofrecord;
    xdrrec_eof;
    xdrrec_skiprecord;
    xdrstdio_create;
    xprt_register;
    xprt_unregister;

  local:
    *;
};

NTIRPC_PRIVATE {
  global:
  global_foo_bar;
};
