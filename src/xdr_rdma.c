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

#define CALLQ_SIZE (2)
#define RFC5666_BUFFER_SIZE (1024)
#define RPCRDMA_VERSION (1)

#define x_xprt(xdrs) ((RDMAXPRT *)((xdrs)->x_lib[1]))

//#define rpcrdma_dump_msg(data, comment, xid)

#ifndef rpcrdma_dump_msg
#define DUMP_BYTES_PER_GROUP (4)
#define DUMP_GROUPS_PER_LINE (4)
#define DUMP_BYTES_PER_LINE (DUMP_BYTES_PER_GROUP * DUMP_GROUPS_PER_LINE)

static void
rpcrdma_dump_msg(struct xdr_ioq_uv *data, char *comment, uint32_t xid)
{
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

/***********************************/
/****** Utilities for buffers ******/
/***********************************/

static void
xdr_rdma_chunk_in(struct poolq_entry *have, u_int k, u_int m, u_int sized)
{
	/* final buffer limited to truncated length */
	IOQ_(have)->v.vio_head = IOQ_(have)->v.vio_base;
	IOQ_(have)->v.vio_tail = (char *)IOQ_(have)->v.vio_base + m;
	IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base + sized;

	while (0 < --k && NULL != (have = TAILQ_PREV(have, poolq_head_s, q))) {
		/* restore defaults after previous usage */
		IOQ_(have)->v.vio_head = IOQ_(have)->v.vio_base;
		IOQ_(have)->v.vio_tail =
		IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base + sized;
	}
}

static void
xdr_rdma_chunk_out(struct poolq_entry *have, u_int k, u_int m, u_int sized)
{
	/* final buffer limited to truncated length */
	IOQ_(have)->v.vio_head =
	IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
	IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base + m;

	while (0 < --k && NULL != (have = TAILQ_PREV(have, poolq_head_s, q))) {
		/* restore defaults after previous usage */
		IOQ_(have)->v.vio_head =
		IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
		IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base + sized;
	}
}

static uint32_t
xdr_rdma_chunk_fetch(struct xdr_ioq *xioq, struct poolq_head *ioqh,
		     char *comment, u_int length, u_int sized, u_int max_sge,
		     void (*setup)(struct poolq_entry *, u_int, u_int, u_int))
{
	struct poolq_entry *have;
	uint32_t k = length / sized;
	uint32_t m = length % sized;

	if (m) {
		/* need fractional buffer */
		k++;
	} else {
		/* have full-sized buffer */
		m = sized;
	}

	/* ensure never asking for more buffers than allowed */
	if (k > max_sge) {
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() requested chunk %" PRIu32
			" is too long (%" PRIu32 ">%" PRIu32 ")",
			__func__, length, k, max_sge);
		k = max_sge;
		m = sized;
	}

	/* ensure we can get all of our buffers without deadlock
	 * (wait for them all to be appended)
	 */
	have = xdr_ioq_uv_fetch(xioq, ioqh, comment, k, IOQ_FLAG_NONE);
	(*setup)(have, k, m, sized);
	return k;
}

/***********************/
/****** Callbacks ******/
/***********************/

/* note parameter order matching svc.h svc_req callbacks */

static int
xdr_rdma_respond_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_lock(&xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&xprt->sm_dr.ioq.ioq_uv.uvqh.qh, &cbc->workq.ioq_s, q);
	(xprt->sm_dr.ioq.ioq_uv.uvqh.qcount)--;
	mutex_unlock(&xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);

	xdr_ioq_destroy(&cbc->workq, sizeof(*cbc));
	return (0);
}

static int
xdr_rdma_destroy_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_lock(&xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&xprt->sm_dr.ioq.ioq_uv.uvqh.qh, &cbc->workq.ioq_s, q);
	(xprt->sm_dr.ioq.ioq_uv.uvqh.qcount)--;
	mutex_unlock(&xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);

	xdr_ioq_destroy(&cbc->workq, sizeof(*cbc));
	return (0);
}

/**
 * xdr_rdma_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static int
xdr_rdma_wait_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	mutex_t *lock = cbc->callback_arg;

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_unlock(lock);
	return (0);
}

/**
 * xdr_rdma_warn_callback: send/recv callback that just unlocks a mutex.
 *
 */
static int
xdr_rdma_warn_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	mutex_t *lock = cbc->callback_arg;

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_unlock(lock);
	return (0);
}

/**
 * xdr_rdma_wrap_callback: send/recv callback converts enum to int.
 *
 */
static int
xdr_rdma_wrap_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	XDR *xdrs = cbc->holdq.xdrs;

	return (int)svc_request(&xprt->sm_dr.xprt, xdrs);
}

/***********************************/
/***** Utilities from Mooshika *****/
/***********************************/

/**
 * xdr_rdma_post_recv_n: Post receive chunk(s).
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] xprt
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
xdr_rdma_post_recv_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	struct poolq_entry *have = TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh);
	int i = 0;
	int ret;

	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (xprt->state) {
	case RDMAXS_CONNECTED:
	case RDMAXS_ROUTE_RESOLVED:
	case RDMAXS_CONNECT_REQUEST:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting recv",
			__func__, xprt, xprt->state, cbc);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] != "
			"connect request, connected, or resolved",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	while (have && i < sge) {
		struct ibv_mr *mr = IOQ_(have)->u.uio_p2;

		if (!mr) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Missing mr: Not requesting.",
				__func__);
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
	}

	cbc->wr.rwr.next = NULL;
	cbc->wr.rwr.wr_id = (uintptr_t)cbc;
	cbc->wr.rwr.sg_list = cbc->sg_list;
	cbc->wr.rwr.num_sge = i;

	if (xprt->srq)
		ret = ibv_post_srq_recv(xprt->srq, &cbc->wr.rwr,
					&xprt->bad_recv_wr);
	else
		ret = ibv_post_recv(xprt->qp, &cbc->wr.rwr,
					&xprt->bad_recv_wr);

	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_recv failed: %s (%d)",
			__func__, xprt, xprt->state, cbc, strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * xdr_rdma_post_recv_cb: Post receive chunk(s) with standard callbacks.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to register
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_recv_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	cbc->positive_cb = xdr_rdma_wrap_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = NULL;
	return xdr_rdma_post_recv_n(xprt, cbc, sge);
}

/**
 * Post a work chunk.
 *
 * @param[IN] xprt
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
xdr_rdma_post_send_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge,
		     struct xdr_rdma_segment *rs, enum ibv_wr_opcode opcode)
{
	struct poolq_entry *have = TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh);
	uint32_t totalsize = 0;
	int i = 0;
	int ret;

	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (xprt->state) {
	case RDMAXS_CONNECTED:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting a send with op %d",
			__func__, xprt, xprt->state, cbc, opcode);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] != "
			"connected",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	// opcode-specific checks:
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
	case IBV_WR_SEND_WITH_IMM:
		break;
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
				"%s() Empty buffer: Not sending.",
				__func__);
			break;
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
	}

	cbc->wr.wwr.next = NULL;
	cbc->wr.wwr.wr_id = (uint64_t)cbc;
	cbc->wr.wwr.opcode = opcode;
//FIXME	cbc->wr.wwr.imm_data = htonl(data->imm_data);
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

	ret = ibv_post_send(xprt->qp, &cbc->wr.wwr, &xprt->bad_send_wr);
	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_send failed: %s (%d)",
			__func__, xprt, xprt->state, cbc, strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * Post a work chunk with standard callbacks.
 *
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to send
 *
 * @return 0 on success, the value of errno on error
 */
static inline int
xdr_rdma_post_send_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, sge, NULL, IBV_WR_SEND);
}

#ifdef UNUSED
/**
 * Post a receive chunk and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param[IN] xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_wait_recv_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_recv_n(xprt, cbc);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

/**
 * Post a send chunk and waits for that one to be completely sent
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] sge	scatter/gather elements to send
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_wait_send_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge)
{
	mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, sge, NULL, IBV_WR_SEND);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

static inline int
xdr_rdma_post_read_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge,
		      struct xdr_rdma_segment *rs)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, sge, rs, IBV_WR_RDMA_READ);
}

static inline int
xdr_rdma_post_write_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge,
		       struct xdr_rdma_segment *rs)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, sge, rs, IBV_WR_RDMA_WRITE);
}
#endif /* UNUSED */

static int
xdr_rdma_wait_read_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge,
		     struct xdr_rdma_segment *rs)
{
	mutex_t lock = MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, sge, rs, IBV_WR_RDMA_READ);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

static int
xdr_rdma_wait_write_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc, int sge,
		      struct xdr_rdma_segment *rs)
{
	mutex_t lock = MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, sge, rs, IBV_WR_RDMA_WRITE);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

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
	while (rl(*pptr)->present) {
		*pptr += sizeof(struct xdr_read_list)
			 / sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void
xdr_rdma_skip_write_list(uint32_t **pptr)
{
	if (wl(*pptr)->present) {
		*pptr += (sizeof(struct xdr_write_list)
			  + sizeof(struct xdr_write_chunk)
			    * ntohl(wl(*pptr)->elements))
			 / sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void
xdr_rdma_skip_reply_array(uint32_t **pptr)
{
	if (wl(*pptr)->present) {
		*pptr += (sizeof(struct xdr_write_list)
			  + sizeof(struct xdr_write_chunk)
			    * ntohl(wl(*pptr)->elements))
			 / sizeof(**pptr);
	} else {
		(*pptr)++;
	}
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

#ifdef UNUSED
void
xdr_rdma_encode_reply_array(wl_t *ary, int chunks)
{
	ary->present = xdr_one;
	ary->elements = htonl(chunks);
}

void
xdr_rdma_encode_array_chunk(wl_t *ary, int chunk_no, u32 handle,
			    u64 offset, u32 write_len)
{
	struct xdr_rdma_segment *seg = &ary->entry[chunk_no].target;
	seg->handle = htonl(handle);
	seg->length = htonl(write_len);
	xdr_encode_hyper((u32 *) &seg->offset, offset);
}

void
xdr_rdma_encode_reply_header(struct svcxprt_rdma *xprt,
				  struct rdma_msg *rdma_argp,
				  struct rdma_msg *rdma_resp,
				  enum rdma_proc rdma_type)
{
	rdma_resp->rdma_xid = htonl(rdma_argp->rdma_xid);
	rdma_resp->rdma_vers = htonl(rdma_argp->rdma_vers);
	rdma_resp->rdma_credit = htonl(xprt->sc_max_requests);
	rdma_resp->rdma_type = htonl(rdma_type);

	/* Encode <nul> chunks lists */
	rdma_resp->rdma_body.rm_chunks[0] = xdr_zero;
	rdma_resp->rdma_body.rm_chunks[1] = xdr_zero;
	rdma_resp->rdma_body.rm_chunks[2] = xdr_zero;
}
#endif /* UNUSED */

/* post recv buffers.
 * keep at least 2 spare waiting for calls,
 * the remainder can be used for incoming rdma buffers.
 */
void
xdr_rdma_callq(RDMAXPRT *xd)
{
	struct poolq_entry *have =
		xdr_ioq_uv_fetch(&xd->sm_dr.ioq, &xd->cbqh,
				 "callq context", 1, IOQ_FLAG_NONE);
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)(_IOQ(have));

	have = xdr_ioq_uv_fetch(&cbc->workq, &xd->inbufs.uvqh,
				"callq buffer", 1, IOQ_FLAG_NONE);

	/* input positions */
	IOQ_(have)->v.vio_head = IOQ_(have)->v.vio_base;
	IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_wrap;
	IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
			       + xd->sm_dr.recvsz;

	cbc->workq.xdrs[0].x_lib[1] =
	cbc->holdq.xdrs[0].x_lib[1] = xd;

	xdr_rdma_post_recv_cb(xd, cbc, 1);
}

/****************************/
/****** Main functions ******/
/****************************/

void
xdr_rdma_destroy(RDMAXPRT *xd)
{
	if (xd->mr) {
		ibv_dereg_mr(xd->mr);
		xd->mr = NULL;
	}

	xdr_ioq_destroy_pool(&xd->sm_dr.ioq.ioq_uv.uvqh);

	/* must be after queues, xdr_ioq_destroy() moves them here */
	xdr_ioq_release(&xd->inbufs.uvqh);
	poolq_head_destroy(&xd->inbufs.uvqh);
	xdr_ioq_release(&xd->outbufs.uvqh);
	poolq_head_destroy(&xd->outbufs.uvqh);

	/* must be after pools */
	if (xd->buffer_aligned) {
		mem_free(xd->buffer_aligned, xd->buffer_total);
		xd->buffer_aligned = NULL;
	}

	xd->sm_dr.ioq.xdrs[0].x_lib[0] = NULL;
	xd->sm_dr.ioq.xdrs[0].x_lib[1] = NULL;
}

/*
 * initializes a stream descriptor for a memory buffer.
 *
 * credits is the number of buffers used
 */
int
xdr_rdma_create(RDMAXPRT *xd)
{
	uint8_t *b;

	if (!xd->pd || !xd->pd->pd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] missing Protection Domain",
			__func__, xd, xd->state);
		return ENODEV;
	}

	/* pre-allocated buffer_total:
	 * the number of credits is irrelevant here.
	 * instead, allocate buffers to match the read/write contexts.
	 * more than one buffer can be chained to one ioq_uv head,
	 * but never need more ioq_uv heads than buffers.
	 */
	xd->buffer_total = xd->sm_dr.recvsz * xd->xa->rq_depth
			 + xd->sm_dr.sendsz * xd->xa->sq_depth;

	xd->buffer_aligned = mem_aligned(xd->sm_dr.pagesz, xd->buffer_total);

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() buffer_aligned at %p",
		__func__, xd->buffer_aligned);

	/* register it in two chunks for read and write??? */
	xd->mr = ibv_reg_mr(xd->pd->pd, xd->buffer_aligned, xd->buffer_total,
			    IBV_ACCESS_LOCAL_WRITE |
			    IBV_ACCESS_REMOTE_WRITE |
			    IBV_ACCESS_REMOTE_READ);

	poolq_head_setup(&xd->inbufs.uvqh);
	xd->inbufs.min_bsize = xd->sm_dr.pagesz;
	xd->inbufs.max_bsize = xd->sm_dr.recvsz;

	poolq_head_setup(&xd->outbufs.uvqh);
	xd->outbufs.min_bsize = xd->sm_dr.pagesz;
	xd->outbufs.max_bsize = xd->sm_dr.sendsz;

	/* Each pre-allocated buffer has a corresponding xdr_ioq_uv,
	 * stored on the pool queues.
	 */
	b = xd->buffer_aligned;

	for (xd->inbufs.uvqh.qcount = 0;
	     xd->inbufs.uvqh.qcount < xd->xa->rq_depth;
	     xd->inbufs.uvqh.qcount++) {
		struct xdr_ioq_uv *data = xdr_ioq_uv_create(0, UIO_FLAG_BUFQ);

		data->v.vio_base =
		data->v.vio_head =
		data->v.vio_tail = b;
		data->v.vio_wrap = (char *)b + xd->sm_dr.recvsz;
		data->u.uio_p1 = &xd->inbufs.uvqh;
		data->u.uio_p2 = xd->mr;
		TAILQ_INSERT_TAIL(&xd->inbufs.uvqh.qh, &data->uvq, q);

		b += xd->sm_dr.recvsz;
	}

	for (xd->outbufs.uvqh.qcount = 0;
	     xd->outbufs.uvqh.qcount < xd->xa->sq_depth;
	     xd->outbufs.uvqh.qcount++) {
		struct xdr_ioq_uv *data = xdr_ioq_uv_create(0, UIO_FLAG_BUFQ);

		data->v.vio_base =
		data->v.vio_head =
		data->v.vio_tail = b;
		data->v.vio_wrap = (char *)b + xd->sm_dr.sendsz;
		data->u.uio_p1 = &xd->outbufs.uvqh;
		data->u.uio_p2 = xd->mr;
		TAILQ_INSERT_TAIL(&xd->outbufs.uvqh.qh, &data->uvq, q);

		b += xd->sm_dr.sendsz;
	}

	while (xd->sm_dr.ioq.ioq_uv.uvqh.qcount < CALLQ_SIZE) {
		xdr_rdma_callq(xd);
	}
	return 0;
}

/** xdr_rdma_clnt_reply
 *
 * Client prepares for a reply
 *
 * potential output buffers are queued in workq.
 *
 * @param[IN] xdrs	cm_data
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_reply(XDR *xdrs, u_int32_t xid)
{
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)xdrs;
	RDMAXPRT *xprt;
	struct xdr_write_list *reply_array;
	struct xdr_ioq_uv *work_uv;
	struct poolq_entry *have;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(xdrs);

	work_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
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
			have = xdr_ioq_uv_fetch(&cbc->holdq, &xprt->inbufs.uvqh,
				"creply body", 1, IOQ_FLAG_NONE);
			rpcrdma_dump_msg(IOQ_(have), "creply body", ntohl(xid));

			/* length < size if the protocol works out...
			 * FIXME: check anyway?
			 */
/*			l = ntohl(reply_array->entry[i].target.length); */
		}
	}

	xdr_ioq_reset(&cbc->holdq, 0);
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
 *			call request is in workq
 *
 * called by svc_rdma_recv()
 */
bool
xdr_rdma_svc_recv(struct rpc_rdma_cbc *cbc, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct rdma_msg *cmsg;
	uint32_t k;
	uint32_t l;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* free old buffers (should do nothing) */
	xdr_ioq_release(&cbc->holdq.ioq_uv.uvqh);
	xdr_rdma_callq(xprt);

	cbc->call_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
	(cbc->call_uv->u.uio_references)++;
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
		xdr_rdma_post_send_cb(xprt, cbc, 1);
		xdr_ioq_uv_release(cbc->call_uv);
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
		xdr_rdma_post_send_cb(xprt, cbc, 1);
		xdr_ioq_uv_release(cbc->call_uv);
		return (false);
	}

	/* locate NFS/RDMA (RFC-5666) chunk positions */
	cbc->read_chunk = xdr_rdma_get_read_list(cmsg);
	cbc->write_chunk = (wl_t *)cbc->read_chunk;
	xdr_rdma_skip_read_list((uint32_t **)&cbc->write_chunk);
	cbc->reply_chunk = cbc->write_chunk;
	xdr_rdma_skip_write_list((uint32_t **)&cbc->reply_chunk);
	cbc->call_data = cbc->reply_chunk;
	xdr_rdma_skip_reply_array((uint32_t **)&cbc->call_data);

	/* swap calling message from workq to holdq */
	TAILQ_CONCAT(&cbc->holdq.ioq_uv.uvqh.qh, &cbc->workq.ioq_uv.uvqh.qh, q);
	cbc->holdq.ioq_uv.uvqh.qcount = cbc->workq.ioq_uv.uvqh.qcount;
	cbc->workq.ioq_uv.uvqh.qcount = 0;

	/* skip past the header for the calling buffer */
	xdr_ioq_reset(&cbc->holdq, ((uintptr_t)cbc->call_data
				  - (uintptr_t)cmsg));

	while (rl(cbc->read_chunk)->present != 0
	    && rl(cbc->read_chunk)->position == 0) {
		l = ntohl(rl(cbc->read_chunk)->target.length);
		k = xdr_rdma_chunk_fetch(&cbc->workq, &xprt->inbufs.uvqh,
					 "call chunk", l,
					 xprt->sm_dr.recvsz,
					 xprt->xa->max_recv_sge,
					 xdr_rdma_chunk_in);

		xdr_rdma_wait_read_cb(xprt, cbc, k, &rl(cbc->read_chunk)->target);
		rpcrdma_dump_msg(IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh)),
				 "call chunk", cmsg->rdma_xid);

		/* concatenate any additional buffers after the calling message,
		 * faking there is more call data in the calling buffer.
		 */
		TAILQ_CONCAT(&cbc->holdq.ioq_uv.uvqh.qh,
			     &cbc->workq.ioq_uv.uvqh.qh, q);
		cbc->holdq.ioq_uv.uvqh.qcount += cbc->workq.ioq_uv.uvqh.qcount;
		cbc->workq.ioq_uv.uvqh.qcount = 0;
		cbc->read_chunk = (char *)cbc->read_chunk
						+ sizeof(struct xdr_read_list);
	}

	return (true);
}

/** xdr_rdma_svc_reply
 *
 * Server prepares for a reply
 *
 * potential output buffers are queued in workq.
 *
 * @param[IN] cbc	incoming request
 *			call request is in holdq
 *
 * called by svc_rdma_reply()
 */
bool
xdr_rdma_svc_reply(struct rpc_rdma_cbc *cbc, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct xdr_write_list *reply_array;
	struct poolq_entry *have;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* free call buffers (head will be retained) */
	xdr_ioq_release(&cbc->holdq.ioq_uv.uvqh);

	reply_array = (wl_t *)cbc->reply_chunk;
	if (reply_array->present == 0) {
		/* no reply array to write, replying inline and hope it works
		 * (OK on RPC/RDMA Read)
		 */
		have = xdr_ioq_uv_fetch(&cbc->holdq, &xprt->outbufs.uvqh,
					"sreply buffer", 1, IOQ_FLAG_NONE);

		/* buffer is limited size */
		IOQ_(have)->v.vio_head =
		IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
		IOQ_(have)->v.vio_wrap = (char *)IOQ_(have)->v.vio_base
					+ RFC5666_BUFFER_SIZE;

		/* make room at head for RDMA header */
		xdr_ioq_reset(&cbc->holdq, (uintptr_t)cbc->call_data
				  - (uintptr_t)cbc->write_chunk
				  + offsetof(struct rdma_msg, rdma_body));
	} else {
		uint32_t i;
		uint32_t l;
		uint32_t n = ntohl(reply_array->elements);

		if (!n) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() missing reply chunks",
				__func__);
			return (false);
		}

		/* fetch all reply chunks in advance to avoid deadlock
		 * (there may be more than one)
		 */
		for (i = 0; i < n; i++) {
			l = ntohl(reply_array->entry[i].target.length);
			xdr_rdma_chunk_fetch(&cbc->holdq, &xprt->outbufs.uvqh,
					     "sreply chunk", l,
					     xprt->sm_dr.sendsz,
					     xprt->xa->max_send_sge,
					     xdr_rdma_chunk_out);
		}

		xdr_ioq_reset(&cbc->holdq, 0);
	}

	return (true);
}

/** xdr_rdma_clnt_flushout
 *
 * @param[IN] cbc	combined callback context
 *			call request is in holdq
 *
 * @return true is message sent, false otherwise
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_flushout(struct rpc_rdma_cbc *cbc)
{
/* FIXME: decide how many buffers we use in argument!!!!!! */
#define num_chunks (xd->xa->credits - 1)

	RDMAXPRT *xd = x_xprt(cbc->workq.xdrs);
	struct rpc_msg *msg;
	struct rdma_msg *rmsg;
	struct xdr_write_list *w_array;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *hold_uv;
	struct poolq_entry *have;
	int i = 0;

	hold_uv = IOQ_(TAILQ_FIRST(&cbc->holdq.ioq_uv.uvqh.qh));
	msg = (struct rpc_msg *)(hold_uv->v.vio_head);
	xdr_tail_update(cbc->workq.xdrs);

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

	cbc->workq.ioq_uv.uvq_fetch = xdr_ioq_uv_fetch_nothing;

	head_uv = IOQ_(xdr_ioq_uv_fetch(&cbc->workq, &xd->outbufs.uvqh,
					"c_head buffer", 1, IOQ_FLAG_NONE));

	(void)xdr_ioq_uv_fetch(&cbc->holdq, &xd->inbufs.uvqh,
				"call buffers", num_chunks, IOQ_FLAG_NONE);

	rmsg = m_(head_uv->v.vio_head);
	rmsg->rdma_xid = msg->rm_xid;
	rmsg->rdma_vers = htonl(RPCRDMA_VERSION);
	rmsg->rdma_credit = htonl(xd->xa->credits);
	rmsg->rdma_type = htonl(RDMA_MSG);

	/* no read, write chunks. */
	rmsg->rdma_body.rdma_msg.rdma_reads = 0; /* htonl(0); */
	rmsg->rdma_body.rdma_msg.rdma_writes = 0; /* htonl(0); */

	/* reply chunk */
	w_array = (wl_t *)&rmsg->rdma_body.rdma_msg.rdma_reply;
	w_array->present = htonl(1);
	w_array->elements = htonl(num_chunks);

	TAILQ_FOREACH(have, &cbc->holdq.ioq_uv.uvqh.qh, q) {
		struct xdr_rdma_segment *w_seg =
			&w_array->entry[i++].target;
		uint32_t length = ioquv_length(IOQ_(have));

		w_seg->handle = htonl(xd->mr->rkey);
		w_seg->length = htonl(length);
		xdr_encode_hyper((uint32_t*)&w_seg->offset,
				 (uintptr_t)IOQ_(have)->v.vio_head);
	}

	head_uv->v.vio_tail = head_uv->v.vio_head
				+ xdr_rdma_header_length(rmsg);

	rpcrdma_dump_msg(head_uv, "clnthead", msg->rm_xid);
	rpcrdma_dump_msg(hold_uv, "clntcall", msg->rm_xid);

	/* actual send, callback will take care of cleanup */
	xdr_rdma_post_send_cb(xd, cbc, 2);
	return (true);
}

/** xdr_rdma_svc_flushout
 *
 * @param[IN] cbc	combined callback context
 *
 * called by svc_rdma_reply()
 */
bool
xdr_rdma_svc_flushout(struct rpc_rdma_cbc *cbc)
{
	RDMAXPRT *xprt;
	struct rpc_msg *msg;
	struct rdma_msg *cmsg;
	struct rdma_msg *rmsg;
	struct xdr_write_list *w_array;
	struct xdr_write_list *reply_array;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *work_uv;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* swap reply body from holdq to workq */
	TAILQ_CONCAT(&cbc->workq.ioq_uv.uvqh.qh, &cbc->holdq.ioq_uv.uvqh.qh, q);
	cbc->workq.ioq_uv.uvqh.qcount = cbc->holdq.ioq_uv.uvqh.qcount;
	cbc->holdq.ioq_uv.uvqh.qcount = 0;

	work_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
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

	/* usurp the holdq for the head, move to workq later */
	head_uv = IOQ_(xdr_ioq_uv_fetch(&cbc->holdq, &xprt->outbufs.uvqh,
					"sreply head", 1, IOQ_FLAG_NONE));

	/* entry was already added directly to the queue */
	head_uv->v.vio_head = head_uv->v.vio_base;
	/* tail adjusted below */
	head_uv->v.vio_wrap = (char *)head_uv->v.vio_base + xprt->sm_dr.sendsz;

	/* build the header that goes with the data */
	rmsg = m_(head_uv->v.vio_head);
	rmsg->rdma_xid = cmsg->rdma_xid;
	rmsg->rdma_vers = cmsg->rdma_vers;
	rmsg->rdma_credit = htonl(xprt->xa->credits);

	/* no read, write chunks. */
	rmsg->rdma_body.rdma_msg.rdma_reads = 0; /* htonl(0); */
	rmsg->rdma_body.rdma_msg.rdma_writes = 0; /* htonl(0); */

	reply_array = (wl_t *)cbc->reply_chunk;
	if (reply_array->present == 0) {
		rmsg->rdma_type = htonl(RDMA_MSG);

		/* no reply chunk either */
		rmsg->rdma_body.rdma_msg.rdma_reply = 0; /* htonl(0); */

		head_uv->v.vio_tail = head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);

		rpcrdma_dump_msg(head_uv, "sreply head", msg->rm_xid);
		rpcrdma_dump_msg(work_uv, "sreply body", msg->rm_xid);
	} else {
		uint32_t i = 0;
		uint32_t n = ntohl(reply_array->elements);

		rmsg->rdma_type = htonl(RDMA_NOMSG);

		/* reply chunk */
		w_array = (wl_t *)&rmsg->rdma_body.rdma_msg.rdma_reply;
		w_array->present = htonl(1);

		while (i < n) {
			struct xdr_rdma_segment *c_seg =
				&reply_array->entry[i].target;
			struct xdr_rdma_segment *w_seg =
				&w_array->entry[i++].target;
			uint32_t length = ntohl(c_seg->length);
			uint32_t k = length / xprt->sm_dr.sendsz;
			uint32_t m = length % xprt->sm_dr.sendsz;

			if (m) {
				/* need fractional buffer */
				k++;
			}

			/* ensure never asking for more buffers than allowed */
			if (k > xprt->xa->max_send_sge) {
				__warnx(TIRPC_DEBUG_FLAG_XDR,
					"%s() requested chunk %" PRIu32
					" is too long (%" PRIu32 ">%" PRIu32 ")",
					__func__, length, k,
					xprt->xa->max_send_sge);
				k = xprt->xa->max_send_sge;
			}

			*w_seg = *c_seg;

			/* sometimes, back-to-back buffers could be sent
			 * together.  releases of unused buffers and
			 * other events eventually scramble the buffers
			 * enough that there's no gain in efficiency.
			 */
			xdr_rdma_wait_write_cb(xprt, cbc, k, w_seg);

			while (0 < k--) {
				struct poolq_entry *have =
					TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh);

				TAILQ_REMOVE(&cbc->workq.ioq_uv.uvqh.qh, have, q);
				(cbc->workq.ioq_uv.uvqh.qcount)--;

				rpcrdma_dump_msg(IOQ_(have), "sreply body",
						 msg->rm_xid);
				xdr_ioq_uv_release(IOQ_(have));
			}
		}
		w_array->elements = htonl(i);

		head_uv->v.vio_tail = head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);
		rpcrdma_dump_msg(head_uv, "sreply head", msg->rm_xid);
	}

	/* actual send, callback will take care of cleanup */
	TAILQ_REMOVE(&cbc->holdq.ioq_uv.uvqh.qh, &head_uv->uvq, q);
	(cbc->holdq.ioq_uv.uvqh.qcount)--;
	(cbc->workq.ioq_uv.uvqh.qcount)++;
	TAILQ_INSERT_HEAD(&cbc->workq.ioq_uv.uvqh.qh, &head_uv->uvq, q);
	xdr_rdma_post_send_cb(xprt, cbc, cbc->workq.ioq_uv.uvqh.qcount);

	/* free the old inbuf we only kept for header */
	xdr_ioq_uv_release(cbc->call_uv);
	return (true);
}
