/* $NetBSD: clnt.h,v 1.14 2000/06/02 22:57:55 fvdl Exp $ */

/*
 * Copyright (c) 2010, Oracle America, Inc.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the "Oracle America, Inc." nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * from: @(#)clnt.h 1.31 94/04/29 SMI
 * from: @(#)clnt.h 2.1 88/07/29 4.0 RPCSRC
 * $FreeBSD: src/include/rpc/clnt.h,v 1.21 2003/01/24 01:47:55 fjoe Exp $
 */

/*
 * clnt.h - Client side remote procedure call interface.
 *
 * (1) There is only one SVCXPRT (service transport) per connection.
 * (2) There can be several CLIENT per SVCXPRT.
 * (3) Each CLIENT rpc_err cl_error is idempotent, initially
 *     RPC_SUCCESS (0). Once set to anything else, it should never be
 *     modified, and CLNT_DESTROY() should be invoked.
 * (4) There can be many asynchronous clnt_req per CLIENT.
 * (5) Each clnt_req has its own idempotent rpc_err cc_error.
 */

#ifndef _TIRPC_CLNT_H_
#define _TIRPC_CLNT_H_

#include <misc/rbtree.h>
#include <misc/wait_queue.h>
#include <rpc/svc.h>
#include <rpc/rpc_err.h>
#include <rpc/clnt_stat.h>
#include <rpc/auth.h>
#include "reentrant.h"

#include <sys/cdefs.h>
#include <netconfig.h>
#if !defined(_WIN32)
#include <sys/un.h>
#endif

/*
 * Well-known IPV6 RPC broadcast address.
 */
#define RPCB_MULTICAST_ADDR "ff02::202"

/*
 * the following errors are in general unrecoverable.  The caller
 * should give up rather than retry.
 */
#define IS_UNRECOVERABLE_RPC(s) (((s) == RPC_AUTHERROR) ||      \
				 ((s) == RPC_CANTENCODEARGS) || \
				 ((s) == RPC_CANTDECODERES) ||  \
				 ((s) == RPC_VERSMISMATCH) ||   \
				 ((s) == RPC_PROCUNAVAIL) ||		\
				 ((s) == RPC_PROGUNAVAIL) ||		\
				 ((s) == RPC_PROGVERSMISMATCH) ||       \
				 ((s) == RPC_CANTDECODEARGS))

struct clnt_req;
typedef void (*clnt_req_freer)(struct clnt_req *, size_t);

/*
 * Client rpc handle.
 * Created by individual implementations
 * Client is responsible for initializing auth, see e.g. auth_none.c.
 */
typedef struct rpc_client {

	struct clnt_ops {
		/* call remote procedure */
		enum clnt_stat (*cl_call) (struct clnt_req *);

		/* abort a call */
		void (*cl_abort) (struct rpc_client *);

		/* frees results */
		 bool(*cl_freeres) (struct rpc_client *, xdrproc_t, void *);

		/* release and mark destroyed */
		void (*cl_destroy) (struct rpc_client *);

		/* the ioctl() of rpc */
		 bool(*cl_control) (struct rpc_client *, u_int, void *);
	} *cl_ops;

	char *cl_netid;		/* network token */
	char *cl_tp;		/* device name */
	void *cl_u1;		/* client user data */
	void *cl_u2;		/* client user data */

	mutex_t cl_lock;	/* serialize private data */
	struct rpc_err cl_error; /* specific error code */
	int32_t cl_refcnt;	/* handle reference count */
	uint16_t cl_flags;	/* state flags */

} CLIENT;

#define CLNT_REQ_FLAG_NONE	0x0000
#define CLNT_REQ_FLAG_EXPIRING	0x0001
#define CLNT_REQ_FLAG_BACKSYNC	0x0004
#define CLNT_REQ_FLAG_ACKSYNC	0x0008

/*
 * RPC context.  Intended to enable efficient multiplexing of calls
 * and replies sharing a common channel.
 */
struct clnt_req {
	struct work_pool_entry cc_wpe;
	struct opr_rbtree_node cc_dplx;
	struct opr_rbtree_node cc_rqst;
	struct waitq_entry cc_we;
	struct opaque_auth cc_verf;

	AUTH *cc_auth;
	CLIENT *cc_clnt;
	struct xdrpair cc_call;
	struct xdrpair cc_reply;
	void (*cc_process_cb)(struct clnt_req *);
	clnt_req_freer cc_free_cb;
	struct timespec cc_timeout;
	struct rpc_err cc_error;
	size_t cc_size;
	int cc_expire_ms;
	int cc_refreshes;
	rpcproc_t cc_proc;
	uint32_t cc_xid;
	int32_t cc_refcnt;
	uint16_t cc_flags;
};

/*
 * Timers used for the pseudo-transport protocol when using datagrams
 */
struct rpc_timers {
	u_short rt_srtt;	/* smoothed round-trip time */
	u_short rt_deviate;	/* estimated deviation */
	u_long rt_rtxcur;	/* current (backed-off) rto */
};

/*
 * Feedback values used for possible congestion and rate control
 */
#define FEEDBACK_REXMIT1 1	/* first retransmit */
#define FEEDBACK_OK  2		/* no retransmits */

/* Used to set version of portmapper used in broadcast */

#define CLCR_SET_LOWVERS 3
#define CLCR_GET_LOWVERS 4

#define RPCSMALLMSGSIZE 400	/* a more reasonable packet size */

/*
 * CLNT flags
 */

#define CLNT_FLAG_NONE			SVC_XPRT_FLAG_NONE
#define CLNT_FLAG_DESTROYING		SVC_XPRT_FLAG_DESTROYING
#define CLNT_FLAG_RELEASING		SVC_XPRT_FLAG_RELEASING
#define CLNT_FLAG_DESTROYED		SVC_XPRT_FLAG_DESTROYED
#define CLNT_FLAG_LOCAL			0x8000 /* Client is unshared/local */

/*
 * CLNT_REF flags
 */

#define CLNT_REF_FLAG_NONE		SVC_XPRT_FLAG_NONE
#define CLNT_REF_FLAG_LOCKED		SVC_XPRT_FLAG_LOCKED

/*
 * CLNT_RELEASE flags
 */

#define CLNT_RELEASE_FLAG_NONE		SVC_XPRT_FLAG_NONE
#define CLNT_RELEASE_FLAG_LOCKED	SVC_XPRT_FLAG_LOCKED

/*
 * client side rpc interface ops
 */
#define CLNT_FAILURE(rh) ((rh)->cl_error.re_status != RPC_SUCCESS)
#define CLNT_SUCCESS(rh) ((rh)->cl_error.re_status == RPC_SUCCESS)

/*
 * enum clnt_stat
 * CLNT_CALL_BACK(cc)
 * CLNT_CALL_ONCE(cc)
 * CLNT_CALL_WAIT(cc)
 *  struct clnt_req *cc;
 */
#define CLNT_CALL_BACK(cc) clnt_req_callback(cc)
#define CLNT_CALL_ONCE(cc) ((*(cc)->cc_clnt->cl_ops->cl_call)(cc))
#define CLNT_CALL_WAIT(cc) clnt_req_wait_reply(cc)

/*
 * void
 * CLNT_ABORT(rh);
 *  CLIENT *rh;
 */
#define CLNT_ABORT(rh) ((*(rh)->cl_ops->cl_abort)(rh))
#define clnt_abort(rh) ((*(rh)->cl_ops->cl_abort)(rh))

/*
 * bool
 * CLNT_FREERES(rh, xres, resp);
 *  CLIENT *rh;
 * xdrproc_t xres;
 * void *resp;
 */
#define CLNT_FREERES(rh, xres, resp) \
	((*(rh)->cl_ops->cl_freeres)(rh, xres, resp))
#define clnt_freeres(rh, xres, resp) \
	((*(rh)->cl_ops->cl_freeres)(rh, xres, resp))

/*
 * bool
 * CLNT_CONTROL(cl, request, info)
 *      CLIENT *cl;
 *      u_int request;
 *      char *info;
 */
#define CLNT_CONTROL(cl, rq, in) \
	((*(cl)->cl_ops->cl_control)(cl, rq, in))
#define clnt_control(cl, rq, in) \
	((*(cl)->cl_ops->cl_control)(cl, rq, in))

/*
 * control operations that apply to all transports
 */
/* reserved 1 */
/* reserved 2 */
#define CLGET_SERVER_ADDR 3	/* get server's address (sockaddr) */
/* reserved 4 */
/* reserved 5 */
#define CLGET_FD  6		/* get connections file descriptor */
#define CLGET_SVC_ADDR  7	/* get server's address (netbuf) */
#define CLSET_FD_CLOSE  8	/* close fd while clnt_destroy */
#define CLSET_FD_NCLOSE  9	/* Do not close fd while clnt_destroy */
#define CLGET_XID   10		/* Get xid */
#define CLSET_XID  11		/* Set xid */
#define CLGET_VERS  12		/* Get version number */
#define CLSET_VERS  13		/* Set version number */
#define CLGET_PROG  14		/* Get program number */
#define CLSET_PROG  15		/* Set program number */
#define CLSET_SVC_ADDR  16	/* get server's address (netbuf) */
#define CLSET_PUSH_TIMOD 17	/* push timod if not already present */
#define CLSET_POP_TIMOD  18	/* pop timod */

/* Protect a CLIENT with a CLNT_REF for each call or request.
 */
static inline void clnt_ref_it(CLIENT *clnt, uint32_t flags,
			       const char *tag, const int line)
{
	int32_t refs = atomic_inc_int32_t(&clnt->cl_refcnt);

	if (flags & CLNT_REF_FLAG_LOCKED)  {
		/* unlock before warning trace */
		mutex_unlock(&clnt->cl_lock);
	}
	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: %p %" PRId32 " @%s:%d",
		__func__, clnt, refs, tag, line);
}
#define CLNT_REF(clnt, flags)						\
	clnt_ref_it(clnt, flags, __func__, __LINE__)

static inline void clnt_release_it(CLIENT *clnt, uint32_t flags,
				   const char *tag, const int line)
{
	int32_t refs = atomic_dec_int32_t(&clnt->cl_refcnt);
	uint16_t cl_flags;

	if (flags & CLNT_RELEASE_FLAG_LOCKED) {
		/* unlock before warning trace */
		mutex_unlock(&clnt->cl_lock);
	}
	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: %p %" PRId32 " @%s:%d",
		__func__, clnt, refs, tag, line);

	if (likely(refs > 0)) {
		/* normal case */
		return;
	}

	/* enforce once-only semantic, trace others */
	cl_flags = atomic_postset_uint16_t_bits(&clnt->cl_flags,
						CLNT_FLAG_RELEASING);

	if (cl_flags & CLNT_FLAG_RELEASING) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p WARNING! already destroying! @%s:%d",
			__func__, clnt, tag, line);
		return;
	}

	/* Releasing last reference */
	(*(clnt)->cl_ops->cl_destroy)(clnt);
}
#define CLNT_RELEASE(clnt, flags)					\
	clnt_release_it(clnt, flags, __func__, __LINE__)

/* CLNT_DESTROY is CLNT_RELEASE with once-only semantics.  Also, idempotent
 * CLNT_FLAG_DESTROYED indicates that more references should not be taken.
 */
static inline void clnt_destroy_it(CLIENT *clnt,
				   const char *tag, const int line)
{
	uint16_t flags = atomic_postset_uint16_t_bits(&clnt->cl_flags,
						      CLNT_FLAG_DESTROYING);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: %p %" PRId32 " @%s:%d",
		__func__, clnt, clnt->cl_refcnt, tag, line);

	if (flags & CLNT_FLAG_DESTROYING) {
		/* previously set, do nothing */
		return;
	}

	clnt_release_it(clnt, CLNT_RELEASE_FLAG_NONE, tag, line);
}
#define CLNT_DESTROY(clnt)						\
	clnt_destroy_it(clnt, __func__, __LINE__)

/*
 * RPCTEST is a test program which is accessible on every rpc
 * transport/port.  It is used for testing, performance evaluation,
 * and network administration.
 */

#define RPCTEST_PROGRAM  ((rpcprog_t)1)
#define RPCTEST_VERSION  ((rpcvers_t)1)
#define RPCTEST_NULL_PROC ((rpcproc_t)2)
#define RPCTEST_NULL_BATCH_PROC ((rpcproc_t)3)

/*
 * By convention, procedure 0 takes null arguments and returns them
 */

#define NULLPROC ((rpcproc_t)0)

/*
 * Below are the client handle creation routines for the various
 * implementations of client side rpc.
 *
 * Always returns CLIENT. Must check cl_error.re_status,
 * followed by CLNT_DESTROY() as necessary.
 */
__BEGIN_DECLS

/*
 * Generic client creation routine. Supported protocols are those that
 * belong to the nettype namespace (/etc/netconfig).
 */
extern CLIENT *clnt_ncreate_timed(const char *, const rpcprog_t,
				  const rpcvers_t, const char *,
				  const struct timeval *);
/*
 *
 * const char *hostname;   -- hostname
 * const rpcprog_t prog;   -- program number
 * const rpcvers_t vers;   -- version number
 * const char *nettype;    -- network type
 * const struct timeval *tp;  -- timeout
 */

/*
 * Calls clnt_ncreate_timed() with a NULL value for the timeout
 * pointer, indicating that the default timeout should be used.
 */
static inline CLIENT *
clnt_ncreate(const char *hostname, const rpcprog_t prog, const rpcvers_t vers,
	     const char *nettype)
{
	return (clnt_ncreate_timed(hostname, prog, vers, nettype, NULL));
}

/*
 * Generic client creation routine. Supported protocols belong
 * to the nettype name space.
 */
extern CLIENT *clnt_ncreate_vers_timed(const char *, const rpcprog_t,
				       rpcvers_t *, const rpcvers_t,
				       const rpcvers_t, const char *,
				       const struct timeval *);
/*
 * const char *host;  -- hostname
 * const rpcprog_t prog;  -- program number
 * rpcvers_t *vers_out;  -- servers highest available version
 * const rpcvers_t vers_low; -- low version number
 * const rpcvers_t vers_high; -- high version number
 * const char *nettype;  -- network type
 * const struct timeval *tp -- timeout
 */

/*
 * Calls clnt_create_vers_timed() with a NULL value for the timeout
 * pointer, indicating that the default timeout should be used.
 */
static inline CLIENT *
clnt_ncreate_vers(const char *hostname, const rpcprog_t prog,
		  rpcvers_t *vers_out, const rpcvers_t vers_low,
		  const rpcvers_t vers_high, const char *nettype)
{
	return (clnt_ncreate_vers_timed
		(hostname, prog, vers_out, vers_low, vers_high, nettype, NULL));
}

/*
 * Generic client creation routine. It takes a netconfig structure
 * instead of nettype
 */
extern CLIENT *clnt_tp_ncreate_timed(const char *, const rpcprog_t,
				     const rpcvers_t, const struct netconfig *,
				     const struct timeval *);
/*
 * const char *hostname;   -- hostname
 * const rpcprog_t prog;   -- program number
 * const rpcvers_t vers;   -- version number
 * const struct netconfig *netconf;  -- network config structure
 * const struct timeval *tp  -- timeout
 */

/*
 * Calls clnt_tp_create_timed() with a NULL value for the timeout
 * pointer, indicating that the default timeout should be used.
 */
static inline CLIENT *
clnt_tp_ncreate(const char *hostname, const rpcprog_t prog,
		const rpcvers_t vers, const struct netconfig *nconf)
{
	return (clnt_tp_ncreate_timed(hostname, prog, vers, nconf, NULL));
}

/*
 * Generic TLI create routine. Only provided for compatibility.
 */

extern CLIENT *clnt_tli_ncreate(const int, const struct netconfig *,
				struct netbuf *, const rpcprog_t,
				const rpcvers_t, const u_int, const u_int);
/*
 * const register int fd;  -- fd
 * const struct netconfig *nconf; -- netconfig structure
 * struct netbuf *svcaddr;  -- servers address
 * const u_long prog;   -- program number
 * const u_long vers;   -- version number
 * const u_int sendsz;   -- send size
 * const u_int recvsz;   -- recv size
 */

/*
 * Low level clnt create routines for connectionful transports, e.g. tcp.
 */

/* uint16_t actually used */
#define CLNT_CREATE_FLAG_NONE		SVC_CREATE_FLAG_NONE
#define CLNT_CREATE_FLAG_CLOSE		SVC_CREATE_FLAG_CLOSE

/* uint32_t instructions */
#define CLNT_CREATE_FLAG_CONNECT	0x10000000
#define CLNT_CREATE_FLAG_LISTEN		SVC_CREATE_FLAG_LISTEN
#define CLNT_CREATE_FLAG_SVCXPRT	0x40000000
#define CLNT_CREATE_FLAG_XPRT_DOREG	SVC_CREATE_FLAG_XPRT_DOREG
#define CLNT_CREATE_FLAG_XPRT_NOREG	SVC_CREATE_FLAG_XPRT_NOREG

extern CLIENT *clnt_vc_ncreatef(const int, const struct netbuf *,
				const rpcprog_t, const rpcvers_t,
				const u_int, const u_int, const uint32_t);

static inline CLIENT *
clnt_vc_ncreate(const int fd, const struct netbuf *raddr,
		const rpcprog_t prog, const rpcvers_t vers,
		const u_int sendsz, const u_int recvsz)
{
	return (clnt_vc_ncreatef(fd, raddr, prog, vers, sendsz, recvsz,
				 CLNT_CREATE_FLAG_CONNECT));
}

/*
 * Create a client handle from an active service transport handle.
 */
extern CLIENT *clnt_vc_ncreate_svc(const SVCXPRT *, const rpcprog_t,
				   const rpcvers_t, const uint32_t);
/*
 *      const SVCXPRT *xprt;                    -- active service xprt
 *      const rpcprog_t prog;                   -- RPC program number
 *      const rpcvers_t vers;                   -- RPC program version
 *      const uint32_t flags;                   -- flags
 */

/*
 * Low level clnt create routine for connectionless transports, e.g. udp.
 */
extern CLIENT *clnt_dg_ncreatef(const int, const struct netbuf *,
				const rpcprog_t, const rpcvers_t,
				const u_int, const u_int, const uint32_t);
/*
 * const int fd;    -- open file descriptor
 * const struct netbuf *svcaddr;  -- servers address
 * const rpcprog_t program;  -- program number
 * const rpcvers_t version;  -- version number
 * const u_int sendsz;   -- buffer recv size
 * const u_int recvsz;   -- buffer send size
 */

static inline CLIENT *
clnt_dg_ncreate(const int fd, const struct netbuf *raddr,
		const rpcprog_t prog, const rpcvers_t vers,
		const u_int sendsz, const u_int recvsz)
{
	return (clnt_dg_ncreatef(fd, raddr, prog, vers, sendsz, recvsz,
				 CLNT_CREATE_FLAG_NONE));
}

/*
 * Memory based rpc (for speed check and testing)
 * CLIENT *
 * clnt_raw_create(prog, vers)
 * u_long prog;
 * u_long vers;
 */
extern CLIENT *clnt_raw_ncreate(rpcprog_t, rpcvers_t);

/*
 * Get the transport handle for a given rpc_client.
 */
extern SVCXPRT *clnt_vc_get_client_xprt(const CLIENT *clnt);
/*
 *      const struct rpc_client *clnt;         -- rpc client
 */

/*
 * Client request processing
 */
static inline void clnt_req_fill(struct clnt_req *cc, struct rpc_client *clnt,
				 AUTH *auth, rpcproc_t proc,
				 xdrproc_t xargs, void *argsp,
				 xdrproc_t xresults, void *resultsp)
{
	cc->cc_clnt = clnt;
	cc->cc_auth = auth;
	cc->cc_proc = proc;
	cc->cc_call.proc = xargs;
	cc->cc_call.where = argsp;
	cc->cc_reply.proc = xresults;
	cc->cc_reply.where = resultsp;
	cc->cc_verf = _null_auth;

	cc->cc_free_cb = (clnt_req_freer)__ntirpc_pkg_params.free_size_;
	cc->cc_size = sizeof(*cc);
	cc->cc_refcnt = 1;

	/* protects this */
	pthread_mutex_init(&cc->cc_we.mtx, NULL);
	pthread_mutex_lock(&cc->cc_we.mtx);
	pthread_cond_init(&cc->cc_we.cv, 0);
}

static inline void clnt_req_fini(struct clnt_req *cc)
{
	pthread_cond_destroy(&cc->cc_we.cv);
	pthread_mutex_unlock(&cc->cc_we.mtx);
	pthread_mutex_destroy(&cc->cc_we.mtx);
}

enum clnt_stat clnt_req_callback(struct clnt_req *);
enum clnt_stat clnt_req_refresh(struct clnt_req *);
void clnt_req_reset(struct clnt_req *);
enum clnt_stat clnt_req_setup(struct clnt_req *, struct timespec);
enum clnt_stat clnt_req_wait_reply(struct clnt_req *);
int clnt_req_release(struct clnt_req *);

__END_DECLS
/*
 * Used by rpc_perror() and rpc_sperror()
 */
__BEGIN_DECLS
extern void clnt_perrno(enum clnt_stat);	/* stderr */
extern char *clnt_sperrno(enum clnt_stat);	/* string */
__END_DECLS
/*
 * The simplified interface:
 * enum clnt_stat
 * rpc_call(host, prognum, versnum, procnum, inproc, in, outproc, out, nettype)
 * const char *host;
 * const rpcprog_t prognum;
 * const rpcvers_t versnum;
 * const rpcproc_t procnum;
 * const xdrproc_t inproc;
 * const void *in;
 * const xdrproc_t outproc;
 * void *out;
 * const char *nettype;
 */
__BEGIN_DECLS
extern enum clnt_stat rpc_call(const char *, const rpcprog_t,
			       const rpcvers_t, const rpcproc_t,
			       const xdrproc_t, const void *,
			       const xdrproc_t, void *,
			       const char *);
__END_DECLS
/*
 * RPC broadcast interface
 * The call is broadcasted to all locally connected nets.
 *
 * extern enum clnt_stat
 * rpc_broadcast(prog, vers, proc, xargs, argsp, xresults, resultsp,
 *   eachresult, nettype)
 * const rpcprog_t  prog;  -- program number
 * const rpcvers_t  vers;  -- version number
 * const rpcproc_t  proc;  -- procedure number
 * const xdrproc_t xargs;  -- xdr routine for args
 * void *argsp;  -- pointer to args
 * const xdrproc_t xresults; -- xdr routine for results
 * void *resultsp; -- pointer to results
 * const resultproc_t eachresult; -- call with each result
 * const char  *nettype; -- Transport type
 *
 * For each valid response received, the procedure eachresult is called.
 * Its form is:
 *  done = eachresult(resp, raddr, nconf)
 *   bool done;
 *   void *resp;
 *   struct netbuf *raddr;
 *   struct netconfig *nconf;
 * where resp points to the results of the call and raddr is the
 * address if the responder to the broadcast.  nconf is the transport
 * on which the response was received.
 *
 * extern enum clnt_stat
 * rpc_broadcast_exp(prog, vers, proc, xargs, argsp, xresults, resultsp,
 *   eachresult, inittime, waittime, nettype)
 * const rpcprog_t  prog;  -- program number
 * const rpcvers_t  vers;  -- version number
 * const rpcproc_t  proc;  -- procedure number
 * const xdrproc_t xargs;  -- xdr routine for args
 * void *argsp;  -- pointer to args
 * const xdrproc_t xresults; -- xdr routine for results
 * void  *resultsp; -- pointer to results
 * const resultproc_t eachresult; -- call with each result
 * const int   inittime; -- how long to wait initially
 * const int   waittime; -- maximum time to wait
 * const char  *nettype; -- Transport type
 */
typedef bool(*resultproc_t) (void *, ...);

__BEGIN_DECLS
extern enum clnt_stat rpc_broadcast(const rpcprog_t,
				    const rpcvers_t,
				    const rpcproc_t,
				    const xdrproc_t, void *,
				    const xdrproc_t, void *,
				    const resultproc_t,
				    const char *);
extern enum clnt_stat rpc_broadcast_exp(const rpcprog_t, const rpcvers_t,
					const rpcproc_t, const xdrproc_t,
					void *, const xdrproc_t, void *,
					const resultproc_t, const int,
					const int, const char *);
__END_DECLS
/* For backward compatibility */
#include <rpc/tirpc_compat.h>
#endif				/* !_TIRPC_CLNT_H_ */
