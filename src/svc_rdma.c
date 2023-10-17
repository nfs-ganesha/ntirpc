/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * contributeur : William Allen Simpson <bill@cohortfs.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
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
 */

/*
 * Implements connection server side RPC/RDMA.
 */

#include "config.h"

#include <sys/cdefs.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/poll.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h> /* before rpc.h */
#endif
#include <rpc/rpc.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netconfig.h>
#include <err.h>

#include "rpc_com.h"
#include "misc/city.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_rdma.h"
#include <rpc/svc_rqst.h>
#include <rpc/svc_auth.h>

static void svc_rdma_ops(SVCXPRT *);

/*
 * svc_rdma_rendezvous: waits for connection request
 */
enum xprt_stat
svc_rdma_rendezvous(SVCXPRT *xprt)
{
	struct sockaddr_storage *ss;
	RDMAXPRT *req_xd = RDMA_DR(REC_XPRT(xprt));
	RDMAXPRT *xd = rpc_rdma_accept_wait(req_xd,
					    __svc_params->idle_timeout);

	if (!xd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return (XPRT_DIED);
	}

	xd->sm_dr.xprt.xp_flags = SVC_XPRT_FLAG_CLOSE
				| SVC_XPRT_FLAG_INITIAL
				| SVC_XPRT_FLAG_INITIALIZED;
	/* fixme: put something here, but make it not work on fd operations. */
	xd->sm_dr.xprt.xp_fd = -1;

	ss = (struct sockaddr_storage *)rdma_get_local_addr(xd->cm_id);
	__rpc_address_setup(&xd->sm_dr.xprt.xp_local);
	memcpy(&xd->sm_dr.xprt.xp_local.nb.buf, ss,
		xd->sm_dr.xprt.xp_local.nb.len);

	ss = (struct sockaddr_storage *)rdma_get_peer_addr(xd->cm_id);
	__rpc_address_setup(&xd->sm_dr.xprt.xp_remote);
	memcpy(&xd->sm_dr.xprt.xp_remote.nb.buf, ss,
		xd->sm_dr.xprt.xp_remote.nb.len);

	svc_rdma_ops(&xd->sm_dr.xprt);

	if (xdr_rdma_create(xd)) {
		SVC_DESTROY(&xd->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	if (rpc_rdma_accept_finalize(xd)) {
		SVC_DESTROY(&xd->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	xd->sm_dr.xprt.xp_parent = xprt;
	if (xprt->xp_dispatch.rendezvous_cb(&xd->sm_dr.xprt)
	 || svc_rqst_xprt_register(&xd->sm_dr.xprt, xprt)) {
		SVC_DESTROY(&xd->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}
	return (XPRT_IDLE);
}

/*ARGSUSED*/
static enum xprt_stat
svc_rdma_stat(SVCXPRT *xprt)
{
	/* note: RDMAXPRT is this xprt! */
	switch((RDMA_DR(REC_XPRT(xprt)))->state) {
		case RDMAXS_LISTENING:
		case RDMAXS_CONNECTED:
			return (XPRT_IDLE);
			/* any point in adding a break? */
		default: /* suppose anything else means a problem */
			return (XPRT_DIED);
	}
}

static enum xprt_stat
svc_rdma_decode(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	struct xdr_ioq *holdq = XIOQ(xdrs);
	struct rpc_rdma_cbc *cbc =
		opr_containerof(holdq, struct rpc_rdma_cbc, holdq);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p incoming xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	if (!xdr_rdma_svc_recv(cbc, 0)){
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_rdma_svc_recv failed",
			__func__);
		return (XPRT_DIED);
	}

	xdrs->x_op = XDR_DECODE;
	/* No need, already positioned to beginning ...
	XDR_SETPOS(xdrs, 0);
	 */
	rpc_msg_init(&req->rq_msg);

	if (!xdr_dplx_decode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_dplx_decode failed",
			__func__);
		return (XPRT_DIED);
	}

	/* the checksum */
	req->rq_cksum = 0;

	return (req->rq_xprt->xp_dispatch.process_cb(req));
}

static enum xprt_stat
svc_rdma_reply(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	struct xdr_ioq *holdq = XIOQ(xdrs);
	struct rpc_rdma_cbc *cbc =
		opr_containerof(holdq, struct rpc_rdma_cbc, holdq);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p outgoing xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	if (!xdr_rdma_svc_reply(cbc, 0)){
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_rdma_svc_reply failed (will set dead)",
			__func__);
		return (XPRT_DIED);
	}
	xdrs->x_op = XDR_ENCODE;

	if (!xdr_reply_encode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_reply_encode failed (will set dead)",
			__func__);
		return (XPRT_DIED);
	}
	xdr_tail_update(xdrs);

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	 && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS
	 && req->rq_auth
	 && !SVCAUTH_WRAP(req, xdrs)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: SVCAUTH_WRAP failed (will set dead)",
			__func__);
		return (XPRT_DIED);
	}
	xdr_tail_update(xdrs);

	if (!xdr_rdma_svc_flushout(cbc)){
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: flushout failed (will set dead)",
			__func__);
		return (XPRT_DIED);
	}

	return (XPRT_IDLE);
}

static void
svc_rdma_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	RDMAXPRT *xd = RDMA_DR(REC_XPRT(xprt));

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refcnt %" PRId32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refcnt, tag, line);

	xdr_rdma_destroy(xd);

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}
	if (xprt->xp_parent)
		SVC_RELEASE(xprt->xp_parent, SVC_RELEASE_FLAG_NONE);
	rpc_rdma_destroy(xd);
}

extern mutex_t ops_lock;

static bool
/*ARGSUSED*/
svc_rdma_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
	    *(u_int *)in = xprt->xp_flags;
	    break;
	case SVCSET_XP_FLAGS:
	    xprt->xp_flags = *(u_int *)in;
	    break;
	case SVCGET_XP_UNREF_USER_DATA:
	    mutex_lock(&ops_lock);
	    *(svc_xprt_void_fun_t *) in = xprt->xp_ops->xp_unref_user_data;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_UNREF_USER_DATA:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_unref_user_data = *(svc_xprt_void_fun_t) in;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCGET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    *(svc_xprt_fun_t *)in = xprt->xp_ops->xp_free_user_data;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_free_user_data = *(svc_xprt_fun_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	default:
	    return (FALSE);
	}
	return (TRUE);
}

static void
svc_rdma_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */

	mutex_lock(&ops_lock);

	/* Fill in type of service */
	xprt->xp_type = XPRT_RDMA;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = NULL;
		ops.xp_stat = svc_rdma_stat;
		ops.xp_decode = svc_rdma_decode;
		ops.xp_reply = svc_rdma_reply;
		ops.xp_checksum = NULL;		/* not used */
		ops.xp_unref_user_data = NULL;	/* no default */
		ops.xp_destroy = svc_rdma_destroy,
		ops.xp_control = svc_rdma_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;

	mutex_unlock(&ops_lock);
}
