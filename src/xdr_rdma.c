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

#include "config.h"
#include <sys/cdefs.h>

#include "namespace.h"
#include <sys/types.h>

#include <netinet/in.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/xdr_ioq.h>
#include <rpc/rpc.h>
#include "un-namespace.h"

#include "svc_internal.h"
#include "rpc_rdma.h"

/* NOTA BENE: as in xdr_ioq.c, although indications of failure are returned,
 * they are rarely checked.
 */

#define RFC5666_BUFFER_SIZE (1024)
#define RPCRDMA_VERSION (1)

#ifndef rpcrdma_dump_msg
#define DUMP_BYTES_PER_GROUP (4)
#define DUMP_GROUPS_PER_LINE (4)
#define DUMP_BYTES_PER_LINE (DUMP_BYTES_PER_GROUP * DUMP_GROUPS_PER_LINE)

static void
rpcrdma_dump_msg(struct xdr_ioq_uv *data, char *comment, uint32_t xid)
{
	if (!__debug_flag(TIRPC_DEBUG_FLAG_XDR))
		return;

	char *buffer;
	uint8_t *datum = data->v.vio_head;
	int sized = ioquv_length(data);
	int buffered = (((sized / DUMP_BYTES_PER_LINE) + 1 /*partial line*/)
			* (12 /* heading */
			   + (((DUMP_BYTES_PER_GROUP * 2 /*%02X*/) + 1 /*' '*/)
			      * DUMP_GROUPS_PER_LINE)))
			+ 1 /*'\0'*/;
	int i = 0;
	int m = 0;

	xid = ntohl(xid);
	if (sized == 0) {
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"rpcrdma 0x%" PRIx32 "(%" PRIu32 ") %s?",
			xid, xid, comment);
		return;
	}
	buffer = (char *)mem_alloc(buffered);

	while (sized > i) {
		int j = sized - i;
		int k = j < DUMP_BYTES_PER_LINE ? j : DUMP_BYTES_PER_LINE;
		int l = 0;
		int r = sprintf(&buffer[m], "\n%10d:", i);	/* heading */

		if (r < 0)
			goto quit;
		m += r;

		for (; l < k; l++) {
			if (l % DUMP_BYTES_PER_GROUP == 0)
				buffer[m++] = ' ';

			r = sprintf(&buffer[m], "%02X", datum[i++]);
			if (r < 0)
				goto quit;
			m += r;
		}
	}
quit:
	buffer[m] = '\0';	/* in case of error */
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"rpcrdma 0x%" PRIx32 "(%" PRIu32 ") %s:%s\n",
		xid, xid, comment, buffer);
	mem_free(buffer, buffered);
}
#endif /* rpcrdma_dump_msg */

/*
** match RFC-5666bis as closely as possible
*/
struct xdr_rdma_segment {
	uint32_t handle;	/* Registered memory handle */
	uint32_t length;	/* Length of the chunk in bytes */
	uint64_t offset;	/* Chunk virtual address or offset */
};

struct xdr_read_list {
	uint32_t present;	/* 1 indicates presence */
	uint32_t position;	/* Position in XDR stream */
	struct xdr_rdma_segment target;
};

struct xdr_write_chunk {
	struct xdr_rdma_segment target;
};

struct xdr_write_list {
	uint32_t present;	/* 1 indicates presence */
	uint32_t elements;	/* Number of array elements */
	struct xdr_write_chunk entry[0];
};

struct rpc_rdma_header {
	uint32_t rdma_reads;
	uint32_t rdma_writes;
	uint32_t rdma_reply;
	/* rpc body follows */
};

struct rpc_rdma_header_nomsg {
	uint32_t rdma_reads;
	uint32_t rdma_writes;
	uint32_t rdma_reply;
};

enum rdma_proc {
	RDMA_MSG = 0,	/* An RPC call or reply msg */
	RDMA_NOMSG = 1,	/* An RPC call or reply msg - separate body */
	RDMA_ERROR = 4	/* An RPC RDMA encoding error */
};

enum rpcrdma_errcode {
	RDMA_ERR_VERS = 1,
	RDMA_ERR_BADHEADER = 2
};

struct rpcrdma_err_vers {
	uint32_t rdma_vers_low;
	uint32_t rdma_vers_high;
};

struct rdma_msg {
	uint32_t rdma_xid;	/* Mirrors the RPC header xid */
	uint32_t rdma_vers;	/* Version of this protocol */
	uint32_t rdma_credit;	/* Buffers requested/granted */
	uint32_t rdma_type;	/* Type of message (enum rdma_proc) */
	union {
		struct rpc_rdma_header		rdma_msg;
		struct rpc_rdma_header_nomsg	rdma_nomsg;
	} rdma_body;
};

/***********************/
/****** Callbacks ******/
/***********************/

/* note parameter order matching svc.h svc_req callbacks */

static void
xdr_rdma_callback_signal(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int32_t read_waits = atomic_dec_int32_t(&cbc->read_waits);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s xprt %p cbc %p cbc read_waits %d "
	    "read_waits %d",
	    __func__, rdma_xprt, cbc, cbc->read_waits, read_waits);

	if (!read_waits) {
		pthread_mutex_lock(&cbc->cb_done_mutex);
		pthread_cond_signal(&cbc->cb_done);
		pthread_mutex_unlock(&cbc->cb_done_mutex);
	}
}

static int
xdr_rdma_respond_callback_send(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;
	int32_t write_waits = atomic_dec_int32_t(&cbc->write_waits);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
	    "%s() %p[%u] cbc %p refs %d write_waits %d\n",
	    __func__, rdma_xprt, rdma_xprt->state, cbc,
	    cbc->refcnt, write_waits);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
		    "refs %d write_waits %d already destroyed",
		    __func__, rdma_xprt, cbc, cbc->refcnt, write_waits);

		ret =  -1;
	}

	cbc_release_it(cbc);

	return ret;
}

static int
xdr_rdma_destroy_callback_send(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;
	int write_waits = atomic_dec_int32_t(&cbc->write_waits);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
	    "%s() %p[%u] cbc %p refs %d write_waits %d\n",
	    __func__, rdma_xprt, rdma_xprt->state, cbc,
	    cbc->refcnt, write_waits);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
		    "refs %d write_waits %d already destroyed",
		    __func__, rdma_xprt, cbc, cbc->refcnt, write_waits);

		ret = -1;
	}

	cbc_release_it(cbc);

	SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

	return ret;
}

static int
xdr_rdma_respond_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() %p[%u] cbc %p\n",
		__func__, rdma_xprt, rdma_xprt->state, cbc);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
			"already destroyed", __func__, rdma_xprt, cbc);

		ret = -1;
	}

	xdr_rdma_callback_signal(cbc, rdma_xprt);

	cbc_release_it(cbc);

	return ret;
}

static int
xdr_rdma_destroy_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() %p[%u] cbc %p\n",
		__func__, rdma_xprt, rdma_xprt->state, cbc);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
			"already destroyed", __func__, rdma_xprt, cbc);

		ret = -1;
	}

	xdr_rdma_callback_signal(cbc, rdma_xprt);

	cbc_release_it(cbc);

	SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

	return ret;
}

static int
xdr_rdma_destroy_callback_recv(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"Error in recv callback %s() %p[%u] cbc %p\n",
		__func__, rdma_xprt, rdma_xprt->state, cbc);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
			"already destroyed", __func__, rdma_xprt, cbc);

		ret = -1;
	}

	cbc->cbc_flags = CBC_FLAG_RELEASE;

	/* Release senital ref */
	cbc_release_it(cbc);

	SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

	return ret;
}

/**
 * xdr_rdma_wrap_callback: send/recv callback converts enum to int.
 *
 */
static int
xdr_rdma_wrap_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int ret = 0;

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
			"already destroyed", __func__, rdma_xprt, cbc);

		ret = -1;
		goto err;
	}

	/* Use xdrs from recvq in context */
	XDR *xdrs = cbc->recvq.xdrs;

	atomic_inc_uint32_t(&rdma_xprt->active_requests);

	ret = (int)svc_request(&rdma_xprt->sm_dr.xprt, xdrs);

	atomic_dec_uint32_t(&rdma_xprt->active_requests);

err:
	cbc->cbc_flags = CBC_FLAG_RELEASE;

	/* Release senital ref */
	cbc_release_it(cbc);

	return ret;
}

/***********************************/
/***** Utilities from Mooshika *****/
/***********************************/

/**
 * xdr_rdma_post_recv_n: Post receive chunk(s).
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] rdma_xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to register
 *
 * Must be set in advance:
 * @param[IN] positive_cb	function that'll be called when done
 * @param[IN] negative_cb	function that'll be called on error
 * @param[IN] callback_arg	argument to give to the callback

 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_recv_n(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	struct poolq_entry *have = TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh);
	int i = 0;
	int ret;

	if (!rdma_xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() rdma_xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (rdma_xprt->state) {
	case RDMAXS_CONNECTED:
	case RDMAXS_ROUTE_RESOLVED:
	case RDMAXS_CONNECT_REQUEST:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting recv",
			__func__, rdma_xprt, rdma_xprt->state, cbc);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] != "
			"connect request, connected, or resolved",
			__func__, rdma_xprt, rdma_xprt->state);
		return EINVAL;
	}

	while (have && i < sge) {
		struct ibv_mr *mr = IOQ_(have)->u.uio_p2;

		if (!mr) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Missing mr: Not requesting addr %p",
				__func__, IOQ_(have)->v.vio_head);
			return EINVAL;
		}

		cbc->sg_list[i].addr = (uintptr_t)(IOQ_(have)->v.vio_head);
		cbc->sg_list[i].length = ioquv_length(IOQ_(have));
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %" PRIx64 ", %" PRIu32 " [%" PRIx32 "]",
			__func__,
			cbc->sg_list[i].addr,
			cbc->sg_list[i].length,
			mr->lkey);
		cbc->sg_list[i++].lkey = mr->lkey;

		have = TAILQ_NEXT(have, q);

		if (i < sge)
			assert(have);
	}

	cbc->wr.rwr.next = NULL;
	cbc->wr.rwr.wr_id = (uintptr_t)cbc;
	cbc->wr.rwr.sg_list = cbc->sg_list;
	cbc->wr.rwr.num_sge = i;

	if (rdma_xprt->srq)
		ret = ibv_post_srq_recv(rdma_xprt->srq, &cbc->wr.rwr,
					&rdma_xprt->bad_recv_wr);
	else
		ret = ibv_post_recv(rdma_xprt->qp, &cbc->wr.rwr,
					&rdma_xprt->bad_recv_wr);

	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_recv failed: %s (%d)",
			__func__, rdma_xprt, rdma_xprt->state, cbc, strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * xdr_rdma_post_recv_cb: Post receive chunk(s) with standard callbacks.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] rdma_xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to register
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_recv_cb(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	cbc->positive_cb = xdr_rdma_wrap_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback_recv;
	cbc->callback_arg = NULL;
	cbc->call_inline = 0;

	SVC_REF(&rdma_xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);

	int ret = xdr_rdma_post_recv_n(rdma_xprt, cbc, sge);

	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s post_recv failed rdma_xprt %p "
			"cbc %p error %d", __func__, rdma_xprt, cbc, ret);

		cbc->cbc_flags = CBC_FLAG_RELEASE;

		/* Release senital ref */
		cbc_release_it(cbc);

		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);
	}

	return ret;
}

/**
 * Post a work chunk.
 *
 * @param[IN] rdma_xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to send
 * @param[IN] rs	remote segment
 * @param[IN] opcode
 *
 * Must be set in advance:
 * @param[IN] positive_cb	function that'll be called when done
 * @param[IN] negative_cb	function that'll be called on error
 * @param[IN] callback_arg	argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_send_n(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge,
		     struct xdr_rdma_segment *rs, enum ibv_wr_opcode opcode)
{
	struct poolq_entry *have = cbc->have;

	uint32_t totalsize = 0;
	int i = 0;
	int ret;

	if (!rdma_xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() rdma_xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (rdma_xprt->state) {
	case RDMAXS_CONNECTED:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting a send with op %d",
			__func__, rdma_xprt, rdma_xprt->state, cbc, opcode);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] != "
			"connected",
			__func__, rdma_xprt, rdma_xprt->state);
		return EINVAL;
	}

	/* opcode-specific checks */
	switch (opcode) {
	case IBV_WR_RDMA_WRITE:
	case IBV_WR_RDMA_READ:
		if (!rs) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Cannot do rdma without a remote location!",
				__func__);
			return EINVAL;
		}
		break;
	case IBV_WR_SEND:
		break;
	case IBV_WR_SEND_WITH_IMM:
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() unsupported op code: %d",
			__func__, opcode);
		return EINVAL;
	}

	while (have && i < sge) {
		struct ibv_mr *mr = IOQ_(have)->u.uio_p2;
		uint32_t length = ioquv_length(IOQ_(have));

		if (!length) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() Empty buffer: sending.",
				__func__);
		}
		if (!mr) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Missing mr: Not sending.",
				__func__);
			return EINVAL;
		}

		cbc->sg_list[i].addr = (uintptr_t)(IOQ_(have)->v.vio_head);
		cbc->sg_list[i].length = length;
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %" PRIx64 ", %" PRIu32 " [%" PRIx32 "]",
			__func__,
			cbc->sg_list[i].addr,
			cbc->sg_list[i].length,
			mr->lkey);
		cbc->sg_list[i++].lkey = mr->lkey;

		totalsize += length;
		have = TAILQ_NEXT(have, q);

		if (i < sge)
			assert(have);
	}

	cbc->wr.wwr.next = NULL;
	cbc->wr.wwr.wr_id = (uint64_t)cbc;
	cbc->wr.wwr.opcode = opcode;
	cbc->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	cbc->wr.wwr.sg_list = cbc->sg_list;
	cbc->wr.wwr.num_sge = i;

	if (rs) {
		cbc->wr.wwr.wr.rdma.rkey = ntohl(rs->handle);
		cbc->wr.wwr.wr.rdma.remote_addr =
			xdr_decode_hyper(&rs->offset);

		if (ntohl(rs->length) < totalsize) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() chunk bigger than the remote buffer "
				"(%" PRIu32 ">%" PRIu32 ")",
				__func__, totalsize, ntohl(rs->length));
			return EMSGSIZE;
		} else {
			/* save in place for posterity */
			rs->length = htonl(totalsize);
		}
	}

	ret = ibv_post_send(rdma_xprt->qp, &cbc->wr.wwr, &rdma_xprt->bad_send_wr);
	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_send failed: %s (%d), "
			"num sge %d imm_data %d",
			__func__, rdma_xprt, rdma_xprt->state, cbc, strerror(ret), ret,
			cbc->wr.wwr.num_sge, cbc->wr.wwr.imm_data);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

static inline int
xdr_rdma_wait_cb_done_locked(struct rpc_rdma_cbc *cbc)
{
	struct timespec ts;
	ts.tv_sec = time(NULL) + RDMA_CB_TIMEOUT_SEC;
	ts.tv_nsec = 0;
	RDMAXPRT *rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
		    "already destroyed", __func__, rdma_xprt, cbc);
	}

	/* cond_wait should atomically release cb_done_mutex */
	return pthread_cond_timedwait(&cbc->cb_done,
		    &cbc->cb_done_mutex, &ts);

}

/**
 * Post a work chunk with standard callbacks.
 *
 * @param[IN] rdma_xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to send
 *
 * @return 0 on success, the value of errno on error
 */
static inline int
xdr_rdma_async_send_cb(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	int ret;

	cbc->positive_cb = xdr_rdma_respond_callback_send;
	cbc->negative_cb = xdr_rdma_destroy_callback_send;
	cbc->callback_arg = NULL;
	cbc->call_inline = 1;
	int32_t write_waits = atomic_dec_int32_t(&cbc->write_waits);

	cbc_ref_it(cbc, rdma_xprt);

	ret = xdr_rdma_post_send_n(rdma_xprt, cbc, sge, NULL, IBV_WR_SEND);

	if (ret) {
		cbc_release_it(cbc);

		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

		/* Assuming there won't be callback */
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: failed ret %d err %d "
		    " rdma_xprt %p cbc %p refs %d write_waits %d",
		    __func__, ret, errno, rdma_xprt, cbc, cbc->refcnt, write_waits);
	}

	return ret;
}

static int
xdr_rdma_wait_read_cb(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge,
		     struct xdr_rdma_segment *rs)
{
	int ret;

	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = NULL;
	cbc->call_inline = 1;

	cbc_ref_it(cbc, rdma_xprt);
	int32_t read_waits = atomic_inc_int32_t(&cbc->read_waits);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s xprt %p cbc %p cbc read_waits %d "
	    "read_waits %d",
	    __func__, rdma_xprt, cbc, cbc->read_waits, read_waits);

	ret = xdr_rdma_post_send_n(rdma_xprt, cbc, sge, rs, IBV_WR_RDMA_READ);

	if (ret) {
		read_waits = atomic_dec_int32_t(&cbc->read_waits);

		cbc_release_it(cbc);
		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

		/* Assuming there won't be callback */
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: failed ret %d err %d "
		    " rdma_xprt %p cbc %p refs %d read_waits %d",
		    __func__, ret, errno, rdma_xprt, cbc, cbc->refcnt, read_waits);
	}

	return ret;
}

static int
xdr_rdma_async_write_cb(RDMAXPRT *rdma_xprt, struct rpc_rdma_cbc *cbc, int sge,
		      struct xdr_rdma_segment *rs)
{
	int ret;

	cbc->positive_cb = xdr_rdma_respond_callback_send;
	cbc->negative_cb = xdr_rdma_destroy_callback_send;
	cbc->callback_arg = NULL;
	cbc->call_inline = 1;
	int32_t write_waits = atomic_inc_int32_t(&cbc->write_waits);

	cbc_ref_it(cbc, rdma_xprt);

	ret = xdr_rdma_post_send_n(rdma_xprt, cbc, sge, rs, IBV_WR_RDMA_WRITE);

	if (ret) {
		write_waits = atomic_dec_int32_t(&cbc->write_waits);

		cbc_release_it(cbc);

		SVC_DESTROY(&rdma_xprt->sm_dr.xprt);

		/* Assuming there won't be callback */
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: failed ret %d err %d "
		    " rdma_xprt %p cbc %p refs %d write_waits %d",
		    __func__, ret, errno, rdma_xprt, cbc, cbc->refcnt,
		    write_waits);
	}

	return ret;
}

/***********************************/
/****** Utilities for rpcrdma ******/
/***********************************/

#define m_(ptr) ((struct rdma_msg *)ptr)
#define rl(ptr) ((struct xdr_read_list*)ptr)

typedef struct xdr_write_list wl_t;
#define wl(ptr) ((struct xdr_write_list*)ptr)

static inline void
xdr_rdma_skip_read_list(uint32_t **pptr)
{
	uint32_t *ptr1 = *pptr;
	while (rl(*pptr)->present) {
		*pptr += sizeof(struct xdr_read_list)
			 / sizeof(**pptr);
	}
	(*pptr)++;

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: ptr1 %p ptr2 %p diff %d", __func__, ptr1, *pptr, (*pptr) - ptr1);
}

static inline void
xdr_rdma_skip_write_list(uint32_t **pptr)
{
	uint32_t *ptr1 = *pptr;
	if (wl(*pptr)->present) {
		*pptr += (sizeof(struct xdr_write_list)
			  + sizeof(struct xdr_write_chunk)
			    * ntohl(wl(*pptr)->elements))
			 / sizeof(**pptr);
	}
	(*pptr)++;
	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: ptr1 %p ptr2 %p diff %d", __func__, ptr1, *pptr, (*pptr) - ptr1);
}

static inline void
xdr_rdma_skip_reply_array(uint32_t **pptr)
{
	uint32_t *ptr1 = *pptr;
	if (wl(*pptr)->present) {
		*pptr += (sizeof(struct xdr_write_list)
			  + sizeof(struct xdr_write_chunk)
			    * ntohl(wl(*pptr)->elements))
			 / sizeof(**pptr);
	} else {
		(*pptr)++;
	}
	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: ptr1 %p ptr2 %p diff %d", __func__, ptr1, *pptr, (*pptr) - ptr1);
}

static inline uint32_t *
xdr_rdma_get_read_list(void *data)
{
	return &m_(data)->rdma_body.rdma_msg.rdma_reads;
}

#ifdef UNUSED
static inline uint32_t *
xdr_rdma_get_write_array(void *data)
{
	uint32_t *ptr = xdr_rdma_get_read_list(data);

	xdr_rdma_skip_read_list(&ptr);

	return ptr;
}
#endif /* UNUSED */

static inline uint32_t *
xdr_rdma_get_reply_array(void *data)
{
	uint32_t *ptr = xdr_rdma_get_read_list(data);

	xdr_rdma_skip_read_list(&ptr);
	xdr_rdma_skip_write_list(&ptr);

	return ptr;
}

static inline uint32_t *
xdr_rdma_skip_header(struct rdma_msg *rmsg)
{
	uint32_t *ptr = &rmsg->rdma_body.rdma_msg.rdma_reads;

	xdr_rdma_skip_read_list(&ptr);
	xdr_rdma_skip_write_list(&ptr);
	xdr_rdma_skip_reply_array(&ptr);

	return ptr;
}

static inline uintptr_t
xdr_rdma_header_length(struct rdma_msg *rmsg)
{
	uint32_t *ptr = xdr_rdma_skip_header(rmsg);

	return ((uintptr_t)ptr - (uintptr_t)rmsg);
}

void
xdr_rdma_encode_error(struct xdr_ioq_uv *call_uv, enum rpcrdma_errcode err)
{
	struct rdma_msg *cmsg = m_(call_uv->v.vio_head);
	uint32_t *va = &cmsg->rdma_type;

	*va++ = htonl(RDMA_ERROR);
	*va++ = htonl(err);

	switch (err) {
	case RDMA_ERR_VERS:
		*va++ = htonl(RPCRDMA_VERSION);
		*va++ = htonl(RPCRDMA_VERSION);
		break;
	case RDMA_ERR_BADHEADER:
		break;
	}
	call_uv->v.vio_tail = (uint8_t *)va;
}

/* post recv buffers */
void
xdr_rdma_callq(RDMAXPRT *rdma_xprt)
{
	/* Get context buf from cbqh and add to sm_dr
	 * rpc_rdma_allocate->xdr_ioq_setup(&rdma_xprt->sm_dr.ioq);
	 * Check if we have credits availabled from cbqh
	 * rpc_rdma_setup_cbq
	 * rc = rpc_rdma_setup_cbq(&rdma_xprt->cbqh,
	 * rdma_xprt->xa->rq_depth + rdma_xprt->xa->sq_depth, rdma_xprt->xa->credits);
	 * Remove from cbqh and add to sm_dr.ioq */

	struct poolq_entry *have =
		xdr_rdma_ioq_uv_fetch(&rdma_xprt->sm_dr.ioq, &rdma_xprt->cbqh,
				      "callq context", 1, IOQ_FLAG_NONE);
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)(_IOQ(have));

	cbc->recvq.xdrs[0].x_lib[1] =
	cbc->sendq.xdrs[0].x_lib[1] =
	cbc->dataq.xdrs[0].x_lib[1] =
	cbc->freeq.xdrs[0].x_lib[1] = rdma_xprt;

	/* Get recv buf from inbufs_hdr and  add to context recvq */
	have = xdr_rdma_ioq_uv_fetch(&cbc->recvq, &rdma_xprt->inbufs_hdr.uvqh,
				"callq buffer", 1, IOQ_FLAG_NONE);

	/* input positions */
	IOQ_(have)->v.vio_head = IOQ_(have)->v.vio_base;
	IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_wrap;
	IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
			       + rdma_xprt->sm_dr.recv_hdr_sz;

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc_track start %p recvq %p %d "
		"sendq %p %d dataq %p %d freeq %p %d ioq %p %d rdma_xprt %p",
		__func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount,
		&cbc->sendq, cbc->sendq.ioq_uv.uvqh.qcount, &cbc->dataq,
		cbc->dataq.ioq_uv.uvqh.qcount, &cbc->freeq,
		cbc->freeq.ioq_uv.uvqh.qcount, &rdma_xprt->sm_dr.ioq,
		rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qcount, rdma_xprt);

	pthread_mutex_lock(&rdma_xprt->cbclist.qmutex);

	cbc->call_inline = 0;
	cbc->data_chunk_uv = NULL;
	cbc->refcnt = 1; // Senital ref
	cbc->cbc_flags = CBC_FLAG_NONE;
	cbc->read_waits = 0;
	cbc->write_waits = 0;
	cbc->active = false;
	cbc->non_registered_buf = NULL;
	cbc->non_registered_buf_len = 0;

	TAILQ_INSERT_TAIL(&rdma_xprt->cbclist.qh, &cbc->cbc_list, q);
	rdma_xprt->cbclist.qcount++;
	pthread_mutex_unlock(&rdma_xprt->cbclist.qmutex);

	/* rdma_xprt ref is taken by xdr_rdma_post_recv_cb */
	if (xdr_rdma_post_recv_cb(rdma_xprt, cbc, 1)) {
		 __warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: recv failed %p",
			__func__, rdma_xprt);
	}
}

/****************************/
/****** Main functions ******/
/****************************/

/* Split b_addr into buf_count chunks of size buf_size and
 * add it to uv_head */
static void
xdr_rdma_add_bufs_locked(RDMAXPRT *rdma_xprt, struct ibv_mr *mr,
    struct xdr_ioq_uv_head *uv_head, xdr_ioq_uv_type_t buf_type,
    uint32_t buf_size, uint32_t buf_count, uint8_t *b_addr,
    struct rpc_io_bufs *io_buf)
{
	/* Each pre-allocated buffer has a corresponding xdr_ioq_uv,
	 * stored on the pool queues.
	 */

	for (int qcount = 0;
	     qcount < buf_count;
	     qcount++) {
		struct xdr_ioq_uv *data = xdr_ioq_uv_create(0, UIO_FLAG_BUFQ);

		assert(data);

		data->rdma_uv = 1;

		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() recvbuf at %p base %p",
			__func__, data, b_addr);

		data->v.vio_base =
		data->v.vio_head =
		data->v.vio_tail = b_addr;
		data->v.vio_wrap = (char *)b_addr + buf_size;
		data->rdma_v = data->v;
		data->u.uio_p1 = &uv_head->uvqh;
		data->u.uio_p2 = mr;
		data->u.uio_u1 = io_buf;
		data->rdma_u = data->u;
		data->uv_type = buf_type;
		TAILQ_INSERT_TAIL(&uv_head->uvqh.qh, &data->uvq, q);
		uv_head->uvqh.qcount++;
		atomic_inc_uint64_t(&io_buf->buf_count);

		b_addr += buf_size;
	}
}

/* Split b_addr into buf_count chunks of size buf_size and
 * add it to uv_head */
static void
xdr_rdma_add_bufs(RDMAXPRT *rdma_xprt, struct ibv_mr *mr,
    struct xdr_ioq_uv_head *uv_head, xdr_ioq_uv_type_t buf_type,
    uint32_t buf_size, uint32_t buf_count, uint8_t *b_addr,
    struct rpc_io_bufs *io_buf)
{

	/* Each pre-allocated buffer has a corresponding xdr_ioq_uv,
	 * stored on the pool queues.
	 */

	pthread_mutex_lock(&uv_head->uvqh.qmutex);

	xdr_rdma_add_bufs_locked(rdma_xprt, mr, uv_head, buf_type,
	    buf_size, buf_count, b_addr, io_buf);

	pthread_mutex_unlock(&uv_head->uvqh.qmutex);
}

/* Add on allocated buffers to the io_bufs */
static struct rpc_io_bufs *
xdr_rdma_update_io_bufs(RDMAXPRT *rdma_xprt, struct ibv_mr *mr, uint32_t buffer_total,
    uint8_t *buffer_aligned, rpc_io_buf_type_t type)
{

	pthread_mutex_lock(&rdma_xprt->io_bufs.qmutex);

	rdma_xprt->io_bufs.qsize = sizeof(struct rpc_io_bufs);

	struct rpc_io_bufs *io_buf =
	    mem_zalloc(rdma_xprt->io_bufs.qsize);

	assert(io_buf);

	io_buf->mr = mr;
	io_buf->buffer_total = buffer_total;
	io_buf->buffer_aligned = buffer_aligned;
	io_buf->type = type;
	io_buf->refs = 0;
	io_buf->ready = false;
	io_buf->ctx = (void *)rdma_xprt;

	TAILQ_INSERT_TAIL(&rdma_xprt->io_bufs.qh, &io_buf->q, q);

	if (rdma_xprt->io_bufs_count == 0)
		rdma_xprt->first_io_buf = io_buf;

	rdma_xprt->io_bufs_count++;
	rdma_xprt->io_bufs.qcount++;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() io bufs count %u io_buf %p rdma_xprt %p",
		__func__, rdma_xprt->io_bufs_count, io_buf, rdma_xprt);

	pthread_mutex_unlock(&rdma_xprt->io_bufs.qmutex);

	return io_buf;
}

uint64_t total_rdma_reg_mem;

static struct ibv_mr *
xdr_rdma_reg_mr(RDMAXPRT *rdma_xprt, uint8_t *buffer_aligned, uint32_t buffer_total)
{
	struct ibv_mr *mr = NULL;

	mr = ibv_reg_mr(rdma_xprt->pd->pd, buffer_aligned, buffer_total,
			    IBV_ACCESS_LOCAL_WRITE |
			    IBV_ACCESS_REMOTE_WRITE |
			    IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

	if (mr) {
		atomic_add_uint64_t(&total_rdma_reg_mem, buffer_total);

		__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s: total_rdma_reg_mem %llu registered, "
		    "registering for xprt %p mr %p buffer_aligned %p buffer_total %u",
		    __func__, atomic_fetch_uint64_t(&total_rdma_reg_mem),
		    rdma_xprt, mr, buffer_aligned, buffer_total);
	} else {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: ibv_reg_mr failed "
		    "total_rdma_reg_mem %llu registered, "
		    "registering for xprt %p mr %p buffer_aligned %p buffer_total %u",
		    __func__, atomic_fetch_uint64_t(&total_rdma_reg_mem),
		    rdma_xprt, mr, buffer_aligned, buffer_total);
	}

	return mr;
}

int
xdr_rdma_dereg_mr(RDMAXPRT *rdma_xprt, struct ibv_mr *mr,
    uint8_t *buffer_aligned, uint32_t buffer_total)
{
	int ret;


	ret = ibv_dereg_mr(mr);

	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s: ibv_dereg_mr failed err %d "
		    "total_rdma_reg_mem %llu xprt %p mr %p "
		    "buffer_aligned %p buffer_total %u",
		    __func__, ret, atomic_fetch_uint64_t(&total_rdma_reg_mem),
		    rdma_xprt, mr, buffer_aligned, buffer_total);
	} else {
		atomic_sub_uint64_t(&total_rdma_reg_mem, buffer_total);

		__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s: total_rdma_reg_mem %llu registered, "
		    "unregistering for xprt %p mr %p buffer_aligned %p buffer_total %u",
		    __func__, atomic_fetch_uint64_t(&total_rdma_reg_mem),
		    rdma_xprt, mr, buffer_aligned, buffer_total);
	}

	return ret;
}

void
xdr_rdma_add_outbufs_hdr(RDMAXPRT *rdma_xprt)
{
	uint8_t *b;
	int hdr_qdepth = RDMA_HDR_CHUNKS(rdma_xprt->xa);
	uint32_t buffer_total = rdma_xprt->sm_dr.send_hdr_sz * hdr_qdepth;
	struct rpc_io_bufs *io_buf = NULL;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_total %llu, sendsz %llu sq %llu rdma_xprt %p pagesz %llu",
		__func__, buffer_total, rdma_xprt->sm_dr.send_hdr_sz, hdr_qdepth,
		rdma_xprt, rdma_xprt->sm_dr.pagesz);

	uint8_t *buffer_aligned = mem_aligned(rdma_xprt->sm_dr.pagesz, buffer_total);

	assert(buffer_aligned);
	memset(buffer_aligned, 0, buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_aligned at %p protection domain %p rdma_xprt %p",
		__func__, buffer_aligned, rdma_xprt->pd->pd, rdma_xprt);

	struct ibv_mr *mr = xdr_rdma_reg_mr(rdma_xprt, buffer_aligned, buffer_total);

	assert(mr);

	b = buffer_aligned;

	io_buf = xdr_rdma_update_io_bufs(rdma_xprt, mr, buffer_total, buffer_aligned,
	    IO_OUTBUF_HDR);

	xdr_rdma_add_bufs_locked(rdma_xprt, mr, &rdma_xprt->outbufs_hdr, UV_HDR,
	    rdma_xprt->sm_dr.send_hdr_sz, hdr_qdepth, b, io_buf);

	io_buf->ready = true;
}

void
xdr_rdma_add_outbufs_data(RDMAXPRT *rdma_xprt)
{
	uint8_t *b;
	int data_qdepth = RDMA_DATA_CHUNKS;
	uint32_t buffer_total = rdma_xprt->sm_dr.sendsz * data_qdepth;
	struct rpc_io_bufs *io_buf = NULL;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_total %llu, sendsz %llu sq %llu rdma_xprt %p pagesz %llu",
		__func__, buffer_total, rdma_xprt->sm_dr.sendsz, data_qdepth,
		rdma_xprt, rdma_xprt->sm_dr.pagesz);

	uint8_t *buffer_aligned = mem_aligned(rdma_xprt->sm_dr.pagesz, buffer_total);

	assert(buffer_aligned);
	memset(buffer_aligned, 0, buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_aligned at %p protection domain %p rdma_xprt %p",
		__func__, buffer_aligned, rdma_xprt->pd->pd, rdma_xprt);

	struct ibv_mr *mr = xdr_rdma_reg_mr(rdma_xprt, buffer_aligned, buffer_total);

	assert(mr);

	b = buffer_aligned;

	io_buf = xdr_rdma_update_io_bufs(rdma_xprt, mr, buffer_total, buffer_aligned,
	    IO_OUTBUF_DATA);

	xdr_rdma_add_bufs_locked(rdma_xprt, mr, &rdma_xprt->outbufs_data,
	    UV_DATA, rdma_xprt->sm_dr.sendsz, data_qdepth, b, io_buf);

	io_buf->ready = true;
}

void
xdr_rdma_add_inbufs_hdr(RDMAXPRT *rdma_xprt)
{
	uint8_t *b;
	int hdr_qdepth = RDMA_HDR_CHUNKS(rdma_xprt->xa);
	uint32_t buffer_total = rdma_xprt->sm_dr.recv_hdr_sz * hdr_qdepth;
	struct rpc_io_bufs *io_buf = NULL;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_total %llu, recvsz %llu rq %llu rdma_xprt %p pagesz %llu",
		__func__, buffer_total, rdma_xprt->sm_dr.recv_hdr_sz, hdr_qdepth,
		rdma_xprt, rdma_xprt->sm_dr.pagesz);

	uint8_t *buffer_aligned = mem_aligned(rdma_xprt->sm_dr.pagesz, buffer_total);

	assert(buffer_aligned);
	memset(buffer_aligned, 0, buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_aligned at %p protection domain %p rdma_xprt %p",
		__func__, buffer_aligned, rdma_xprt->pd->pd, rdma_xprt);

	struct ibv_mr *mr = xdr_rdma_reg_mr(rdma_xprt, buffer_aligned, buffer_total);

	assert(mr);

	b = buffer_aligned;

	io_buf = xdr_rdma_update_io_bufs(rdma_xprt, mr, buffer_total, buffer_aligned,
	    IO_INBUF_HDR);

	xdr_rdma_add_bufs_locked(rdma_xprt, mr, &rdma_xprt->inbufs_hdr, UV_HDR,
	    rdma_xprt->sm_dr.recv_hdr_sz, hdr_qdepth, b, io_buf);

	io_buf->ready = true;
}

void
xdr_rdma_add_inbufs_data(RDMAXPRT *rdma_xprt)
{
	uint8_t *b;
	int data_qdepth = RDMA_DATA_CHUNKS;
	uint32_t buffer_total = rdma_xprt->sm_dr.recvsz * data_qdepth;
	struct rpc_io_bufs *io_buf = NULL;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_total %llu, recvsz %llu rq %llu rdma_xprt %p pagesz %llu",
		__func__, buffer_total, rdma_xprt->sm_dr.recvsz, data_qdepth,
		rdma_xprt, rdma_xprt->sm_dr.pagesz);

	uint8_t *buffer_aligned = mem_aligned(rdma_xprt->sm_dr.pagesz, buffer_total);

	assert(buffer_aligned);
	memset(buffer_aligned, 0, buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_aligned at %p protection domain %p rdma_xprt %p",
		__func__, buffer_aligned, rdma_xprt->pd->pd, rdma_xprt);

	struct ibv_mr *mr = xdr_rdma_reg_mr(rdma_xprt, buffer_aligned, buffer_total);

	assert(mr);

	b = buffer_aligned;

	io_buf = xdr_rdma_update_io_bufs(rdma_xprt, mr, buffer_total, buffer_aligned,
	    IO_INBUF_DATA);

	xdr_rdma_add_bufs_locked(rdma_xprt, mr, &rdma_xprt->inbufs_data,
	    UV_DATA, rdma_xprt->sm_dr.recvsz, data_qdepth, b, io_buf);

	io_buf->ready = true;
}

/*
 * initializes a stream descriptor for a memory buffer.
 *
 * credits is the number of buffers used
 */
int
xdr_rdma_create(RDMAXPRT *rdma_xprt)
{
	uint8_t *b;
	int data_qdepth = RDMA_DATA_CHUNKS;
	int hdr_qdepth = RDMA_HDR_CHUNKS(rdma_xprt->xa);
	struct rpc_io_bufs *io_buf = NULL;

	if (!rdma_xprt->pd || !rdma_xprt->pd->pd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] missing Protection Domain",
			__func__, rdma_xprt, rdma_xprt->state);
		return ENODEV;
	}

	/* pre-allocated buffer_total:
	 * the number of credits is irrelevant here.
	 * We have kept credits same as outstanding.
	 * Since there could be more than one buffers
	 * required for each request, we need higher
	 * number of buffers than credits.
	 * instead, allocate buffers to match the read/write contexts.
	 * more than one buffer can be chained to one ioq_uv head,
	 * but never need more ioq_uv heads than buffers.
	 */
	size_t total_hdr_sz = RDMA_HDR_CHUNK_SZ * (RDMA_HDR_CHUNKS(rdma_xprt->xa) * 2);
	size_t tirpc_buff_total = rdma_xprt->sm_dr.recvsz * data_qdepth
				  + rdma_xprt->sm_dr.sendsz * data_qdepth;

	rdma_xprt->buffer_total = tirpc_buff_total + total_hdr_sz;

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_total %llu(%llu + %llu), rdma_xprt %p pagesz %llu "
		"recvsz data %llu hdr %llu rq %llu "
		"sendsz data %llu hdr %llu sq %llu",
		__func__, rdma_xprt->buffer_total,
		tirpc_buff_total, total_hdr_sz, rdma_xprt, rdma_xprt->sm_dr.pagesz,
		rdma_xprt->sm_dr.recvsz, rdma_xprt->sm_dr.recv_hdr_sz, data_qdepth,
		rdma_xprt->sm_dr.sendsz, rdma_xprt->sm_dr.send_hdr_sz, data_qdepth);

	rdma_xprt->buffer_aligned = mem_aligned(rdma_xprt->sm_dr.pagesz, rdma_xprt->buffer_total);

	assert(rdma_xprt->buffer_aligned);
	memset(rdma_xprt->buffer_aligned, 0, rdma_xprt->buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_EVENT,
		"%s() buffer_aligned at %p protection domain %p rdma_xprt %p",
		__func__, rdma_xprt->buffer_aligned, rdma_xprt->pd->pd, rdma_xprt);

	rdma_xprt->mr = xdr_rdma_reg_mr(rdma_xprt, rdma_xprt->buffer_aligned,
	    rdma_xprt->buffer_total);

	assert(rdma_xprt->mr);

	rdma_xprt->sm_dr.xprt.xp_rdma = true;

	poolq_head_setup(&rdma_xprt->inbufs_data.uvqh);
	rdma_xprt->inbufs_data.min_bsize = rdma_xprt->sm_dr.pagesz;
	rdma_xprt->inbufs_data.max_bsize = rdma_xprt->sm_dr.recvsz;

	poolq_head_setup(&rdma_xprt->outbufs_data.uvqh);
	rdma_xprt->outbufs_data.min_bsize = rdma_xprt->sm_dr.pagesz;
	rdma_xprt->outbufs_data.max_bsize = rdma_xprt->sm_dr.sendsz;

	poolq_head_setup(&rdma_xprt->inbufs_hdr.uvqh);
	rdma_xprt->inbufs_hdr.min_bsize = rdma_xprt->sm_dr.pagesz;
	rdma_xprt->inbufs_hdr.max_bsize = rdma_xprt->sm_dr.recv_hdr_sz;

	poolq_head_setup(&rdma_xprt->outbufs_hdr.uvqh);
	rdma_xprt->outbufs_hdr.min_bsize = rdma_xprt->sm_dr.pagesz;
	rdma_xprt->outbufs_hdr.max_bsize = rdma_xprt->sm_dr.send_hdr_sz;

	b = rdma_xprt->buffer_aligned;

	poolq_head_setup(&rdma_xprt->io_bufs);
	rdma_xprt->io_bufs_count = 0;

	io_buf = xdr_rdma_update_io_bufs(rdma_xprt, rdma_xprt->mr,
	    rdma_xprt->buffer_total,
	    rdma_xprt->buffer_aligned, IO_BUF_ALL);

	xdr_rdma_add_bufs(rdma_xprt, rdma_xprt->mr, &rdma_xprt->inbufs_data, UV_DATA,
	    rdma_xprt->sm_dr.recvsz, data_qdepth, b, io_buf);
	b = b + (data_qdepth * rdma_xprt->sm_dr.recvsz);

	xdr_rdma_add_bufs(rdma_xprt, rdma_xprt->mr, &rdma_xprt->outbufs_data, UV_DATA,
	    rdma_xprt->sm_dr.sendsz, data_qdepth, b, io_buf);
	b = b + (data_qdepth * rdma_xprt->sm_dr.sendsz);

	xdr_rdma_add_bufs(rdma_xprt, rdma_xprt->mr, &rdma_xprt->inbufs_hdr, UV_HDR,
	    rdma_xprt->sm_dr.recv_hdr_sz, hdr_qdepth, b, io_buf);
	b = b + (hdr_qdepth * rdma_xprt->sm_dr.recv_hdr_sz);

	xdr_rdma_add_bufs(rdma_xprt, rdma_xprt->mr, &rdma_xprt->outbufs_hdr, UV_HDR,
	    rdma_xprt->sm_dr.send_hdr_sz, hdr_qdepth, b, io_buf);
	b = b + (hdr_qdepth * rdma_xprt->sm_dr.send_hdr_sz);

	io_buf->ready = true;

	/* Keep enough cbc available to serve cbc allocation.
	 * We allocate MAX_CBC_OUTSTANDING * 2 cbcs.
	 * we could have max cbcs required will be callq_size * 2 */
	int callq_size = MAX_RECV_OUTSTANDING(rdma_xprt->xa);

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA, "callq size %d", callq_size);

	poolq_head_setup(&rdma_xprt->cbclist);

	while (rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qcount < callq_size) {
		/* Post callq_size buffers to do first recvs
		 * callback will be done on recv which should rearam again */
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() qcount %d callq size %d",
			__func__, rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qcount);
		xdr_rdma_callq(rdma_xprt);
	}

	return 0;
}

/** xdr_rdma_clnt_reply
 *
 * Client prepares for a reply
 *
 * potential output buffers are queued in recvq.
 *
 * @param[IN] xdrs	cm_data
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_reply(XDR *xdrs, u_int32_t xid)
{
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)xdrs;
	RDMAXPRT *rdma_xprt;
	struct xdr_write_list *reply_array;
	struct xdr_ioq_uv *work_uv;
	struct poolq_entry *have;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	rdma_xprt = x_rdma_xprt(xdrs);

	work_uv = IOQ_(TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh));
	rpcrdma_dump_msg(work_uv, "creply head", htonl(xid));

	reply_array = (wl_t *)xdr_rdma_get_reply_array(work_uv->v.vio_head);
	if (reply_array->present == 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() No reply/read array, failing miserably "
			"till writes/inlines are handled",
			__func__);
		return (false);
	} else {
		uint32_t i;
/*		uint32_t l; */
		uint32_t n = ntohl(reply_array->elements);

		for (i = 0; i < n; i++) {
			/* FIXME: xdr_rdma_getaddrbuf hangs instead of
			 * failing if no match. add a zero timeout
			 * when implemented
			 */
			have = xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->inbufs_data.uvqh,
				"creply body", 1, IOQ_FLAG_NONE);
			rpcrdma_dump_msg(IOQ_(have), "creply body", ntohl(xid));

			/* length < size if the protocol works out...
			 * FIXME: check anyway?
			 */
/*			l = ntohl(reply_array->entry[i].target.length); */
		}
	}

	xdr_ioq_reset(&cbc->sendq, 0);
	return (true);
}

/** xdr_rdma_svc_recv
 *
 * Server assembles a call request
 *
 * concatenates any rdma Read buffers for processing,
 * but clones call rdma header in place for future use.
 *
 * @param[IN] cbc	incoming request
 *			call request is in recvq
 *
 * called by svc_rdma_recv()
 */
bool
xdr_rdma_svc_recv(struct rpc_rdma_cbc *cbc, u_int32_t xid)
{
	RDMAXPRT *rdma_xprt;
	struct rdma_msg *cmsg;
	uint32_t l, offset = 0;
	bool status = true;
	struct xdr_ioq_uv *read_chunk_uv = NULL, *data_chunk_uv = NULL;
	struct xdr_ioq_uv *chunk_uv = NULL;


	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}

	/* Both sendq and recvq (xdrs)->x_lib[1] points to xprt */
	rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);

	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &rdma_xprt->sm_dr.recv.ts);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
		    "already destroyed", __func__, rdma_xprt, cbc);
		return false;
	}

        __warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d sendq %p %d "
		"dataq %p %d rdma_xprt %p",
                __func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
                cbc->sendq.ioq_uv.uvqh.qcount, &cbc->dataq, cbc->dataq.ioq_uv.uvqh.qcount,
		rdma_xprt);

	/* recvq will have request header part */
	assert(cbc->recvq.ioq_uv.uvqh.qcount == 1);
	assert(cbc->sendq.ioq_uv.uvqh.qcount == 0);
	assert(cbc->dataq.ioq_uv.uvqh.qcount == 0);
	assert(cbc->freeq.ioq_uv.uvqh.qcount == 0);
	assert(cbc->read_waits == 0);
	assert(cbc->write_waits == 0);

	/* Maintain max_outstanding */
	xdr_rdma_callq(rdma_xprt);

	/* Get inbuf from recvq */
	cbc->call_uv = IOQ_(TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh));
	cbc->call_head = cbc->call_uv->v.vio_head;
	cmsg = m_(cbc->call_head);
	rpcrdma_dump_msg(cbc->call_uv, "call", cmsg->rdma_xid);

	switch (ntohl(cmsg->rdma_vers)) {
	case RPCRDMA_VERSION:
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() rdma_vers %" PRIu32 "?",
			__func__, ntohl(cmsg->rdma_vers));
		xdr_rdma_encode_error(cbc->call_uv, RDMA_ERR_VERS);
		cbc->have = TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh);
		xdr_rdma_async_send_cb(rdma_xprt, cbc, 1);
		return (false);
	}

	switch (ntohl(cmsg->rdma_type)) {
	case RDMA_MSG:
	case RDMA_NOMSG:
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() rdma_type %" PRIu32 "?",
			__func__, ntohl(cmsg->rdma_type));
		xdr_rdma_encode_error(cbc->call_uv, RDMA_ERR_BADHEADER);
		cbc->have = TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh);
		xdr_rdma_async_send_cb(rdma_xprt, cbc, 1);
		return (false);
	}

	/* locate NFS/RDMA (RFC-5666) chunk positions */
	cbc->read_chunk = xdr_rdma_get_read_list(cmsg);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s:%d read chunk %p present %u position %u",
		__func__, __LINE__, cbc->read_chunk, rl(cbc->read_chunk)->present,
		rl(cbc->read_chunk)->position);

	cbc->write_chunk = (wl_t *)cbc->read_chunk;
	xdr_rdma_skip_read_list((uint32_t **)&cbc->write_chunk);
	cbc->reply_chunk = cbc->write_chunk;
	xdr_rdma_skip_write_list((uint32_t **)&cbc->reply_chunk);
	cbc->call_data = cbc->reply_chunk;
	xdr_rdma_skip_reply_array((uint32_t **)&cbc->call_data);

	struct xdr_write_list *reply_array = (wl_t *)cbc->reply_chunk;
	bool data_chunk = 0;

	/* We need data_chunk for read/readdir in 2 cases
	 * 1> read/readdir with reply/write list.
	 * 2> read/readdir without reply/write list.
	 * If reply/write list not present we allocate
	 * data_chunk by default since we don't know which request we
	 * received, in this case we allocate data_chunk of smaller
	 * size */
	if (reply_array->present == 0) {
		reply_array = (wl_t *)cbc->write_chunk;
		if (reply_array->present) {
			/* Allocate data_chunk of larger size if write_chunks
			 * are requested for rdma_write to serve NFS reads */
			data_chunk = 1;
		}
	} else {
		/* Allocate data_chunk of larget size if reply_chunks
		 * requested for rdma_write to serve NFS readdir */
		data_chunk = 1;
	}

	/* Allocate data buffer for read/readdir */
	if (data_chunk) {
		data_chunk_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->dataq,
		    &rdma_xprt->outbufs_data.uvqh,
		    "sreply data_chunk_L", 1, IOQ_FLAG_NONE));

		/* entry was already added directly to the queue */
		data_chunk_uv->v.vio_head = data_chunk_uv->v.vio_tail =
		    data_chunk_uv->v.vio_base;
		/* tail adjusted below */
		data_chunk_uv->v.vio_wrap = (char *)data_chunk_uv->v.vio_base +
		    rdma_xprt->sm_dr.sendsz;

		cbc->data_chunk_uv = data_chunk_uv;

		__warnx(TIRPC_DEBUG_FLAG_XDR, "data_chunk_uv data %p cbc %p",
		    data_chunk_uv, cbc);
	} else {
		/* For read/readdir without reply/write list, we should not need
		 * big buffer */
		data_chunk_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->dataq,
		    &rdma_xprt->outbufs_hdr.uvqh,
		    "sreply data_chunk_S", 1, IOQ_FLAG_NONE));

		/* entry was already added directly to the queue */
		data_chunk_uv->v.vio_head = data_chunk_uv->v.vio_tail =
		    data_chunk_uv->v.vio_base;
		/* tail adjusted below */
		data_chunk_uv->v.vio_wrap = (char *)data_chunk_uv->v.vio_base +
		    rdma_xprt->sm_dr.send_hdr_sz;

		cbc->data_chunk_uv = data_chunk_uv;

		__warnx(TIRPC_DEBUG_FLAG_XDR, "data_chunk_uv hdr %p cbc %p",
		    data_chunk_uv, cbc);
	}


	/* skip past the header for the calling buffer
	 * xdr_ioq_reset->xdr_ioq_uv_reset set the xdrs point to buf
	 * Set xdr in sendq for decoding.
	 * xioq->xdrs[0].x_data = uv->v.vio_head; */
	xdr_ioq_reset(&cbc->recvq, ((uintptr_t)cbc->call_data
				  - (uintptr_t)cmsg));

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s read chunk %p present %u "
		"position %u reply chunk %p reply chunk present %d write "
		"chunk %p write chunk present %d",
		__func__, cbc->read_chunk, rl(cbc->read_chunk)->present,
		rl(cbc->read_chunk)->position, cbc->reply_chunk,
		wl(cbc->reply_chunk)->present, cbc->write_chunk,
		wl(cbc->write_chunk)->present);

	pthread_mutex_lock(&cbc->recvq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&cbc->recvq.ioq_uv.uvqh.qh, &cbc->call_uv->uvq, q);
	(cbc->recvq.ioq_uv.uvqh.qcount)--;
	pthread_mutex_unlock(&cbc->recvq.ioq_uv.uvqh.qmutex);

	assert(cbc->recvq.ioq_uv.uvqh.qcount == 0);

	while (rl(cbc->read_chunk)->present && status) {
		l = ntohl(rl(cbc->read_chunk)->target.length);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() offset %u length %u",
			__func__, offset, l);

		if (!read_chunk_uv) {
			read_chunk_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->dataq,
			    &rdma_xprt->inbufs_data.uvqh,
			    "sreply rdma_read", 1, IOQ_FLAG_NONE));

			/* entry was already added directly to the queue */
			read_chunk_uv->v.vio_head = read_chunk_uv->v.vio_tail =
			    read_chunk_uv->v.vio_base;

			/* tail adjusted below */
			read_chunk_uv->v.vio_wrap = (char *)read_chunk_uv->v.vio_base +
			    rdma_xprt->sm_dr.recvsz;

			read_chunk_uv->v.vio_tail = (char *)read_chunk_uv->v.vio_head +
			    rdma_xprt->sm_dr.recvsz;
		}

		chunk_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->recvq,
		    &rdma_xprt->inbufs_hdr.uvqh,
		    "sreply rdma_read", 1, IOQ_FLAG_NONE));

		/* entry was already added directly to the queue */
		data_chunk_uv->v.vio_head = data_chunk_uv->v.vio_tail =
		    data_chunk_uv->v.vio_base;

		if (offset < rdma_xprt->sm_dr.recvsz) {
			/* entry was already added directly to the queue */
			chunk_uv->v.vio_head = chunk_uv->v.vio_tail =
			    chunk_uv->v.vio_base = read_chunk_uv->v.vio_head + offset;

			/* Point mr to read_chunk_uv */
			chunk_uv->u.uio_p2 = read_chunk_uv->u.uio_p2;
		}

		/* tail adjusted below */
		chunk_uv->v.vio_wrap = (char *)chunk_uv->v.vio_base + l;

		chunk_uv->v.vio_tail = (char *)chunk_uv->v.vio_head + l;

		cbc->have = TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh);

		if (xdr_rdma_wait_read_cb(rdma_xprt, cbc, 1,
					  &rl(cbc->read_chunk)->target)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: rdma_read failed rdma_xprt %p cbc %p",
				__func__, rdma_xprt, cbc);
			status = false;
		}

		pthread_mutex_lock(&cbc->recvq.ioq_uv.uvqh.qmutex);
		pthread_mutex_lock(&cbc->freeq.ioq_uv.uvqh.qmutex);

		TAILQ_CONCAT(&cbc->freeq.ioq_uv.uvqh.qh, &cbc->recvq.ioq_uv.uvqh.qh, q);
		cbc->freeq.ioq_uv.uvqh.qcount += cbc->recvq.ioq_uv.uvqh.qcount;
		cbc->recvq.ioq_uv.uvqh.qcount = 0;

		pthread_mutex_unlock(&cbc->freeq.ioq_uv.uvqh.qmutex);
		pthread_mutex_unlock(&cbc->recvq.ioq_uv.uvqh.qmutex);

		cbc->read_chunk = (char *)cbc->read_chunk
						+ sizeof(struct xdr_read_list);

		/* Move offset by length */
		offset = offset + l;
	}

	/* Wait for callback only if rdma_reads pending */
	if ((status == true) && atomic_fetch_int32_t(&cbc->read_waits)) {
		pthread_mutex_lock(&cbc->cb_done_mutex);

		/* Check for race signaled before we wait
		 * If read_waits > 1 then not signaled yet since we hold
		 * cb_done_mutex */
		if (atomic_fetch_int32_t(&cbc->read_waits)) {

			/* Wait for all rdma_read callbacks to complete */
			int rc = xdr_rdma_wait_cb_done_locked(cbc);

			if (rc) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: Failed to "
				    "get callback cbc %p cbc refs %d "
				    "read_waits %d rdma_xprt %p err %s",
				    __func__, cbc, cbc->refcnt, cbc->read_waits,
				    rdma_xprt, strerror(rc));

				/* If connection is in closing state, then we don't expect any
				 * callback, so release the refs on rdma_reads */
				if ((rc == ETIMEDOUT) &&
				    (rdma_xprt->state == RDMAXS_CLOSING)) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: cbc release "
					    "for rdma_reads cbc %p xprt %p", __func__,
					    cbc, rdma_xprt);
					for (int i = 0;
					    i < atomic_fetch_int32_t(&cbc->read_waits);
					    i++) {
						cbc_release_it(cbc);
					}
				}
				status = false;
			}
		}
		pthread_mutex_unlock(&cbc->cb_done_mutex);
	}

	pthread_mutex_lock(&cbc->recvq.ioq_uv.uvqh.qmutex);
	pthread_mutex_lock(&cbc->freeq.ioq_uv.uvqh.qmutex);

	TAILQ_CONCAT(&cbc->recvq.ioq_uv.uvqh.qh, &cbc->freeq.ioq_uv.uvqh.qh, q);
	cbc->recvq.ioq_uv.uvqh.qcount += cbc->freeq.ioq_uv.uvqh.qcount;
	cbc->freeq.ioq_uv.uvqh.qcount = 0;

	TAILQ_INSERT_HEAD(&cbc->recvq.ioq_uv.uvqh.qh, &cbc->call_uv->uvq, q);
	(cbc->recvq.ioq_uv.uvqh.qcount)++;

	/* Now recvq = req_header buf + rdma_read data bufs
	 * We use recvq xdrs for decoding */

	pthread_mutex_unlock(&cbc->freeq.ioq_uv.uvqh.qmutex);
	pthread_mutex_unlock(&cbc->recvq.ioq_uv.uvqh.qmutex);

	rpcrdma_dump_msg(IOQ_(TAILQ_FIRST(&cbc->recvq.ioq_uv.uvqh.qh)),
			 "call chunk", cmsg->rdma_xid);

        __warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d "
		"sendq %p %d freeq %p %d rdma_xprt %p ",
                __func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
                cbc->sendq.ioq_uv.uvqh.qcount, &cbc->freeq,
		cbc->freeq.ioq_uv.uvqh.qcount, rdma_xprt);

	return status;
}

/** xdr_rdma_svc_reply
 *
 * Server prepares for a reply
 *
 * potential output buffers are queued in recvq.
 *
 * @param[IN] cbc	incoming request
 *			call request is in sendq
 *
 * called by svc_rdma_reply()
 */
bool
xdr_rdma_svc_reply(struct rpc_rdma_cbc *cbc, u_int32_t xid,
    bool rdma_buf_used)
{
	RDMAXPRT *rdma_xprt;
	struct xdr_write_list *reply_array;
	struct poolq_entry *have;
	bool allocate_header = 0;
	int write_chunk = 0;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);

	if (rdma_xprt->sm_dr.xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, " %s rdma_xprt %p cbc %p "
		    "already destroyed", __func__, rdma_xprt, cbc);
		return false;
	}

        __warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d sendq %p %d rdma_xprt %p",
                __func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
                cbc->sendq.ioq_uv.uvqh.qcount, rdma_xprt);

	reply_array = (wl_t *)cbc->reply_chunk;
	 /* For write_chuks we need to allocate seperate buffer for
	 * RPC header */
	if (reply_array->present == 0) {
		reply_array = (wl_t *)cbc->write_chunk;
		if (reply_array->present)
			write_chunk = 1;
	}

	if (reply_array->present == 0) {
		/* no reply array to write, replying inline */

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() no reply array to write, "
			"replying inline and hope it works", __func__);

		have = xdr_rdma_ioq_uv_fetch(&cbc->sendq,
		    &rdma_xprt->outbufs_hdr.uvqh,
		    "sreply buffer", 1, IOQ_FLAG_NONE);

		/* buffer is limited size */
		IOQ_(have)->v.vio_head =
		IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
		IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
					+ rdma_xprt->sm_dr.send_hdr_sz;

		/* Should be only set for read and readdir */
		if (rdma_buf_used) {
			have = xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->outbufs_hdr.uvqh,
						"sreply buffer", 1, IOQ_FLAG_NONE);

			/* buffer is limited size */
			IOQ_(have)->v.vio_head =
			IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
			IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
						+ rdma_xprt->sm_dr.send_hdr_sz;
		}
		/* make room at head for RDMA header */
		xdr_ioq_reset(&cbc->sendq, 0);
	} else {
		uint32_t i;
		uint32_t l;
		uint32_t n = ntohl(reply_array->elements);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() reply array to write n %u ",
			__func__, n);

		if (!n) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() missing reply chunks",
				__func__);
			return (false);
		}

		allocate_header = 1;

		if (allocate_header) {
			have = xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->outbufs_hdr.uvqh,
					"sreply buffer", 1, IOQ_FLAG_NONE);


			/* buffer is limited size */
			IOQ_(have)->v.vio_head =
			IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
			IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
					+ rdma_xprt->sm_dr.send_hdr_sz;
			/* make room at head for RDMA header */
			xdr_ioq_reset(&cbc->sendq, 0);
		}

		/* fetch all reply chunks in advance to avoid deadlock
		 * (there may be more than one)
		 */
		for (i = 0; i < n; i++) {
			l = ntohl(reply_array->entry[i].target.length);
			if (write_chunk) {
				/* For write_list we get buffer from protocol,
				 * we need buffer to refer it */
				have = xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->outbufs_hdr.uvqh,
				    "sreply buffer", 1, IOQ_FLAG_NONE);

				/* buffer is limited size */
				IOQ_(have)->v.vio_head =
				IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
				IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
					+ rdma_xprt->sm_dr.send_hdr_sz;
			} else {
				/* For reply_list we copy from protocol buffer so allocate bigger
				 * chunk */
				assert(l <= rdma_xprt->sm_dr.sendsz);
				have = xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->outbufs_data.uvqh,
				    "sreply buffer", 1, IOQ_FLAG_NONE);

				/* buffer is limited size */
				IOQ_(have)->v.vio_head =
				IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
				IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base + l;
			}
		}
		if (!allocate_header)
			xdr_ioq_reset(&cbc->sendq, 0);
	}

        __warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d sendq %p %d rdma_xprt %p",
                __func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
                cbc->sendq.ioq_uv.uvqh.qcount, rdma_xprt);

	return (true);
}

/** xdr_rdma_clnt_flushout
 *
 * @param[IN] cbc	combined callback context
 *			call request is in sendq
 *
 * @return true is message sent, false otherwise
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_flushout(struct rpc_rdma_cbc *cbc)
{
/* FIXME: decide how many buffers we use in argument!!!!!! */
#define num_chunks (rdma_xprt->xa->credits - 1)

	RDMAXPRT *rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);
	struct rpc_msg *msg;
	struct rdma_msg *rmsg;
	struct xdr_write_list *w_array;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *hold_uv;
	struct poolq_entry *have;
	int i = 0;

	hold_uv = IOQ_(TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh));
	msg = (struct rpc_msg *)(hold_uv->v.vio_head);
	xdr_tail_update(cbc->recvq.xdrs);

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		/* good to go */
		break;
	    case REPLY:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() nothing to send on REPLY (%u)",
			__func__, ntohl(msg->rm_direction));
		return (true);
	    default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() bad rm_direction (%u)",
			__func__, ntohl(msg->rm_direction));
		return (false);
	}

	cbc->recvq.ioq_uv.uvq_fetch = xdr_rdma_ioq_uv_fetch_nothing;

	head_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->recvq, &rdma_xprt->outbufs_data.uvqh,
					"c_head buffer", 1, IOQ_FLAG_NONE));

	(void)xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->inbufs_data.uvqh,
				"call buffers", num_chunks, IOQ_FLAG_NONE);

	rmsg = m_(head_uv->v.vio_head);
	rmsg->rdma_xid = msg->rm_xid;
	rmsg->rdma_vers = htonl(RPCRDMA_VERSION);
	rmsg->rdma_credit = htonl(rdma_xprt->xa->credits);
	rmsg->rdma_type = htonl(RDMA_MSG);

	/* no read, write chunks. */
	rmsg->rdma_body.rdma_msg.rdma_reads = 0; /* htonl(0); */
	rmsg->rdma_body.rdma_msg.rdma_writes = 0; /* htonl(0); */

	/* reply chunk */
	w_array = (wl_t *)&rmsg->rdma_body.rdma_msg.rdma_reply;
	w_array->present = htonl(1);
	w_array->elements = htonl(num_chunks);

	TAILQ_FOREACH(have, &cbc->sendq.ioq_uv.uvqh.qh, q) {
		struct xdr_rdma_segment *w_seg =
			&w_array->entry[i++].target;
		uint32_t length = ioquv_length(IOQ_(have));

		w_seg->handle = htonl(rdma_xprt->mr->rkey);
		w_seg->length = htonl(length);
		xdr_encode_hyper((uint32_t*)&w_seg->offset,
				 (uintptr_t)IOQ_(have)->v.vio_head);
	}

	head_uv->v.vio_tail = head_uv->v.vio_head
				+ xdr_rdma_header_length(rmsg);

	rpcrdma_dump_msg(head_uv, "clnthead", msg->rm_xid);
	rpcrdma_dump_msg(hold_uv, "clntcall", msg->rm_xid);

	/* actual send, callback will take care of cleanup */
	xdr_rdma_async_send_cb(rdma_xprt, cbc, 2);
	return (true);
}

/** xdr_rdma_svc_flushout
 *
 * @param[IN] cbc	combined callback context
 *
 * called by svc_rdma_reply()
 *
 * This function mainly populate and send response,
 * 1> Populate rdma header
 * 2> Popualte NFS header
 * 3> Do rdma_writes
 * 4> Do rdma send for response
 *
 * rdma_writes can be done for reply/write list.
 *
 * For reply_list we send NFS header as part of rdma_write,
 * for reply list of 1 with length 1k for compound op
 * rdma_write = nfs_header + nfs_response + data_chunk
 * We reply in this sequence
 * rdma_write(nfs_header + nfs_response + data_chunk)
 * rdma_send(rdma_header)
 *
 * For write_list rdma_writes will be done only for data_chunks
 * nfs_header will be sent as rdma_send
 * We reply in this sequence
 * rdma_write(data_chunks)
 * rdma_send(nfs_header + nfs_response + rdma_header)
 *
 * For no reply_list and write_list, we reply as
 * rdma_send(nfs_header + nfs_response + rdma_header)
 *
 */
bool
xdr_rdma_svc_flushout(struct rpc_rdma_cbc *cbc, bool rdma_buf_used)
{
	RDMAXPRT *rdma_xprt;
	struct rpc_msg *msg;
	struct rdma_msg *cmsg;
	struct rdma_msg *rmsg;
	struct xdr_write_list *w_array;
	struct xdr_write_list *reply_array;
	struct xdr_ioq_uv *rdma_head_uv;
	struct xdr_ioq_uv *work_uv, *nfs_header_uv;
	struct xdr_ioq_uv *first_send_buf_uv = NULL;
	bool write_chunk = 0, add_nfs_header = 1;
	struct xdr_uio  *uio_refer = NULL;
	uint8_t *rdma_buf_addr = NULL, *non_registered_buf = NULL;
	int rdma_buf_len = 0, non_registered_buf_len = 0;
	bool ret = true;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);

        __warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d sendq %p %d rdma_xprt %p",
                __func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
                cbc->sendq.ioq_uv.uvqh.qcount, rdma_xprt);

	reply_array = (wl_t *)cbc->reply_chunk;


	if (reply_array->present == 0) {
		reply_array = (wl_t *)cbc->write_chunk;
		if (reply_array->present)
			write_chunk = 1;
	}

	/* Remove NFS rpc header to send it after rdma_writes,
	 * We allocated seperate header for nfs */
	nfs_header_uv = IOQ_(TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh));

	work_uv = nfs_header_uv;
	msg = (struct rpc_msg *)(work_uv->v.vio_head);
	/* work_uv->v.vio_tail has been set by xdr_tail_update() */

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() nothing to send on CALL (%u)",
			__func__, ntohl(msg->rm_direction));
		return (true);
	    case REPLY:
		/* good to go */
		break;
	    default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() bad rm_direction (%u)",
			__func__, ntohl(msg->rm_direction));
		return (false);
	}
	cmsg = m_(cbc->call_head);

	if (cmsg->rdma_xid != msg->rm_xid) {
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() xid (%u) not equal RPC (%u)",
			__func__, ntohl(cmsg->rdma_xid), ntohl(msg->rm_xid));
		return (false);
	}

	/* usurp the sendq for the head, move to recvq later */
	/* Allocate seperate buf to send RPCoRDMA header */
	rdma_head_uv = IOQ_(xdr_rdma_ioq_uv_fetch(&cbc->sendq, &rdma_xprt->outbufs_hdr.uvqh,
					"sreply head", 1, IOQ_FLAG_NONE));

	/* entry was already added directly to the queue */
	rdma_head_uv->v.vio_head = rdma_head_uv->v.vio_tail =
	    rdma_head_uv->v.vio_base;
	/* tail adjusted below */
	rdma_head_uv->v.vio_wrap = (char *)rdma_head_uv->v.vio_base +
	    rdma_xprt->sm_dr.send_hdr_sz;

	/* build the header that goes with the data */
	rmsg = m_(rdma_head_uv->v.vio_head);
	rmsg->rdma_xid = cmsg->rdma_xid;
	rmsg->rdma_vers = cmsg->rdma_vers;
	rmsg->rdma_credit = htonl(rdma_xprt->xa->credits);

	/* no read, write chunks. */
	rmsg->rdma_body.rdma_msg.rdma_reads = 0; /* htonl(0); */
	rmsg->rdma_body.rdma_msg.rdma_writes = 0; /* htonl(0); */
	rmsg->rdma_body.rdma_msg.rdma_reply = 0;

	rpcrdma_dump_msg(nfs_header_uv, "sreply nfs head", msg->rm_xid);

	pthread_mutex_lock(&cbc->sendq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&cbc->sendq.ioq_uv.uvqh.qh, &rdma_head_uv->uvq, q);
	(cbc->sendq.ioq_uv.uvqh.qcount)--;

	TAILQ_REMOVE(&cbc->sendq.ioq_uv.uvqh.qh, &nfs_header_uv->uvq, q);
	(cbc->sendq.ioq_uv.uvqh.qcount)--;
	pthread_mutex_unlock(&cbc->sendq.ioq_uv.uvqh.qmutex);

	if (reply_array->present == 0) {
		rmsg->rdma_type = htonl(RDMA_MSG);

		/* no reply chunk either */
		rmsg->rdma_body.rdma_msg.rdma_reply = 0; /* htonl(0); */

		rdma_head_uv->v.vio_tail = rdma_head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);

		struct poolq_entry *have = TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh);
		if (have) {
			/* We will do rdma_send, so if there is rdma_buf from
			 * protocol, just copy it, for read/readdir we always
			 * get rdma_buf even if there are no reply_array/write_array,
			 * if there is no rdma_buf the it could be compound op with reply_buf,
			 * So no harm in copying for metadata ops */

			first_send_buf_uv  = IOQ_(have);
			uint8_t *ptr = first_send_buf_uv->v.vio_head;
			uint32_t len = ioquv_length(first_send_buf_uv);

			assert(rdma_buf_used);
			assert(cbc->sendq.ioq_uv.uvqh.qcount == 1);
			assert(len <= rdma_xprt->sm_dr.send_hdr_sz);

			/* If its rdma_buf from protocol we should have
			 * UIO_FLAG_REFER set. */
			if (first_send_buf_uv->u.uio_flags & UIO_FLAG_REFER)
				uio_refer = first_send_buf_uv->u.uio_refer;

			first_send_buf_uv->v = first_send_buf_uv->rdma_v;
			first_send_buf_uv->u = first_send_buf_uv->rdma_u;

			memcpy(first_send_buf_uv->v.vio_head, ptr,
			    len);
			first_send_buf_uv->v.vio_tail = first_send_buf_uv->v.vio_head + len;
		}

		rpcrdma_dump_msg(rdma_head_uv, "sreply head", msg->rm_xid);
		rpcrdma_dump_msg(work_uv, "sreply body", msg->rm_xid);
	} else {
		uint32_t i = 0;
		uint32_t n = ntohl(reply_array->elements);

		rmsg->rdma_type = htonl(RDMA_NOMSG);

		struct poolq_entry *have = TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh);
		first_send_buf_uv  = IOQ_(have);

		/* reply chunk */
		w_array = (wl_t *)&rmsg->rdma_body.rdma_msg.rdma_reply;
		if (write_chunk) {
			w_array = (wl_t *)&rmsg->rdma_body.rdma_msg.rdma_writes;
			rmsg->rdma_type = htonl(RDMA_MSG);
		}

		/* In case of read/readdir xdr encode xdr_putbufs will
		 * change the uv vio buffers to point to buffser set by
		 * protocols and UIO_FLAG_REFER will be set.
		 * first_buf = nfs_header buf + rdma_write bufs */
		if (rdma_buf_used) {
			rdma_buf_addr = first_send_buf_uv->v.vio_head;
			rdma_buf_len = ioquv_length(first_send_buf_uv);
			if (first_send_buf_uv->u.uio_flags & UIO_FLAG_REFER)
				uio_refer = first_send_buf_uv->u.uio_refer;

			/* If no write_list then we do memcpy from rdma_buf passed
			 * passed from protocol*/
			if (!write_chunk) {
				non_registered_buf = mem_zalloc(rdma_buf_len +
				    ioquv_length(nfs_header_uv));
				cbc->non_registered_buf = non_registered_buf;
				memcpy(non_registered_buf, nfs_header_uv->v.vio_head,
				    ioquv_length(nfs_header_uv));
				memcpy(non_registered_buf + ioquv_length(nfs_header_uv),
				    rdma_buf_addr, rdma_buf_len);
				rdma_buf_addr = non_registered_buf;
				rdma_buf_len = rdma_buf_len + ioquv_length(nfs_header_uv);
				non_registered_buf_len = rdma_buf_len;
				cbc->non_registered_buf_len = non_registered_buf_len;
				add_nfs_header = 0;
				xdr_rdma_ioq_uv_release(nfs_header_uv);
			}
		} else {
			memcpy(first_send_buf_uv->v.vio_head, nfs_header_uv->v.vio_head,
			    ioquv_length(nfs_header_uv));
			memcpy(first_send_buf_uv->v.vio_head + ioquv_length(nfs_header_uv),
			    rdma_buf_addr, rdma_buf_len);
			first_send_buf_uv->v.vio_tail = first_send_buf_uv->v.vio_head +
			    ioquv_length(nfs_header_uv) + rdma_buf_len;
			add_nfs_header = 0;
			xdr_rdma_ioq_uv_release(nfs_header_uv);
		}

		__warnx(TIRPC_DEBUG_FLAG_XDR, "first_send_buf_uv %d write_chunk %d",
			ioquv_length(first_send_buf_uv), write_chunk);

		w_array->present = htonl(1);

		rdma_head_uv->v.vio_tail = rdma_head_uv->v.vio_head
					+ 64;

		cbc->have = TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh);
		while (ret && (i < n)) {
			struct xdr_rdma_segment *c_seg =
				&reply_array->entry[i].target;
			struct xdr_rdma_segment *w_seg =
				&w_array->entry[i++].target;
			struct xdr_ioq_uv *send_uv = NULL;
			uint32_t length = ntohl(c_seg->length);
			uint32_t nfs_header_len = ioquv_length(nfs_header_uv);

			assert(length <= rdma_xprt->sm_dr.sendsz);

			*w_seg = *c_seg;

			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() requested chunk length %u offset %llx handle %x length %x"
				" nfs_header_len %u rdma_buf_addr %p rdma_buf_len %u",
				__func__, length, w_seg->offset, w_seg->handle, w_seg->length,
				nfs_header_len, rdma_buf_addr, rdma_buf_len);

			send_uv  = IOQ_(cbc->have);
			assert(send_uv->rdma_uv);

			if (rdma_buf_used) {
				uint32_t write_len = (rdma_buf_len > length) ? length : rdma_buf_len;

				if (non_registered_buf) {
					/* Copy allocated buffer to registered buffer */
					send_uv->v = send_uv->rdma_v;
					send_uv->u = send_uv->rdma_u;
					memcpy(send_uv->v.vio_head, rdma_buf_addr,
					    write_len);
				} else {
					/* If its protocol rdma buffer, we need to set uv vio addr to
					 * point to protocol rdma buf, this is zero copy */
					send_uv->v.vio_head = rdma_buf_addr;
					/* mr could be different if its extra added buffer
					 * protocol buffer will always be from preallocate mr */
					send_uv->u.uio_p2 = cbc->data_chunk_uv->u.uio_p2;
				}
				send_uv->v.vio_tail = send_uv->v.vio_head + write_len;
				rdma_buf_addr = rdma_buf_addr + write_len;
				rdma_buf_len = rdma_buf_len - write_len;
			}

			if (xdr_rdma_async_write_cb(rdma_xprt, cbc, 1, w_seg)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s rdma_write failed rdma_xprt %p "
				    "cbc %p", __func__, rdma_xprt, cbc);
				ret = false;
			}

			rpcrdma_dump_msg(IOQ_(have), "sreply rdma write",
					 msg->rm_xid);

			/* Reset uv vio back to rdma buf, so that we don't release protocol
			 * rdma_buf */
			if (rdma_buf_addr) {
				send_uv->v = send_uv->rdma_v;
				send_uv->u = send_uv->rdma_u;
			}
			cbc->have = TAILQ_NEXT(cbc->have, q);
		}

		w_array->elements = htonl(i);

		rdma_head_uv->v.vio_tail = rdma_head_uv->v.vio_head
						+ 64;
		if (write_chunk) {
			wl_t * reply_array1 = (wl_t *)xdr_rdma_get_reply_array(rdma_head_uv->v.vio_head);
			uint32_t * iptr = (uint32_t * )reply_array1;
			iptr--;
			*iptr = 0;
			reply_array1->present = 0;
		}

		assert(cbc->freeq.ioq_uv.uvqh.qcount == 0);

		pthread_mutex_lock(&cbc->freeq.ioq_uv.uvqh.qmutex);
		pthread_mutex_lock(&cbc->sendq.ioq_uv.uvqh.qmutex);

		TAILQ_CONCAT(&cbc->freeq.ioq_uv.uvqh.qh, &cbc->sendq.ioq_uv.uvqh.qh, q);
		cbc->freeq.ioq_uv.uvqh.qcount += cbc->sendq.ioq_uv.uvqh.qcount;
		cbc->sendq.ioq_uv.uvqh.qcount = 0;

		pthread_mutex_unlock(&cbc->sendq.ioq_uv.uvqh.qmutex);
		pthread_mutex_unlock(&cbc->freeq.ioq_uv.uvqh.qmutex);

		rdma_head_uv->v.vio_tail = rdma_head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);

	}

	rpcrdma_dump_msg(rdma_head_uv, "sreply rdma head", msg->rm_xid);
	rpcrdma_dump_msg(nfs_header_uv, "sreply nfs head", msg->rm_xid);

	pthread_mutex_lock(&cbc->sendq.ioq_uv.uvqh.qmutex);

	if (add_nfs_header) {
		TAILQ_INSERT_HEAD(&cbc->sendq.ioq_uv.uvqh.qh, &nfs_header_uv->uvq, q);
		(cbc->sendq.ioq_uv.uvqh.qcount)++;
	}

	TAILQ_INSERT_HEAD(&cbc->sendq.ioq_uv.uvqh.qh, &rdma_head_uv->uvq, q);
	(cbc->sendq.ioq_uv.uvqh.qcount)++;

	pthread_mutex_unlock(&cbc->sendq.ioq_uv.uvqh.qmutex);

	/* recvq = request_header buf + rdma_read bufs */
	/* sendq = response_header_buf + rdma_write_bufs */
	/* dataq = protocol_buf and rdma_read buf */

	assert((cbc->dataq.ioq_uv.uvqh.qcount > 0) &&
	    (cbc->dataq.ioq_uv.uvqh.qcount <= 2));

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s() sendq %d recvq %d", __func__,
		cbc->sendq.ioq_uv.uvqh.qcount, cbc->recvq.ioq_uv.uvqh.qcount);

	cbc->have = TAILQ_FIRST(&cbc->sendq.ioq_uv.uvqh.qh);

	if (xdr_rdma_async_send_cb(rdma_xprt, cbc,
				  cbc->sendq.ioq_uv.uvqh.qcount)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s rdma_send failed rdma_xprt %p "
		    "cbc %p", __func__, rdma_xprt, cbc);
		ret = false;
	}

	/* Release uio for read/readdir */
	if (uio_refer) {
		uio_refer->uio_release(uio_refer, UIO_FLAG_NONE);
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc %p recvq %p %d sendq %p %d rdma_xprt %p",
		__func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount, &cbc->sendq,
		cbc->sendq.ioq_uv.uvqh.qcount, rdma_xprt);

	return ret;
}
