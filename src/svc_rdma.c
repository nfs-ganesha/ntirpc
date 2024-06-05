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
	RDMAXPRT *req_rdma_xprt = RDMA_DR(REC_XPRT(xprt));

	RDMAXPRT *rdma_xprt = rpc_rdma_accept_wait(req_rdma_xprt,
					           __svc_params->idle_timeout);

	if (!rdma_xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return (XPRT_DIED);
	}

	/* We don't manage socket fd for rdma */
	rdma_xprt->sm_dr.xprt.xp_flags = SVC_XPRT_FLAG_INITIAL
				| SVC_XPRT_FLAG_INITIALIZED;

	ss = (struct sockaddr_storage *)rdma_get_local_addr(rdma_xprt->cm_id);
	__rpc_address_setup(&rdma_xprt->sm_dr.xprt.xp_local);
	memcpy(rdma_xprt->sm_dr.xprt.xp_local.nb.buf, ss,
		rdma_xprt->sm_dr.xprt.xp_local.nb.len);

	ss = (struct sockaddr_storage *)rdma_get_peer_addr(rdma_xprt->cm_id);
	__rpc_address_setup(&rdma_xprt->sm_dr.xprt.xp_remote);
	memcpy(rdma_xprt->sm_dr.xprt.xp_remote.nb.buf, ss,
		rdma_xprt->sm_dr.xprt.xp_remote.nb.len);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s:%u local %p remote %p xprt %p", __func__, __LINE__,
		&rdma_xprt->sm_dr.xprt.xp_local.nb,
		&rdma_xprt->sm_dr.xprt.xp_remote.nb,
		&rdma_xprt->sm_dr.xprt);

	svc_rdma_ops(&rdma_xprt->sm_dr.xprt);
	rdma_xprt->sm_dr.recvsz = req_rdma_xprt->sm_dr.recvsz;
	rdma_xprt->sm_dr.sendsz = req_rdma_xprt->sm_dr.sendsz;
	rdma_xprt->sm_dr.pagesz = req_rdma_xprt->sm_dr.pagesz;

	rdma_xprt->sm_dr.recv_hdr_sz = req_rdma_xprt->sm_dr.recv_hdr_sz;
	rdma_xprt->sm_dr.send_hdr_sz = req_rdma_xprt->sm_dr.send_hdr_sz;

	if (xdr_rdma_create(rdma_xprt)) {
		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	if (rpc_rdma_accept_finalize(rdma_xprt)) {
		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	XPRT_TRACE(&rdma_xprt->sm_dr.xprt, __func__, __func__, __LINE__);

	/* Take ref on parent */
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	rdma_xprt->sm_dr.xprt.xp_parent = xprt;

	int retval = xprt->xp_dispatch.rendezvous_cb(&rdma_xprt->sm_dr.xprt);
	if (retval) {
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s:%u ERROR (return %d)",
			__func__, __LINE__, retval);
		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	/* RDMA xprts do not have a valid FD but identified with QPs, But for
	 * bookkeeping the clients we will use the FD of RDMA event channel here
	 */
	rdma_xprt->sm_dr.xprt.xp_fd = rdma_xprt->event_channel->fd;

	if (svc_rdma_add_xprt_fd(&rdma_xprt->sm_dr.xprt)) {
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s:%u svc_rdma_add_xprt failed (xprt %p)",
			__func__, __LINE__, &rdma_xprt->sm_dr.xprt);
		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);
		return (XPRT_DESTROYED);
	}

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s:%u New RDMA client connected xprt %p, xp_fd %d, "
		"qp_num %d, xp_fd %d is_rdma_enabled %d to local port %d "
		"from remote port %d ref %d epoll %#04x",
		__func__, __LINE__,
		&rdma_xprt->sm_dr.xprt, rdma_xprt->sm_dr.xprt.xp_fd,
		rdma_xprt->qp->qp_num,
		rdma_xprt->sm_dr.xprt.xp_fd, rdma_xprt->sm_dr.xprt.xp_rdma,
		rdma_xprt->sm_dr.xprt.xp_local.nb.buf ?
		svc_get_port(rdma_xprt->sm_dr.xprt.xp_local.nb.buf) : 0,
		rdma_xprt->sm_dr.xprt.xp_remote.nb.buf ?
		svc_get_port(rdma_xprt->sm_dr.xprt.xp_remote.nb.buf) : 0,
		rdma_xprt->sm_dr.xprt.xp_refcnt,
		rdma_xprt->sm_dr.xprt.xp_flags);

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
	/* We used xdrs from sendq */
	XDR *xdrs = req->rq_xdrs;
	struct xdr_ioq *recvq = XIOQ(xdrs);
	struct rpc_rdma_cbc *cbc =
		opr_containerof(recvq, struct rpc_rdma_cbc, recvq);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p incoming xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	if (!xdr_rdma_svc_recv(cbc, 0)){
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_rdma_svc_recv failed",
			__func__);
		return (XPRT_DIED);
	}

	req->data_chunk = cbc->data_chunk_uv->v.vio_head;
	req->data_chunk_length = ioquv_size(cbc->data_chunk_uv);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: xdata %p req %p data chunk %p",
		__func__, xdrs->x_data, req, req->data_chunk);

	xdrs->x_op = XDR_DECODE;
	rpc_msg_init(&req->rq_msg);

	if (!xdr_dplx_decode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_dplx_decode failed",
			__func__);
		return (XPRT_DIED);
	}

	/* the checksum */
	req->rq_cksum = 0;

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: post decode req %p data chunk %p",
		__func__, xdrs->x_data, req, req->data_chunk);

	return (req->rq_xprt->xp_dispatch.process_cb(req));
}

static enum xprt_stat
svc_rdma_reply(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	struct xdr_ioq *recvq = XIOQ(xdrs);
	struct rpc_rdma_cbc *cbc =
		opr_containerof(recvq, struct rpc_rdma_cbc, recvq);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p outgoing xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	xdrs = cbc->sendq.xdrs;

	if (!xdr_rdma_svc_reply(cbc, 0, req->data_chunk_length ? 1 : 0)){
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

	if (!xdr_rdma_svc_flushout(cbc, req->data_chunk_length ? 1 : 0)){
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: flushout failed (will set dead)",
			__func__);
		return (XPRT_DIED);
	}

	return (XPRT_IDLE);
}

void
svc_rdma_unlink(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	svc_rqst_xprt_unregister_rdma(xprt, flags);

	/* schedule task to cleanup pending cbcs and release xprt refs */
	REC_XPRT(xprt)->ioq.ioq_wpe.fun = rdma_cleanup_cbcs_task;

	/* Take ref for task */
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	work_pool_submit(&svc_work_pool, &(REC_XPRT(xprt)->ioq.ioq_wpe));
}

void
svc_rdma_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	RDMAXPRT *rdma_xprt = RDMA_DR(REC_XPRT(xprt));

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refcnt %" PRId32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refcnt, tag, line);

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}

	if (xprt->xp_parent)
		SVC_RELEASE(xprt->xp_parent, SVC_RELEASE_FLAG_NONE);

	rpc_rdma_destroy(rdma_xprt);
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
		ops.xp_unlink = svc_rdma_unlink;
		ops.xp_destroy = svc_rdma_destroy,
		ops.xp_control = svc_rdma_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;

	mutex_unlock(&ops_lock);
}
