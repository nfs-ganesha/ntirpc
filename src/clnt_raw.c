/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
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
 * clnt_raw.c
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * Memory based rpc for simple testing and timing.
 * Interface to create an rpc client and server in the same process.
 * This lets us similate rpc and get round trip overhead, without
 * any interference from the kernel.
 */
#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>

#include <rpc/rpc.h>
#include "rpc_raw.h"
#include "clnt_internal.h"

struct cm_data {
	struct cx_data cm_cx;
	XDR xdr_stream;
};
#define CM_DATA(p) (opr_containerof((p), struct cm_data, cm_cx))

static struct clnt_ops *clnt_raw_ops(void);

static void
clnt_raw_data_free(struct cm_data *cm)
{
	clnt_data_destroy(&cm->cm_cx);
	mem_free(cm, sizeof(struct cm_data));
}

static struct cm_data *
clnt_raw_data_zalloc(void)
{
	struct cm_data *cm = mem_zalloc(sizeof(struct cm_data));

	clnt_data_init(&cm->cm_cx);
	return (cm);
}

/*
 * Create a client handle for memory based rpc.
 */
CLIENT *
clnt_raw_ncreate(rpcprog_t prog, rpcvers_t vers)
{
	struct cm_data *cm = clnt_raw_data_zalloc();
	CLIENT *client = &cm->cm_cx.cx_c;
	SVCXPRT *xprt;
	struct rpc_msg call_msg;
	XDR xdrs[1];		/* temp XDR stream */

	client->cl_ops = clnt_raw_ops();

	/* find or create shared state; ref+1 */
	xprt = svc_raw_ncreate();
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: svc_raw_ncreatef failed",
			__func__);
		client->cl_error.re_status = RPC_TLIERROR;
		return (client);
	}

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = REC_XPRT(xprt)->call_xid;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = prog;
	call_msg.cb_vers = vers;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(xdrs, cm->cm_cx.cx_mcallc, MCALL_MSG_SIZE,
		      XDR_ENCODE);
	if (!xdr_callhdr(xdrs, &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: xdr_callhdr failed",
			__func__);
		client->cl_error.re_status = RPC_CANTENCODEARGS;
		XDR_DESTROY(xdrs);
		return (client);
	}
	cm->cm_cx.cx_mpos = XDR_GETPOS(xdrs);
	XDR_DESTROY(xdrs);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_RAW,
		"%s: completed",
		__func__);
	return (client);
}

static enum clnt_stat
clnt_raw_call(struct clnt_req *cc)
{
	CLIENT *clnt = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct cm_data *cm = CM_DATA(cx);
	XDR *xdrs = &cm->xdr_stream;
	u_int32_t *uint32p;

	cc->cc_error.re_status = RPC_SUCCESS;

	mutex_lock(&clnt->cl_lock);
	uint32p = (u_int32_t *)&cx->cx_mcallc[0];
	*uint32p = htonl(cc->cc_xid);

	/*
	 * send request
	 */
	if ((!XDR_PUTBYTES(xdrs, cx->cx_mcallc, cx->cx_mpos))
	    || (!XDR_PUTUINT32(xdrs, cc->cc_proc))
	    || (!AUTH_MARSHALL(cc->cc_auth, xdrs))
	    || (!(*cc->cc_call.proc) (xdrs, cc->cc_call.where))) {
		/* error case */
		mutex_unlock(&clnt->cl_lock);
		__warnx(TIRPC_DEBUG_FLAG_CLNT_RAW,
			"%s: failed",
			__func__);
		XDR_DESTROY(xdrs);
		return (RPC_CANTENCODEARGS);
	}
	(void)XDR_GETPOS(xdrs);	/* called just to cause overhead */
	mutex_unlock(&clnt->cl_lock);

#ifndef FIXME
	return(RPC_SUCCESS);
#else
	struct rpc_msg msg;
	struct rpc_err error;

	/*
	 * We have to call server input routine here because this is
	 * all going on in one process. Yuk.
	 */
	svc_getreq_common(FD_SETSIZE);

	/*
	 * get results
	 */
	xdrs->x_op = XDR_DECODE;
	XDR_SETPOS(xdrs, 0);
	msg.RPCM_ack.ar_verf = _null_auth;
	msg.RPCM_ack.ar_results.proc = cc->cc_msg.rm_xdr.proc;
	msg.RPCM_ack.ar_results.where = cc->cc_msg.rm_xdr.where;
	if (!xdr_replymsg(xdrs, &msg)) {
		/*
		 * It's possible for xdr_replymsg() to fail partway
		 * through its attempt to decode the result from the
		 * server. If this happens, it will leave the reply
		 * structure partially populated with dynamically
		 * allocated memory. (This can happen if someone uses
		 * clntudp_bufcreate() to create a CLIENT handle and
		 * specifies a receive buffer size that is too small.)
		 * This memory must be free()ed to avoid a leak.
		 */
		int op = xdrs->x_op;
		xdrs->x_op = XDR_FREE;
		(void) xdr_replymsg(xdrs, &msg);
		xdrs->x_op = op;
		return (RPC_CANTDECODERES);
	}
	_seterr_reply(&msg, &error);

	return (error.re_status);
#endif
}

/* ARGSUSED */
static bool
clnt_raw_freeres(CLIENT *cl, xdrproc_t xdr_res, void *res_ptr)
{
	return (xdr_free(xdr_res, res_ptr));
}

/*ARGSUSED*/
static void
clnt_raw_abort(CLIENT *cl)
{
}

/*ARGSUSED*/
static bool
clnt_raw_control(CLIENT *cl, u_int ui, void *str)
{
	return (false);
}

static void
clnt_raw_destroy(CLIENT *client)
{
	struct cx_data *cx = CX_DATA(client);

	if (cx->cx_rec) {
		SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
	}
	clnt_raw_data_free(CM_DATA(cx));
}

static struct clnt_ops *
clnt_raw_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock; /* XXXX does it need to be extern? */

	/* VARIABLES PROTECTED BY ops_lock: ops */

	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_raw_call;
		ops.cl_abort = clnt_raw_abort;
		ops.cl_freeres = clnt_raw_freeres;
		ops.cl_destroy = clnt_raw_destroy;
		ops.cl_control = clnt_raw_control;
	}
	mutex_unlock(&ops_lock);
	return (&ops);
}
