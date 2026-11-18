/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2012-2017 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
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
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

/*
 * Implements a connectionless client side RPC.
 */
#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <rpc/svc_rqst.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"

#define MAX_DEFAULT_FDS                 20000

static enum xprt_stat clnt_dg_rendezvous(SVCXPRT *xprt);
static struct clnt_ops *clnt_dg_ops(void);

struct cu_data {
	struct cx_data cu_cx;
	struct sockaddr_storage cu_raddr;	/* remote address */
};
#define CU_DATA(p) (opr_containerof((p), struct cu_data, cu_cx))

static void
clnt_dg_data_free(struct cu_data *cu)
{
	clnt_data_destroy(&cu->cu_cx);
	mem_free(cu, sizeof(struct cu_data));
}

static struct cu_data *
clnt_dg_data_zalloc(void)
{
	struct cu_data *cu = mem_zalloc(sizeof(struct cu_data));

	clnt_data_init(&cu->cu_cx);
	return (cu);
}

/*
 * Connection less client creation returns with client handle parameters.
 * Default options are set, which the user can change using clnt_control().
 * fd should be open and bound.
 *
 * sendsz and recvsz are the maximum allowable packet sizes that can be
 * sent and received. Normally they are the same, but they can be
 * changed to improve the program efficiency and buffer allocation.
 * If they are 0, use the transport default.
 *
 * Always returns CLIENT. Must check cl_error.re_status,
 * followed by CLNT_DESTROY() as necessary.
 */
CLIENT *
clnt_dg_ncreatef(const int fd,	/* open file descriptor */
		 const struct sockaddr_storage *svcaddr, /* servers address */
		 const rpcprog_t program,	/* program number */
		 const rpcvers_t version,	/* version number */
		 const u_int sendsz,	/* buffer recv size */
		 const u_int recvsz,	/* buffer send size */
		 const uint32_t flags)
{
	struct cu_data *cu = clnt_dg_data_zalloc();
	CLIENT *clnt = &cu->cu_cx.cx_c;
	SVCXPRT *xprt;
	struct svc_dg_xprt *su;
	struct rpc_msg call_msg;
	XDR cu_xdrs[1];		/* temp XDR stream */

	clnt->cl_ops = clnt_dg_ops();

	if (svcaddr == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d called with missing servers address",
			__func__, fd);
		clnt->cl_error.re_status = RPC_UNKNOWNADDR;
		return (clnt);
	}

	/* find or create shared fd state; ref+1 */
	xprt = svc_dg_ncreatef(fd, sendsz, recvsz, flags);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d svc_dg_ncreatef failed",
			__func__, fd);
		clnt->cl_error.re_status = RPC_TLIERROR;
		return (clnt);
	}
	su = su_data(xprt);

	if (!su->su_dr.ev_p) {
		xprt->xp_dispatch.rendezvous_cb = clnt_dg_rendezvous;
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);
	}
	cu->cu_cx.cx_rec = &su->su_dr;

	(void)memcpy(&cu->cu_raddr, svcaddr, sizeof(cu->cu_raddr));

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = su->su_dr.call_xid;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = program;
	call_msg.cb_vers = version;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(cu_xdrs, cu->cu_cx.cx_mcallc, MCALL_MSG_SIZE,
		      XDR_ENCODE);
	if (!xdr_callhdr(cu_xdrs, &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d xdr_callhdr failed",
			__func__, fd);
		clnt->cl_error.re_status = RPC_CANTENCODEARGS;
		XDR_DESTROY(cu_xdrs);
		return (clnt);
	}
	cu->cu_cx.cx_mpos = XDR_GETPOS(cu_xdrs);
	XDR_DESTROY(cu_xdrs);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_DG,
		"%s: fd %d completed",
		__func__, fd);
	return (clnt);
}

static enum xprt_stat
clnt_dg_process(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;

	__warnx(TIRPC_DEBUG_FLAG_WARN,
		"%s: %p fd %d unexpected CALL",
		__func__, xprt, xprt->xp_fd);
	return SVC_STAT(xprt);
}

static enum xprt_stat
clnt_dg_rendezvous(SVCXPRT *xprt)
{
	xprt->xp_dispatch.process_cb = clnt_dg_process;
	return SVC_RECV(xprt);
}

static enum clnt_stat
clnt_dg_call(struct clnt_req *cc)
{
	CLIENT *clnt = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	SVCXPRT *xprt = &rec->xprt;
	struct xdr_ioq *xioq;
	XDR *xdrs;
	u_int32_t *uint32p;
	size_t outlen;

	/* XXX Until gss_get_mic and gss_wrap can be replaced with
	 * iov equivalents, replies with RPCSEC_GSS security must be
	 * encoded in a contiguous buffer.
	 *
	 * Nb, we should probably use getpagesize() on Unix.  Need
	 * an equivalent for Windows.
	 */
	xioq = xdr_ioq_create(RPC_MAXDATA_DEFAULT,
			      __svc_params->ioq.send_max + RPC_MAXDATA_DEFAULT,
			      (cc->cc_auth->ah_cred.oa_flavor == RPCSEC_GSS)
			      ? UIO_FLAG_REALLOC | UIO_FLAG_FREE
			      : UIO_FLAG_FREE);

	xdrs = xioq->xdrs;
	cc->cc_error.re_status = RPC_SUCCESS;

	mutex_lock(&clnt->cl_lock);
	uint32p = (u_int32_t *)&cx->cx_mcallc[0];
	*uint32p = htonl(cc->cc_xid);

	if ((!XDR_PUTBYTES(xdrs, cx->cx_mcallc, cx->cx_mpos))
	    || (!XDR_PUTUINT32(xdrs, cc->cc_proc))
	    || (!AUTH_MARSHALL(cc->cc_auth, xdrs))
	    || (!AUTH_WRAP(cc->cc_auth, xdrs,
			   cc->cc_call.proc, cc->cc_call.where))) {
		/* error case */
		mutex_unlock(&clnt->cl_lock);
		__warnx(TIRPC_DEBUG_FLAG_CLNT_DG,
			"%s: fd %d failed",
			__func__, xprt->xp_fd);
		XDR_DESTROY(xdrs);
		return (RPC_CANTENCODEARGS);
	}
	outlen = (size_t) XDR_GETPOS(xdrs);
	mutex_unlock(&clnt->cl_lock);

	if (sendto(xprt->xp_fd, xdrs->x_v.vio_head, outlen, 0,
		   (struct sockaddr *)&cu->cu_raddr, sizeof(cu->cu_raddr))
	    != outlen) {
		clnt->cl_error.re_errno = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d sendto failed (%d)\n",
			__func__, xprt->xp_fd, clnt->cl_error.re_errno);
		XDR_DESTROY(xdrs);
		return (RPC_CANTSEND);
	}
	XDR_DESTROY(xdrs);

	return (RPC_SUCCESS);
}

static bool
clnt_dg_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	return (xdr_free(xdr_res, res_ptr));
}

 /*ARGSUSED*/
static void
clnt_dg_abort(CLIENT *h)
{
}

static bool
clnt_dg_control(CLIENT *clnt, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	u_int32_t *uint32p;
	bool rslt = true;

	/* always take recv lock first, if taking both locks together */
	rpc_dplx_rli(rec);
	mutex_lock(&clnt->cl_lock);

	/*
	 * By default, SVC_XPRT_FLAG_CLOSE starts false.  It is user
	 * responsibility to do a close on the fd, or set by cl_control.
	 */
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
	case CLGET_SERVER_ADDR:	/* Give him the fd address */
		/* Now obsolete. Only for backward compatibility */
		(void)memcpy(info, &cu->cu_raddr, sizeof(cu->cu_raddr));
		break;
	case CLGET_FD:
		*(int *)info = rec->xprt.xp_fd;
		break;
	case CLGET_SVC_ADDR:
		*(struct sockaddr_storage *)info = cu->cu_raddr;
		break;
	case CLSET_SVC_ADDR:	/* set to new address */
		cu->cu_raddr = *(struct sockaddr_storage *)info;
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
	mutex_unlock(&clnt->cl_lock);

	return (rslt);
}

static void
clnt_dg_destroy(CLIENT *clnt)
{
	struct cx_data *cx = CX_DATA(clnt);

	if (cx->cx_rec) {
		SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
	}
	clnt_dg_data_free(CU_DATA(cx));
}

static struct clnt_ops *
clnt_dg_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock; /* XXXX does it need to be extern? */
	sigset_t mask;
	sigset_t newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_dg_call;
		ops.cl_abort = clnt_dg_abort;
		ops.cl_freeres = clnt_dg_freeres;
		ops.cl_destroy = clnt_dg_destroy;
		ops.cl_control = clnt_dg_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	return (&ops);
}
