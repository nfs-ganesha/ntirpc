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
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

/*
 * svc_raw.c,   This a toy for simple testing and timing.
 * Interface to create an rpc client and server in the same UNIX process.
 * This lets us similate rpc and get rpc (round trip) overhead, without
 * any interference from the kernel.
 *
 */
#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <stdlib.h>

#include <rpc/rpc.h>
#include <rpc/svc_rqst.h>
#include "rpc_raw.h"
#include "svc_internal.h"

#ifndef UDPMSGSIZE
#define UDPMSGSIZE 8800
#endif

/*
 * This is the "network" that we will be moving data over
 */
static struct rpc_raw_xprt *svc_raw_private;

extern mutex_t svcraw_lock;

static void svc_raw_ops(SVCXPRT *);

/*static*/ void
svc_raw_xprt_free(struct rpc_raw_xprt *srp)
{
	XDR_DESTROY(srp->raw_dr.ioq.xdrs);
	rpc_dplx_rec_destroy(&srp->raw_dr);
	mem_free(srp, sizeof(struct rpc_raw_xprt) + srp->raw_dr.maxrec);
}

static struct rpc_raw_xprt *
svc_raw_xprt_zalloc(size_t sz)
{
	struct rpc_raw_xprt *srp = mem_zalloc(sizeof(struct rpc_raw_xprt) + sz);

	/* Init SVCXPRT locks, etc */
	rpc_dplx_rec_init(&srp->raw_dr);
	xdr_ioq_setup(&srp->raw_dr.ioq);
	return (srp);
}

SVCXPRT *
svc_raw_ncreate(void)
{
	SVCXPRT *xprt;
	struct rpc_raw_xprt *srp;

	/* VARIABLES PROTECTED BY svcraw_lock: svc_raw_private, srp */
	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		srp = svc_raw_xprt_zalloc(UDPMSGSIZE);
		srp->raw_dr.xprt.xp_fd = FD_SETSIZE;
		svc_raw_private = srp;
	}
	xprt = &srp->raw_dr.xprt;

	svc_raw_ops(xprt);
/* XXX check and or fixme */
#if 0
	srp->server.xp_verf.oa_base = srp->verf_body;
#endif
	srp->raw_dr.sendsz =
	srp->raw_dr.recvsz =
	srp->raw_dr.maxrec = UDPMSGSIZE;

	xdrmem_create(srp->raw_dr.ioq.xdrs, srp->raw_buf, UDPMSGSIZE,
		      XDR_DECODE);

	svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
			    SVC_RQST_FLAG_CHAN_AFFINITY);
	mutex_unlock(&svcraw_lock);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_raw");
#endif

	return (xprt);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_raw_stat(SVCXPRT *xprt)
{
	return (XPRT_IDLE);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_raw_recv(SVCXPRT *xprt)
{
	struct rpc_raw_xprt *srp;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (XPRT_DIED);
	}
	mutex_unlock(&svcraw_lock);

	return svc_request(xprt, srp->raw_dr.ioq.xdrs);
}

static enum xprt_stat
svc_raw_decode(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;

	xdrs->x_op = XDR_DECODE;
	(void)XDR_SETPOS(xdrs, 0);
	rpc_msg_init(&req->rq_msg);

	if (!xdr_callmsg(xdrs, &req->rq_msg))
		return (XPRT_DIED);

	return (req->rq_xprt->xp_dispatch.process_cb(req));
}

 /*ARGSUSED*/
static enum xprt_stat
svc_raw_reply(struct svc_req *req)
{
	struct rpc_raw_xprt *srp;
	XDR *xdrs;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (XPRT_DIED);
	}
	mutex_unlock(&svcraw_lock);

	xdrs = srp->raw_dr.ioq.xdrs;
	xdrs->x_op = XDR_ENCODE;
	(void)XDR_SETPOS(xdrs, 0);
	if (!xdr_replymsg(xdrs, &req->rq_msg))
		return (XPRT_DIED);
	(void)XDR_GETPOS(xdrs);	/* called just for overhead */

	return (XPRT_IDLE);
}

 /*ARGSUSED*/
static void
svc_raw_unlink(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
}

static void
svc_raw_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
}

 /*ARGSUSED*/
static bool
svc_raw_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	return (false);
}

static void
svc_raw_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	extern mutex_t ops_lock;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_raw_recv;
		ops.xp_stat = svc_raw_stat;
		ops.xp_decode = svc_raw_decode;
		ops.xp_reply = svc_raw_reply;
		ops.xp_checksum = NULL;		/* optional */
		ops.xp_unlink = svc_raw_unlink;
		ops.xp_unref_user_data = NULL;	/* no default */
		ops.xp_destroy = svc_raw_destroy;
		ops.xp_control = svc_raw_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}
