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
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.  [Ouch, that was intemperate.]
 */
#include "config.h"

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <misc/socket.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include "rpc_com.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_ioq.h>
#include "svc_ioq.h"
#include "clnt_internal.h"
#include "svc_internal.h"

static enum xprt_stat clnt_vc_process(struct svc_req *req);
static struct clnt_ops *clnt_vc_ops(void);

struct ct_data {
	struct cx_data ct_cx;
	struct sockaddr_storage ct_raddr;	/* remote addr */
	int ct_rlen;
};
#define CT_DATA(p) (opr_containerof((p), struct ct_data, ct_cx))

static void
clnt_vc_data_free(struct ct_data *ct)
{
	clnt_data_destroy(&ct->ct_cx);
	mem_free(ct, sizeof(struct ct_data));
}

static struct ct_data *
clnt_vc_data_zalloc(void)
{
	struct ct_data *ct = mem_zalloc(sizeof(struct ct_data));

	clnt_data_init(&ct->ct_cx);
	return (ct);
}

/*
 *      This machinery implements per-fd locks for MT-safety.  It is not
 *      sufficient to do per-CLIENT handle locks for MT-safety because a
 *      user may create more than one CLIENT handle with the same fd behind
 *      it.  Therfore, we allocate an array of flags (vc_fd_locks), protected
 *      by the clnt_fd_lock mutex, and an array (vc_cv) of condition variables
 *      similarly protected.  Vc_fd_lock[fd] == 1 => a call is active on some
 *      CLIENT handle created for that fd.  (Historical interest only.)
 *
 *      The current implementation holds locks across the entire RPC and reply.
 *      Yes, this is silly, and as soon as this code is proven to work, this
 *      should be the first thing fixed.  One step at a time.  (Fixing this.)
 *
 *      The ONC RPC record marking (RM) standard (RFC 5531, s. 11) does not
 *      provide for mixed call and reply fragment reassembly, so writes to the
 *      bytestream with MUST be record-wise atomic.  It appears that an
 *      implementation may interleave call and reply messages on distinct
 *      conversations.  This is not incompatible with holding a transport
 *      exclusive locked across a full call and reply, bud does require new
 *      control tranfers and delayed decoding support in the transport.  For
 *      duplex channels, full coordination is required between client and
 *      server tranpsorts sharing an underlying bytestream (Matt).
 */

/*
 * Create a client handle for a connection.
 * Default options are set, which the user can change using clnt_control()'s.
 * The rpc/vc package does buffering similar to stdio, so the client
 * must pick send and receive buffer sizes, 0 => use the default.
 * NB: fd is copied into a private area.
 * NB: The rpch->cl_auth is set null authentication. Caller may wish to
 * set this something more useful.
 *
 * fd should be an open socket
 */
CLIENT *
clnt_vc_ncreatef(const int fd,	/* open file descriptor */
		 const struct netbuf *raddr,	/* servers address */
		 const rpcprog_t prog,	/* program number */
		 const rpcvers_t vers,	/* version number */
		 const u_int sendsz,	/* buffer send size */
		 const u_int recvsz,	/* buffer recv size */
		 const uint32_t flags)
{
	struct ct_data *ct = clnt_vc_data_zalloc();
	CLIENT *clnt = &ct->ct_cx.cx_c;
	SVCXPRT *xprt;
	struct svc_vc_xprt *xd;
	struct rpc_msg call_msg;
	sigset_t mask, newmask;
	struct sockaddr_storage ss;
	XDR ct_xdrs[1];		/* temp XDR stream */
	socklen_t slen;

	clnt->cl_ops = clnt_vc_ops();

	if (raddr == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d called with missing servers address",
			__func__, fd);
		clnt->cl_error.re_status = RPC_UNKNOWNADDR;
		return (clnt);
	}
	if (sizeof(struct sockaddr_storage) < raddr->len) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d called with invalid address length"
			" (max %z < %u len)",
			__func__, fd,
			sizeof(struct sockaddr_storage),
			raddr->len);
		clnt->cl_error.re_status = RPC_UNKNOWNADDR;
		return (clnt);
	}

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	if (flags & CLNT_CREATE_FLAG_CONNECT) {
		slen = sizeof(ss);
		if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
			if (errno != ENOTCONN) {
				clnt->cl_error.re_status = RPC_SYSTEMERROR;
				clnt->cl_error.re_errno = errno;
				goto err;
			}
			if (connect
			    (fd, (struct sockaddr *)raddr->buf,
			     raddr->len) < 0) {
				clnt->cl_error.re_status = RPC_SYSTEMERROR;
				clnt->cl_error.re_errno = errno;
				goto err;
			}
			__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
				"%s: fd %d connected",
				__func__, fd);
		}
	}

	/* find or create shared fd state; ref+1 */
	xprt = svc_fd_ncreatef(fd, sendsz, recvsz, flags);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d svc_fd_ncreatef failed",
			__func__, fd);
		clnt->cl_error.re_status = RPC_TLIERROR;
		goto err;
	}
	xd = VC_DR(REC_XPRT(xprt));

	if (!xd->sx_dr.ev_p) {
		xprt->xp_dispatch.process_cb = clnt_vc_process;
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);
	}
	ct->ct_cx.cx_rec = &xd->sx_dr;

	memcpy(&ct->ct_raddr, raddr->buf, raddr->len);
	ct->ct_rlen = raddr->len;

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = xd->sx_dr.call_xid;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = prog;
	call_msg.cb_vers = vers;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(ct_xdrs, ct->ct_cx.cx_mcallc, MCALL_MSG_SIZE,
		      XDR_ENCODE);
	if (!xdr_callhdr(ct_xdrs, &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d xdr_callhdr failed",
			__func__, fd);
		clnt->cl_error.re_status = RPC_CANTENCODEARGS;
		XDR_DESTROY(ct_xdrs);
		goto err;
	}
	ct->ct_cx.cx_mpos = XDR_GETPOS(ct_xdrs);
	XDR_DESTROY(ct_xdrs);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
		"%s: fd %d completed",
		__func__, fd);
 err:
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (clnt);
}

/*
 * Get the transport handle for a given rpc_client.
 */
SVCXPRT *
clnt_vc_get_client_xprt(const struct rpc_client *clnt)
{
	if (clnt == NULL)
		return NULL;

	struct cx_data *cx = CX_DATA(clnt);

	if (cx->cx_rec)
		return &(cx->cx_rec->xprt);
	return NULL;
}

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 */
CLIENT *
clnt_vc_ncreate_svc(const SVCXPRT *xprt, const rpcprog_t prog,
		    const rpcvers_t vers, const uint32_t flags)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	return clnt_vc_ncreatef(xprt->xp_fd, &xprt->xp_remote.nb, prog, vers,
				xd->sx_dr.sendsz, xd->sx_dr.recvsz,
				flags | CLNT_CREATE_FLAG_SVCXPRT);
}

static enum xprt_stat
clnt_vc_process(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;

	__warnx(TIRPC_DEBUG_FLAG_WARN,
		"%s: %p fd %d unexpected CALL",
		__func__, xprt, xprt->xp_fd);
	return SVC_STAT(xprt);
}

static enum clnt_stat
clnt_vc_call(struct clnt_req *cc)
{
	CLIENT *clnt = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	SVCXPRT *xprt = &rec->xprt;
	struct xdr_ioq *xioq;
	XDR *xdrs;
	u_int32_t *uint32p;

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
		__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
			"%s: fd %d failed",
			__func__, xprt->xp_fd);
		XDR_DESTROY(xdrs);
		return (RPC_CANTENCODEARGS);
	}
	mutex_unlock(&clnt->cl_lock);

	xdrs->x_lib[1] = (void *)xprt;
	svc_ioq_write_submit(xprt, xioq);

	return (RPC_SUCCESS);
}

static bool
clnt_vc_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	return (xdr_free(xdr_res, res_ptr));
}

 /*ARGSUSED*/
static void
clnt_vc_abort(CLIENT *clnt)
{
}

static bool
clnt_vc_control(CLIENT *clnt, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct ct_data *ct = CT_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct netbuf *addr;
	u_int32_t *uint32p;
	bool rslt = true;

	/* always take recv lock first if taking together */
	rpc_dplx_rli(rec);
	mutex_lock(&clnt->cl_lock);

	/*
	 * By default, SVC_XPRT_FLAG_CLOSE is always false. It is user
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
	case CLGET_SERVER_ADDR:
		/* Now obsolete. Only for backward compatibility */
		(void)memcpy(info, &ct->ct_raddr, (size_t) ct->ct_rlen);
		break;
	case CLGET_FD:
		*(int *)info = rec->xprt.xp_fd;
		break;
	case CLGET_SVC_ADDR:
		/* The caller should not free this memory area */
		addr = (struct netbuf *)info;
		addr->buf = &ct->ct_raddr;
		addr->len = ct->ct_rlen;
		addr->maxlen = sizeof(ct->ct_raddr);
		break;

	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure
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
clnt_vc_destroy(CLIENT *clnt)
{
	struct cx_data *cx = CX_DATA(clnt);

	if (cx->cx_rec) {
		if (clnt->cl_flags & CLNT_FLAG_LOCAL) {
			/* Local client; destroy the xprt */
			SVC_DESTROY(&cx->cx_rec->xprt);
		}

		/* RELEASE after DESTROY in case an error case has already
		 * called DESTROY */
		SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
	}
	clnt_vc_data_free(CT_DATA(cx));
}

static struct clnt_ops *
clnt_vc_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock;
	sigset_t mask, newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_vc_call;
		ops.cl_abort = clnt_vc_abort;
		ops.cl_freeres = clnt_vc_freeres;
		ops.cl_destroy = clnt_vc_destroy;
		ops.cl_control = clnt_vc_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (&ops);
}
