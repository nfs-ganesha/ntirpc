/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef TIRPC_SVC_INTERNAL_H
#define TIRPC_SVC_INTERNAL_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <misc/os_epoll.h>
#include <rpc/rpc_msg.h>

#include "rpc_dplx_internal.h"

extern int __svc_maxiov;
extern int __svc_maxrec;

#if defined(UIO_MAXIOV)
# define PRESUMED_UIO_MAXIOV UIO_MAXIOV
#else
/* On macOS, the non-kernel sys/uio.h header does not define UIO_MAXIOV. */
# define PRESUMED_UIO_MAXIOV 1024
#endif

/* threading fdsets around is annoying */
struct svc_params {
	mutex_t mtx;
	bool initialized;

	/* package global event handling--may be overridden using the
	 * svc_rqst interface */
	enum svc_event_type ev_type;
	union {
		struct {
			uint32_t id;
			uint32_t max_events;
		} evchan;
		struct {
			fd_set set;	/* select/fd_set (currently unhooked) */
		} fd;
	} ev_u;

	svc_xprt_fun_t disconnect_cb;
	svc_xprt_alloc_fun_t alloc_cb;
	svc_xprt_free_fun_t free_cb;

	struct {
		int ctx_hash_partitions;
		int max_ctx;
		int max_idle_gen;
		int max_gc;
	} gss;

	struct {
		u_int send_max;
		u_int thrd_max;
		u_int thrd_min;
	} ioq;

	u_long flags;
	u_int max_connections;
	int32_t idle_timeout;
#if defined(_USE_NFS_RDMA) || defined(USE_RPC_RDMA)
	uint16_t nfs_rdma_port;
	u_int max_rdma_connections;
#endif
};

enum xprt_stat svc_request(SVCXPRT *xprt, XDR *xdrs);

extern struct svc_params __svc_params[1];

/*
 * The following union is defined just to use SVC_CMSG_SIZE macro for an array
 * length. _GNU_SOURCE must be defined to get in6_pktinfo declaration!
 */
union pktinfo_u {
	struct in_pktinfo x;
	struct in6_pktinfo y;
};
#define SVC_CMSG_SIZE CMSG_SPACE(sizeof(union pktinfo_u))

/**
 * \struct svc_dg_xprt
 * DG transport instance
 *
 * Replaces old struct svc_dg_data by locally wrapping struct rpc_dplx_rec,
 * which wraps struct svc_xprt indexed by fd.
 */
#define DG_NUM_PKTINFO 4 /* s/b enough space for all pktinfos in normal case*/
struct svc_dg_xprt {
	struct rpc_dplx_rec su_dr;	/* SVCXPRT indexed by fd */
	struct msghdr su_msghdr;	/* msghdr received from clnt */
	union pktinfo_u su_cmsg[DG_NUM_PKTINFO]; /* cmsghdr recv'd from clnt */
};
#define DG_DR(p) (opr_containerof((p), struct svc_dg_xprt, su_dr))
#define su_data(xprt) (DG_DR(REC_XPRT(xprt)))

/**
 * \struct svc_vc_xprt
 * VC transport instance
 *
 * Replaces old struct x_vc_data by locally wrapping struct rpc_dplx_rec,
 * which wraps struct svc_xprt indexed by fd.
 */
struct svc_vc_xprt {
	struct rpc_dplx_rec sx_dr;	/* SVCXPRT indexed by fd */
	int32_t sx_fbtbc;		/* fragment bytes to be consumed */
};
#define VC_DR(p) (opr_containerof((p), struct svc_vc_xprt, sx_dr))

/* Epoll interface change */
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 02000000
static inline int
epoll_create_wr(size_t size, int flags)
{
	return (epoll_create(size));
}
#else
static inline int
epoll_create_wr(size_t size, int flags)
{
	return (epoll_create1(flags));
}
#endif

#if defined(HAVE_BLKIN)
extern void __rpc_set_blkin_endpoint(SVCXPRT *xprt, const char *tag);
#endif

enum xprt_stat svc_rendezvous_stat(SVCXPRT *);

static inline void
svc_override_ops(struct xp_ops *ops, SVCXPRT *rendezvous)
{
	if (rendezvous) {
		if (!ops->xp_unref_user_data)
			ops->xp_unref_user_data = rendezvous->xp_ops->xp_unref_user_data;

		if (!ops->xp_free_user_data)
			ops->xp_free_user_data =
				rendezvous->xp_ops->xp_free_user_data;
	}
}

/* in svc_rqst.c */
int svc_rqst_rearm_events_locked(SVCXPRT *, uint16_t);

static inline int svc_rqst_rearm_events(SVCXPRT *xprt, uint16_t ev_flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	int code;

	rpc_dplx_rli(rec);

	code = svc_rqst_rearm_events_locked(xprt, ev_flags);

	rpc_dplx_rui(rec);

	return code;
}

int svc_rqst_xprt_register(SVCXPRT *, SVCXPRT *);
void svc_rqst_xprt_unregister(SVCXPRT *, uint32_t);

#if defined(USE_RPC_RDMA)
void svc_rqst_xprt_unregister_rdma(SVCXPRT *, uint32_t);
#endif

int svc_rqst_evchan_write(SVCXPRT *, struct xdr_ioq *, bool);
void svc_rqst_xprt_send_complete(SVCXPRT *);
void svc_rqst_unhook(SVCXPRT *);

typedef struct sockaddr_storage sockaddr_t;
int svc_get_port(sockaddr_t *);

#endif				/* TIRPC_SVC_INTERNAL_H */
