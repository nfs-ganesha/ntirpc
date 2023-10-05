NTIRPC_${NTIRPC_VERSION_BASE} {
  global:
    # __*
    __ntirpc_pkg_params;
    __rpc_address_port;
    __rpc_address_set_length;
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
    authnone_ncreate_dummy;
    authunix_ncreate;
    authunix_ncreate_default;

    # b*
    bindresvport;
    bindresvport_sa;

    # c*
    cbc_crypt;
    clnt_ncreate_timed;
    clnt_ncreate_vers_timed;
    clnt_dg_ncreatef;
    clnt_perrno;
    clnt_raw_ncreate;
    clnt_req_callback;
    clnt_req_refresh;
    clnt_req_release;
    clnt_req_reset;
    clnt_req_setup;
    clnt_req_wait_reply;
    clnt_sperrno;
    clnt_tli_create;
    clnt_tp_ncreate_timed;
    clnt_vc_get_client_xprt;
    clnt_vc_ncreatef;
    clnt_vc_ncreate_svc;

    # e*
    endnetconfig;
    endnetpath;
    endrpcent;

    # f*
    freenetconfigent;

    # g*
    getnetconfig;
    getnetconfigent;
    getnetpath;
    getrpcent;
    getrpcbynumber;
    getrpcbyname;

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

    # r*
    rbtx_init;
    rpc_broadcast;
    rpc_broadcast_exp;
    rpc_call;
    rpc_control;
    rpc_perror;
    rpc_nullproc;
    rpc_rdma_ncreatef;
    rpc_reg;
    rpc_sperror;
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
    svc_fd_ncreatef;
    svc_init;
    svc_ncreate;
    svc_raw_ncreate;
    svc_reg;
    svc_resume;
    svc_rqst_new_evchan;
    svc_rqst_evchan_reg;
    svc_rqst_evchan_unreg;
    svc_rqst_shutdown;
    svc_rqst_thrd_run;
    svc_rqst_thrd_signal;
    svc_sendreply;
    svc_shutdown;
    svc_tli_ncreate;
    svc_tp_ncreate;
    svc_unreg;
    svc_validate_xprt_list;
    svc_vc_ncreatef;
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

    # t*
    taddr2uaddr;
    tirpc_control;

    # u*
    uaddr2taddr;

    # x*
    xdr_authunix_parms;
    xdr_call_decode;
    xdr_call_encode;
    xdr_double;
    xdr_dplx_decode;
    xdr_dplx_msg;
    xdr_float;
    xdr_free_null_stream;
    xdr_int;
    xdr_long;
    xdr_longlong_t;
    xdr_naccepted_reply;
    xdr_ncallhdr;
    xdr_ncallmsg;
    xdr_netbuf;
    xdr_nnetobj;
    xdr_nrejected_reply;
    xdr_nreplymsg;
    xdr_pmap;
    xdr_pmaplist;
    xdr_pmaplist_ptr;
    xdr_pointer;
    xdr_reference;
    xdr_rmtcall_args;
    xdr_rmtcallres;
    xdr_rpc_gss_cred;
    xdr_rpc_gss_init_args;
    xdr_rpc_gss_init_res;
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
    xdr_u_int;
    xdr_u_long;
    xdr_u_longlong_t;
    xdr_void;
    xdr_wrapstring;
    xdrmem_ncreate;
    xdrstdio_create;

  local:
    *;
};

NTIRPC_PRIVATE {
  global:
  global_foo_bar;
};
