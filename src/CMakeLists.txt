
add_definitions(
  -DPORTMAP
  -DINET6
  -D_GNU_SOURCE
)

# ok on Linux and FreeBSD w/GCC and clang compilers
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

########### next target ###############

SET(ntirpc_common_SRCS
  auth_none.c
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
  getnetconfig.c
  getnetpath.c
  getpeereid.c
  getrpcent.c
  mt_misc.c
  pmap_prot.c
  pmap_prot2.c
  pmap_rmt.c
  rbtree.c
  rbtree_x.c
  rpc_prot.c
  rpc_callmsg.c
  rpc_commondata.c
  rpc_crc32.c
  rpc_dplx_msg.c
  rpc_dtablesize.c
  rpc_generic.c
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
  svc_simple.c
  svc_vc.c
  svc_xprt.c
  xdr.c
  xdr_float.c
  xdr_mem.c
  xdr_reference.c
  xdr_ioq.c
  svc_ioq.c
  work_pool.c
)

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

if(USE_RPC_RDMA)
  SET(ntirpc_rdma_SRCS
  clnt_rdma.c
  rpc_rdma.c
  svc_rdma.c
  xdr_rdma.c
  )
endif(USE_RPC_RDMA)

if(USE_LTTNG_NTIRPC)
  add_subdirectory(lttng)
endif(USE_LTTNG_NTIRPC)

# declares the library
add_library(ntirpc SHARED
  ${ntirpc_common_SRCS}
  ${ntirpc_gss_SRCS}
  ${ntirpc_rdma_SRCS}
  ${ntirpc_lttng_SRCS}
  )

# add required libraries--for Ganesha build, it's ok for them to
# propagate (i.e., omit PRIVATE)
target_link_libraries(ntirpc
  ${SYSTEM_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
  elfstraces
  generate_trace_info
)

# set library version and symbol namespace(s) from gen'd map file
set_target_properties(ntirpc PROPERTIES LINK_FLAGS
	"-Wl,--version-script=${PROJECT_BINARY_DIR}/libntirpc.map"
  VERSION ${NTIRPC_VERSION}
  SOVERSION "${NTIRPC_MAJOR_VERSION}${NTIRPC_MINOR_VERSION}"
  )

install(TARGETS ntirpc DESTINATION ${LIB_INSTALL_DIR})

########### install files ###############

install(
  FILES
    ${PROJECT_SOURCE_DIR}/../elfs/bazel-bin/infra/traces/libelfstraces.so
    ${PROJECT_SOURCE_DIR}/../elfs/bazel-bin/infra/traces/libgenerate_trace_info.so
  DESTINATION ${LIB_INSTALL_DIR}
  PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# We are still missing the install of docs and stuff
