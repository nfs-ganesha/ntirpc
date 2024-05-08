/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * Copyright (c) 2015-2017 Red Hat, Inc. and/or its affiliates.
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
 * Implements an msk connection client side RPC.
 */
#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/poll.h>

#include <sys/time.h>

#include <sys/ioctl.h>
#include <rpc/clnt.h>
#include <arpa/inet.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "rpc_rdma.h"

#define MAX_DEFAULT_FDS		 20000

static struct clnt_ops *clnt_rdma_ops(void);

struct cm_data {
	struct cx_data cm_cx;
};
#define CM_DATA(p) (opr_containerof((p), struct cm_data, cm_cx))

static void
clnt_rdma_data_free(struct cm_data *cm)
{
	clnt_data_destroy(&cm->cm_cx);
	mem_free(cm, sizeof(struct cm_data));
}

static struct cm_data *
clnt_rdma_data_zalloc(void)
{
	struct cm_data *cm = mem_zalloc(sizeof(struct cm_data));

	clnt_data_init(&cm->cm_cx);
	return (cm);
}

/*
 * Create a client handle for a connection.
 *
 * Always returns CLIENT. Must check cl_error.re_status,
 * followed by CLNT_DESTROY() as necessary.
 */
CLIENT *
clnt_rdma_ncreatef(RDMAXPRT *xd,		/* init but NOT connect()ed */
		   const rpcprog_t program,
		   const rpcvers_t version,
		   const u_int flags)
{
	struct cm_data *cm = clnt_rdma_data_zalloc();
	CLIENT *cl = &cm->cm_cx.cx_c;
	struct rpc_msg call_msg;
	XDR xdrs[1];		/* temp XDR stream */

	cl->cl_ops = clnt_rdma_ops();

	if (!xd || xd->state != RDMAXS_INITIAL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p@%p called with invalid transport address",
			__func__, cl, xd);
		cl->cl_error.re_status = RPC_UNKNOWNADDR;
		return (cl);
	}
	cm->cm_cx.cx_rec = &xd->sm_dr;

	rpc_rdma_connect(xd);
	rpc_rdma_connect_finalize(xd);

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = xd->sm_dr.call_xid;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = program;
	call_msg.cb_vers = version;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(xdrs, cm->cm_cx.cx_mcallc, MCALL_MSG_SIZE,
		      XDR_ENCODE);
	if (!xdr_callhdr(xdrs, &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p@%p xdr_callhdr failed",
			__func__, cl, xd);
		cl->cl_error.re_status = RPC_CANTENCODEARGS;
		XDR_DESTROY(xdrs);
		return (cl);
	}
	cm->cm_cx.cx_mpos = XDR_GETPOS(xdrs);
	XDR_DESTROY(xdrs);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_RDMA,
		"%s: %p@%p completed",
		__func__, cl, xd);
	return (cl);
}

/*
 * Send a call.
 *
 * Not truly asynchronous: RDMA cards cannot handle many work queues,
 * so the callback contexts are preallocated and limited.  This will
 * wait for a context to become available, and then will also wait
 * until a spare reply context is also ready.
 */
static enum clnt_stat
clnt_rdma_call(struct clnt_req *cc)
{
	CLIENT *cl = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(cl);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	RDMAXPRT *xd = RDMA_DR(rec);
	struct poolq_entry *have =
		xdr_rdma_ioq_uv_fetch(&xd->sm_dr.ioq, &xd->cbqh,
				 "call context", 1, IOQ_FLAG_NONE);
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)(_IOQ(have));
	XDR *xdrs;
	u_int32_t *uint32p;

	/* free old buffers (should do nothing) */
	xdr_ioq_release(&cbc->recvq.ioq_uv.uvqh);
	xdr_ioq_release(&cbc->sendq.ioq_uv.uvqh);
	xdr_rdma_callq(xd);

	cbc->recvq.xdrs[0].x_lib[1] =
	cbc->sendq.xdrs[0].x_lib[1] = xd;

	(void) xdr_rdma_ioq_uv_fetch(&cbc->sendq, &xd->outbufs_data.uvqh,
				"call buffer", 1, IOQ_FLAG_NONE);
	xdr_ioq_reset(&cbc->sendq, 0);

	xdrs = cbc->sendq.xdrs;
	cc->cc_error.re_status = RPC_SUCCESS;

	mutex_lock(&cl->cl_lock);
	uint32p = (u_int32_t *)&cx->cx_mcallc[0];
	*uint32p = htonl(cc->cc_xid);

	if (!XDR_PUTBYTES(xdrs, cx->cx_mcallc, cx->cx_mpos)
	 || !XDR_PUTUINT32(xdrs, cc->cc_proc)
	 || !AUTH_MARSHALL(cc->cc_auth, xdrs)
	 || !AUTH_WRAP(cc->cc_auth, xdrs,
		       cc->cc_call.proc, cc->cc_call.where)) {
		/* error case */
		mutex_unlock(&cl->cl_lock);
		__warnx(TIRPC_DEBUG_FLAG_CLNT_RDMA,
			"%s: %p@%p failed",
			__func__, cl, cx->cx_rec);
		xdr_ioq_release(&cbc->sendq.ioq_uv.uvqh);
		return (RPC_CANTENCODEARGS);
	}
	mutex_unlock(&cl->cl_lock);

	if (!xdr_rdma_clnt_flushout(cbc)) {
		cl->cl_error.re_errno = errno;
		return (RPC_CANTSEND);
	}

	return (RPC_SUCCESS);
}

static bool
clnt_rdma_freeres(CLIENT *cl, xdrproc_t xdr_res, void *res_ptr)
{
	return (xdr_free(xdr_res, res_ptr));
}

/*ARGSUSED*/
static void
clnt_rdma_abort(CLIENT *h)
{
}

static bool
clnt_rdma_control(CLIENT *cl, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(cl);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	u_int32_t *uint32p;
	bool rslt = true;

	/* always take recv lock first if taking together */
	rpc_dplx_rli(rec); //receive lock clnt
	mutex_lock(&cl->cl_lock);

	switch (request) {
	case CLSET_FD_CLOSE:
		(void)atomic_set_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		goto unlock;
	case CLSET_FD_NCLOSE:
		(void)atomic_clear_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		goto unlock;
	default:
		break;
	}

	/* for other requests which use info */
	if (info == NULL) {
		rslt = false;
		goto unlock;
	}
	switch (request) {
	case CLGET_FD:
		*(struct rpc_dplx_rec **)info = rec;
		break;

	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure *.
		 * This will get the xid of the PREVIOUS call
		 */
		uint32p = (u_int32_t *)&cx->cx_mcallc[0];
		*(u_int32_t *)info = ntohl(*uint32p);
		break;

	case CLSET_XID:
		/* This will set the xid of the NEXT call */
		rec->call_xid = htonl(*(u_int32_t *) info - 1);
		/* decrement by 1 as clnt_req_setup() increments once */
		break;

	case CLGET_VERS:
		/*
		 * This RELIES on the information that, in the call body,
		 * the version number field is the fifth field from the
		 * beginning of the RPC header.
		 */
		uint32p = (u_int32_t *)&cx->cx_mcallc[4 * BYTES_PER_XDR_UNIT];
		*(u_int32_t *)info = ntohl(*uint32p);
		break;

	case CLSET_VERS:
		uint32p = (u_int32_t *)&cx->cx_mcallc[4 * BYTES_PER_XDR_UNIT];
		*uint32p = htonl(*(u_int32_t *)info);
		break;

	case CLGET_PROG:
		/*
		 * This RELIES on the information that, in the call body,
		 * the program number field is the fourth field from the
		 * beginning of the RPC header.
		 */
		uint32p = (u_int32_t *)&cx->cx_mcallc[3 * BYTES_PER_XDR_UNIT];
		*(u_int32_t *)info = ntohl(*uint32p);
		break;

	case CLSET_PROG:
		uint32p = (u_int32_t *)&cx->cx_mcallc[3 * BYTES_PER_XDR_UNIT];
		*uint32p = htonl(*(u_int32_t *)info);
		break;

	default:
		rslt = false;
		break;
	}

unlock:
	rpc_dplx_rui(rec);
	mutex_unlock(&cl->cl_lock);
	return (rslt);
}

static void
clnt_rdma_destroy(CLIENT *clnt)
{
	struct cx_data *cx = CX_DATA(clnt);

	SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
	clnt_rdma_data_free(CM_DATA(cx));
}

static struct clnt_ops *
clnt_rdma_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t	ops_lock;
	sigset_t mask;
	sigset_t newmask;

/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_rdma_call;
		ops.cl_abort = clnt_rdma_abort;
		ops.cl_freeres = clnt_rdma_freeres;
		ops.cl_destroy = clnt_rdma_destroy;
		ops.cl_control = clnt_rdma_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	return (&ops);
}

