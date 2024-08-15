/*
 * Copyright (c) 2013 Linux Box Corporation.
 * Copyright (c) 2013-2017 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#include <err.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include <assert.h>

#include <intrinsic.h>
#include <misc/abstract_atomic.h>
#include "rpc_com.h"

#ifdef USE_RPC_RDMA
#include "rpc_rdma.h"
#endif

#include <rpc/xdr_ioq.h>

#define VREC_MAXBUFS 24

static uint64_t next_id;

#if 0				/* jemalloc docs warn about reclaim */
#define alloc_buffer(size) mem_aligned(0x8, (size))
#else
#define alloc_buffer(size) mem_alloc((size))
#endif				/* 0 */
#define free_buffer(addr,size) mem_free((addr), size)

struct xdr_ioq_uv *
xdr_ioq_uv_create(size_t size, u_int uio_flags)
{
	struct xdr_ioq_uv *uv = mem_zalloc(sizeof(struct xdr_ioq_uv));

	if (size) {
		uv->v.vio_base = alloc_buffer(size);
		uv->v.vio_head = uv->v.vio_base;
		uv->v.vio_tail = uv->v.vio_base;
		uv->v.vio_wrap = uv->v.vio_base + size;
		/* ensure not wrapping to zero */
		assert(uv->v.vio_base < uv->v.vio_wrap);
	}
	uv->u.uio_flags = uio_flags;
	uv->u.uio_references = 1;	/* starting one */

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() uv %p size %lu",
		__func__, uv, (unsigned long) size);

	return (uv);
}

struct poolq_entry *
xdr_ioq_uv_fetch(struct xdr_ioq *xioq, struct poolq_head *ioqh,
		 char *comment, u_int count, u_int ioq_flags)
{
	struct poolq_entry *have = NULL;

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() %u %s",
		__func__, count, comment);
	pthread_mutex_lock(&ioqh->qmutex);

	while (count--) {
		if (likely(0 < ioqh->qcount--)) {
			/* positive for buffer(s) */
			have = TAILQ_FIRST(&ioqh->qh);
			TAILQ_REMOVE(&ioqh->qh, have, q);

			/* added directly to the queue.
			 * this lock is needed for context header queues,
			 * but is not a burden on uncontested data queues.
			 */
			pthread_mutex_lock(&xioq->ioq_uv.uvqh.qmutex);
			(xioq->ioq_uv.uvqh.qcount)++;
			TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, have, q);
			pthread_mutex_unlock(&xioq->ioq_uv.uvqh.qmutex);
		} else {
			u_int saved = xioq->xdrs[0].x_handy;

			/* negative for waiting worker(s):
			 * use the otherwise empty pool to hold them,
			 * simplifying mutex and pointer setup.
			 */
			TAILQ_INSERT_TAIL(&ioqh->qh, &xioq->ioq_s, q);

			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() waiting for %u %s",
				__func__, count, comment);

			/* Note: the mutex is the pool _head,
			 * but the condition is per worker,
			 * making the signal efficient!
			 *
			 * Nota Bene: count was already decremented,
			 * will be zero for last one needed,
			 * then will wrap as unsigned.
			 */
			xioq->xdrs[0].x_handy = count;
			pthread_cond_wait(&xioq->ioq_cond, &ioqh->qmutex);
			xioq->xdrs[0].x_handy = saved;

			/* entry was already added directly to the queue */
			have = TAILQ_LAST(&xioq->ioq_uv.uvqh.qh, poolq_head_s);
		}
	}

	pthread_mutex_unlock(&ioqh->qmutex);
	return have;
}

struct poolq_entry *
xdr_ioq_uv_fetch_nothing(struct xdr_ioq *xioq, struct poolq_head *ioqh,
			 char *comment, u_int count, u_int ioq_flags)
{
	return NULL;
}

static inline void
xdr_ioq_uv_recycle(struct poolq_head *ioqh, struct poolq_entry *have)
{
	pthread_mutex_lock(&ioqh->qmutex);

	if (likely(0 <= ioqh->qcount++)) {
		/* positive for buffer(s) */
		TAILQ_INSERT_TAIL(&ioqh->qh, have, q);
	} else {
		/* negative for waiting worker(s) */
		struct xdr_ioq *wait = _IOQ(TAILQ_FIRST(&ioqh->qh));

		/* added directly to the queue.
		 * no need to lock here, the mutex is the pool _head.
		 */
		(wait->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&wait->ioq_uv.uvqh.qh, have, q);

		/* Nota Bene: x_handy was decremented count,
		 * will be zero for last one needed,
		 * then will wrap as unsigned.
		 */
		if (0 < wait->xdrs[0].x_handy--) {
			/* not removed */
			ioqh->qcount--;
		} else {
			TAILQ_REMOVE(&ioqh->qh, &wait->ioq_s, q);
			pthread_cond_signal(&wait->ioq_cond);
		}
	}

	pthread_mutex_unlock(&ioqh->qmutex);
}

#ifdef USE_RPC_RDMA

static struct rpc_io_bufs *
get_parent_chunk(struct poolq_entry *have)
{
	struct xdr_ioq_uv *uv = IOQ_(have);
	return uv->u.uio_u1;
}

static int
chunk_ref_locked(struct poolq_entry *have)
{
	struct rpc_io_bufs *io_buf = get_parent_chunk(have);
	RDMAXPRT *rdma_xprt = (RDMAXPRT *)io_buf->ctx;

	if ((io_buf != rdma_xprt->first_io_buf) &&
	    ((io_buf->type == IO_INBUF_DATA) ||
	    (io_buf->type == IO_OUTBUF_DATA))) {
		atomic_inc_uint64_t(&rdma_xprt->total_extra_buf_allocations);
		clock_gettime(CLOCK_MONOTONIC_FAST,
		    &rdma_xprt->last_extra_buf_allocation_time);
	}

	return atomic_inc_uint32_t(&io_buf->refs);
}

static void
do_shrink(RDMAXPRT *rdma_xprt, struct rpc_io_bufs *io_buf)
{
	struct poolq_head *ioqh = NULL;

	switch (io_buf->type) {
		case IO_INBUF_HDR:
			break;
		case IO_INBUF_DATA:
			ioqh = &rdma_xprt->inbufs_data.uvqh;
			break;
		case IO_OUTBUF_HDR:
			break;
		case IO_OUTBUF_DATA:
			ioqh = &rdma_xprt->outbufs_data.uvqh;
			break;
		default:
			assert(io_buf->type == IO_BUF_ALL);
	}

	assert(ioqh);

	__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s: Start shrinking xprt %p "
	    "io_buf %p refs %d ioqh %p count %d",
	    __func__, rdma_xprt, io_buf, io_buf->refs, ioqh,
	    ioqh->qcount);

	xdr_rdma_buf_pool_destroy_locked(ioqh, io_buf);
}

#define NSEC_IN_SEC (1000ULL*1000ULL*1000ULL)
#define SHRINK_WAIT_TIME_NS (NSEC_IN_SEC * 60ULL)

static struct rpc_io_bufs *
get_lru_chunk(RDMAXPRT *rdma_xprt)
{
	struct timespec ts_end, ts_start;
	uint64_t diff_ns = 0;
	struct rpc_io_bufs *io_buf = NULL;

	clock_gettime(CLOCK_MONOTONIC_FAST, &ts_end);
	ts_start = rdma_xprt->last_extra_buf_allocation_time;

	if (ts_end.tv_sec > ts_start.tv_sec)
		diff_ns = timespec_diff(&ts_start, &ts_end);

	if (diff_ns >= SHRINK_WAIT_TIME_NS) {
		pthread_mutex_lock(&rdma_xprt->io_bufs.qmutex);
		struct poolq_entry *have = TAILQ_FIRST(&rdma_xprt->io_bufs.qh);
		while (have) {
			io_buf = opr_containerof(have, struct rpc_io_bufs, q);

			/* We are with ioqh_lock so no other thread can allocated
			 * from thi ioqh and get io_buf ref */
			if (io_buf->ready && (io_buf != rdma_xprt->first_io_buf) &&
			    (io_buf->refs == 0) && ((io_buf->type == IO_INBUF_DATA) ||
			    (io_buf->type == IO_OUTBUF_DATA)))
				break;
			struct poolq_entry *next = TAILQ_NEXT(have, q);
			have = next;
			io_buf = NULL;
		}
		if (io_buf) {
			TAILQ_REMOVE(&rdma_xprt->io_bufs.qh, &io_buf->q, q);
			rdma_xprt->io_bufs_count--;
			rdma_xprt->io_bufs.qcount--;

			__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s: xprt %p shrink io_buf %p"
			    "refs %d io_bufs count %d %d",
			    __func__, rdma_xprt, io_buf, io_buf->refs,
			    rdma_xprt->io_bufs_count, rdma_xprt->io_bufs.qcount);
		}
		pthread_mutex_unlock(&rdma_xprt->io_bufs.qmutex);
	}

	return io_buf;
}

static int
chunk_unref_locked(struct poolq_entry *have)
{
	struct rpc_io_bufs *io_buf = get_parent_chunk(have);
	uint32_t refs = atomic_dec_uint32_t(&io_buf->refs);
	RDMAXPRT *rdma_xprt = (RDMAXPRT *)io_buf->ctx;

	if ((io_buf != rdma_xprt->first_io_buf) &&
	    ((io_buf->type == IO_INBUF_DATA) ||
	    (io_buf->type == IO_OUTBUF_DATA)))
		atomic_dec_uint64_t(&rdma_xprt->total_extra_buf_allocations);

	io_buf = get_lru_chunk(rdma_xprt);
	if (io_buf) {
		do_shrink(rdma_xprt, io_buf);
	}
	return refs;
}

static bool
parent_chunk(struct poolq_entry *have,
    struct rpc_io_bufs *check_buf)
{
	struct rpc_io_bufs *io_buf = get_parent_chunk(have);
	return (io_buf == check_buf);
}

struct poolq_entry *
xdr_rdma_ioq_uv_fetch(struct xdr_ioq *xioq, struct poolq_head *ioqh,
		 char *comment, u_int count, u_int ioq_flags)
{
	struct poolq_entry *have = NULL;
	RDMAXPRT *rdma_xprt = NULL;

	assert(xioq->rdma_ioq);

	rdma_xprt = xioq->xdrs[0].x_lib[1];

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() %u %s rdma_xprt %p",
		__func__, count, comment, rdma_xprt);

	pthread_mutex_lock(&ioqh->qmutex);

	while (1) {
		/* positive for buffer(s) */
		have = TAILQ_FIRST(&ioqh->qh);
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %u %s xioq %p %d ioqh %p %d have %p",
			__func__, count, comment, xioq, xioq->ioq_uv.uvqh.qcount,
			ioqh, ioqh->qcount, have);
		if (have) {
			TAILQ_REMOVE(&ioqh->qh, have, q);
			if (ioqh != &rdma_xprt->cbqh)
				chunk_ref_locked(have);

			/* added directly to the queue.
			 * this lock is needed for context header queues,
			 * but is not a burden on uncontested data queues.
			 */
			pthread_mutex_lock(&xioq->ioq_uv.uvqh.qmutex);
			(xioq->ioq_uv.uvqh.qcount)++;
			__warnx(TIRPC_DEBUG_FLAG_XDR, "ioq_track xdr_ioq_uv_fetch insert "
				"have %p to q %p", have, xioq);
			TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, have, q);
			pthread_mutex_unlock(&xioq->ioq_uv.uvqh.qmutex);
			ioqh->qcount--;
			count--;
			if(count == 0)
				break;
		} else {
			if (rdma_xprt) {
				if (ioqh == &rdma_xprt->inbufs_data.uvqh) {
					xdr_rdma_add_inbufs_data(rdma_xprt);
				}

				if (ioqh == &rdma_xprt->outbufs_data.uvqh) {
					xdr_rdma_add_outbufs_data(rdma_xprt);
				}

				if (ioqh == &rdma_xprt->inbufs_hdr.uvqh) {
					xdr_rdma_add_inbufs_hdr(rdma_xprt);
				}

				if (ioqh == &rdma_xprt->outbufs_hdr.uvqh) {
					xdr_rdma_add_outbufs_hdr(rdma_xprt);
				}

				if (unlikely(ioqh == &rdma_xprt->cbqh)) {
					__warnx(TIRPC_DEBUG_FLAG_EVENT, "cbc buffers exhausetd rdma_xprt %p "
						"ioqh %p qcount %d", rdma_xprt, ioqh, ioqh->qcount);
					rpc_rdma_allocate_cbc_locked(ioqh);
				}

			}
		}
	}

	pthread_mutex_unlock(&ioqh->qmutex);
	return have;
}

struct poolq_entry *
xdr_rdma_ioq_uv_fetch_nothing(struct xdr_ioq *xioq, struct poolq_head *ioqh,
			 char *comment, u_int count, u_int ioq_flags)
{
	return NULL;
}

static inline void
xdr_rdma_ioq_uv_recycle(struct poolq_head *ioqh, struct poolq_entry *have)
{
	pthread_mutex_lock(&ioqh->qmutex);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s() ioq_track Reccycle ioqh %p %d have %p",
		__func__, ioqh, ioqh->qcount, have);

	TAILQ_INSERT_TAIL(&ioqh->qh, have, q);
	ioqh->qcount++;

	pthread_mutex_unlock(&ioqh->qmutex);
}

static inline void
xdr_rdma_ioq_uv_recycle_io_buf(struct poolq_head *ioqh,
    struct poolq_entry *have)
{
	pthread_mutex_lock(&ioqh->qmutex);

	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s() ioq_track Reccycle ioqh %p %d have %p",
		__func__, ioqh, ioqh->qcount, have);

	struct rpc_io_bufs *io_buf = get_parent_chunk(have);
	RDMAXPRT *rdma_xprt = (RDMAXPRT *)io_buf->ctx;

	if (io_buf == rdma_xprt->first_io_buf)
		TAILQ_INSERT_HEAD(&ioqh->qh, have, q);
	else
		TAILQ_INSERT_TAIL(&ioqh->qh, have, q);
	ioqh->qcount++;

	if (ioqh != &rdma_xprt->cbqh)
		chunk_unref_locked(have);

	pthread_mutex_unlock(&ioqh->qmutex);
}

void
xdr_rdma_ioq_uv_release(struct xdr_ioq_uv *uv)
{
	/* Reset vectors since we use it to UIO_REFER */
	uv->u = uv->rdma_u;
	uv->v = uv->rdma_v;
	xdr_rdma_ioq_uv_recycle_io_buf(uv->u.uio_p1, &uv->uvq);
}

void
xdr_rdma_ioq_release(struct poolq_head *ioqh, bool xioq_recycle,
    struct xdr_ioq *xioq)
{
	struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

	/* release queued buffers */
	while (have) {
		assert(xioq->rdma_ioq);

		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s ioqh %p %d "
		    "xioq %p have %p", __func__,
		    ioqh, ioqh->qcount, xioq, have);

		pthread_mutex_lock(&ioqh->qmutex);

		TAILQ_REMOVE(&ioqh->qh, have, q);
		(ioqh->qcount)--;

		pthread_mutex_unlock(&ioqh->qmutex);

		xdr_rdma_ioq_uv_release(IOQ_(have));

		have = TAILQ_FIRST(&ioqh->qh);
	}

	assert(ioqh->qcount == 0);

	/* Recycle cbc */
	if (xioq && xioq->ioq_pool && xioq_recycle) {
		xdr_rdma_ioq_uv_recycle(xioq->ioq_pool, &xioq->ioq_s);
	}

}

static void
xdr_rdma_ioq_uv_destroy(struct xdr_ioq_uv *uv)
{
	mem_free(uv, sizeof(*uv));
}

void
xdr_rdma_buf_pool_destroy(struct poolq_head *ioqh,
    struct rpc_io_bufs *io_buf)
{
	pthread_mutex_lock(&ioqh->qmutex);
	xdr_rdma_buf_pool_destroy_locked(ioqh, io_buf);
	pthread_mutex_unlock(&ioqh->qmutex);
}

void
xdr_rdma_buf_pool_destroy_locked(struct poolq_head *ioqh,
    struct rpc_io_bufs *io_buf)
{
	/* pool_head may not be initialized, so check for qcount */
	if (ioqh->qcount && !TAILQ_EMPTY(&ioqh->qh)) {

		assert(io_buf->refs == 0);

		struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

		/* release queued buffers */
		while (have) {
			struct poolq_entry *next = TAILQ_NEXT(have, q);

			__warnx(TIRPC_DEBUG_FLAG_XDR,
			    "%s ioqh %p %d have %p",
			    __func__, ioqh, ioqh->qcount, have);

			if (io_buf && !parent_chunk(have, io_buf)) {
				have = next;
				continue;
			}


			TAILQ_REMOVE(&ioqh->qh, have, q);
			(ioqh->qcount)--;

			atomic_dec_uint64_t(&io_buf->buf_count);

			xdr_rdma_ioq_uv_destroy(IOQ_(have));
			have = next;
		}
		if (0 == atomic_fetch_uint64_t(&io_buf->buf_count)) {
			RDMAXPRT *rdma_xprt = (RDMAXPRT *)io_buf->ctx;
			assert(io_buf->mr);
			assert(!xdr_rdma_dereg_mr(rdma_xprt, io_buf->mr,
			    io_buf->buffer_aligned, io_buf->buffer_total));
			io_buf->mr = NULL;

			__warnx(TIRPC_DEBUG_FLAG_EVENT, "%s() Free xprt %p mr "
			    "io_bufs %p size %u io_buf %p", __func__, rdma_xprt,
			    io_buf->buffer_aligned, io_buf->buffer_total, io_buf);

			assert(io_buf->buffer_aligned);
			mem_free(io_buf->buffer_aligned, io_buf->buffer_total);
			io_buf->buffer_aligned = NULL;
		}
	}
}

#endif

void
xdr_ioq_uv_release(struct xdr_ioq_uv *uv)
{
	if (!(--uv->u.uio_references)) {
		if (uv->u.uio_release) {
			/* handle both xdr_ioq_uv and vio */
			uv->u.uio_release(&uv->u, UIO_FLAG_NONE);
		} else if (uv->u.uio_flags & UIO_FLAG_REFER) {
			/* not optional in this case! */
			__warnx(TIRPC_DEBUG_FLAG_XDR, "Call uio_release");
			uv->u.uio_refer->uio_release(uv->u.uio_refer,
						     UIO_FLAG_NONE);
			mem_free(uv, sizeof(*uv));
		} else if (uv->u.uio_flags & UIO_FLAG_FREE) {
			free_buffer(uv->v.vio_base, ioquv_size(uv));
			mem_free(uv, sizeof(*uv));
		} else if (uv->u.uio_flags & UIO_FLAG_BUFQ) {
			uv->u.uio_references = 1;	/* keeping one */
			xdr_ioq_uv_recycle(uv->u.uio_p1, &uv->uvq);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() memory leak, no release flags (%u)\n",
				__func__, uv->u.uio_flags);
			abort();
		}
	}
}

/*
 * Set current read/insert or fill position.
 */
static inline void
xdr_ioq_uv_reset(struct xdr_ioq *xioq, struct xdr_ioq_uv *uv)
{
	xioq->xdrs[0].x_data = uv->v.vio_head;
	xioq->xdrs[0].x_base = &uv->v;
	xioq->xdrs[0].x_v = uv->v;
}

/*
 * Update read/insert or fill position.
 */
static inline void
xdr_ioq_uv_update(struct xdr_ioq *xioq, struct xdr_ioq_uv *uv)
{
	xdr_ioq_uv_reset(xioq, uv);
	(xioq->ioq_uv.pcount)++;
	/* xioq->ioq_uv.plength is calculated in xdr_ioq_uv_advance() */
}

/*
 * Set initial read/insert or fill position.
 *
 * Note: must be done before any XDR_[GET|SET]POS()
 */
void
xdr_ioq_reset(struct xdr_ioq *xioq, u_int wh_pos)
{
	struct xdr_ioq_uv *uv = IOQ_(TAILQ_FIRST(&xioq->ioq_uv.uvqh.qh));

	xioq->ioq_uv.plength =
	xioq->ioq_uv.pcount = 0;

	if (wh_pos >= ioquv_size(uv)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xioq %p wh_pos %d too big, ignored!\n",
			__func__, xioq, wh_pos);
	} else {
		uv->v.vio_head = uv->v.vio_base + wh_pos;
	}
	xdr_ioq_uv_reset(xioq, uv);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() xioq %p head %p wh_pos %d",
		__func__, xioq, uv->v.vio_head, wh_pos);
}

void
xdr_ioq_setup(struct xdr_ioq *xioq)
{
	XDR *xdrs = xioq->xdrs;

	/* the XDR is the top element of struct xdr_ioq */
	assert((void *)xdrs == (void *)xioq);

	TAILQ_INIT_ENTRY(&xioq->ioq_s, q);
	xioq->ioq_s.qflags = IOQ_FLAG_SEGMENT;

	poolq_head_setup(&xioq->ioq_uv.uvqh);
	pthread_cond_init(&xioq->ioq_cond, NULL);

	xdrs->x_ops = &xdr_ioq_ops;
	xdrs->x_op = XDR_ENCODE;
	xdrs->x_public = NULL;
	xdrs->x_private = NULL;
	xdrs->x_data = NULL;
	xdrs->x_base = NULL;
	xdrs->x_flags = XDR_FLAG_VIO;

	xioq->id = atomic_inc_uint64_t(&next_id);
}

struct xdr_ioq *
xdr_ioq_create(size_t min_bsize, size_t max_bsize, u_int uio_flags)
{
	struct xdr_ioq *xioq = mem_zalloc(sizeof(struct xdr_ioq));

	xdr_ioq_setup(xioq);
	xioq->xdrs[0].x_flags |= XDR_FLAG_FREE;
	xioq->ioq_uv.min_bsize = min_bsize;
	xioq->ioq_uv.max_bsize = max_bsize;

	if (!(uio_flags & UIO_FLAG_BUFQ)) {
		struct xdr_ioq_uv *uv = xdr_ioq_uv_create(min_bsize, uio_flags);
		xioq->ioq_uv.uvqh.qcount = 1;
		TAILQ_INSERT_HEAD(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
		xdr_ioq_reset(xioq, 0);
	}

	return (xioq);
}

/*
 * Advance read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 * All such updates are consolidated here and getpos/setpos,
 * reducing computations in the get/put/inline routines.
 */
static inline struct xdr_ioq_uv *
xdr_ioq_uv_advance(struct xdr_ioq *xioq)
{
	struct xdr_ioq_uv *uv = IOQV(xioq->xdrs[0].x_base);
	size_t len;

	/* update the most recent data length */
	xdr_tail_update(xioq->xdrs);

	len = ioquv_length(uv);
	xioq->ioq_uv.plength += len;

#if 0
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() uv %p len %lu plength %lu NEXT %p",
		__func__, uv, (unsigned long) len, (unsigned long) xioq->ioq_uv.plength,
		IOQ_(TAILQ_NEXT(&uv->uvq, q)));
#endif

	/* next buffer, if any */
	return IOQ_(TAILQ_NEXT(&uv->uvq, q));
}

/*
 * Append at read/insert or fill position.
 */
static struct xdr_ioq_uv *
xdr_ioq_uv_append(struct xdr_ioq *xioq, u_int ioq_flags)
{
	struct xdr_ioq_uv *uv = IOQV(xioq->xdrs[0].x_base);

	if (xioq->ioq_uv.uvq_fetch) {
		/* more of the same kind */
		struct poolq_entry *have =
			xioq->ioq_uv.uvq_fetch(xioq, uv->u.uio_p1,
						"next buffer", 1,
						IOQ_FLAG_NONE);

		/* poolq_entry is the top element of xdr_ioq_uv */
		uv = IOQ_(have);
		assert((void *)uv == (void *)have);
	} else if (ioq_flags & IOQ_FLAG_BALLOC) {
		/* XXX workaround for lack of segmented buffer
		 * interfaces in some callers (e.g, GSS_WRAP) */
		if (uv->u.uio_flags & UIO_FLAG_REALLOC) {
			uint8_t *base;
			size_t size = ioquv_size(uv);
			size_t delta = xdr_tail_inline(xioq->xdrs);
			size_t len = ioquv_length(uv);

			/* bail if we have reached max bufsz */
			if (size >= xioq->ioq_uv.max_bsize)
				return (NULL);

			/* backtrack */
			xioq->ioq_uv.plength -= len;
			assert(uv->u.uio_flags & UIO_FLAG_FREE);

			base = mem_alloc(xioq->ioq_uv.max_bsize);
			memcpy(base, uv->v.vio_head, len);
			mem_free(uv->v.vio_base, size);
			uv->v.vio_base =
			uv->v.vio_head = base + 0;
			uv->v.vio_tail = base + len;
			uv->v.vio_wrap = base + xioq->ioq_uv.max_bsize;
			xioq->xdrs[0].x_v = uv->v;
			xioq->xdrs[0].x_data = uv->v.vio_tail - delta;
			return (uv);
		}
		uv = xdr_ioq_uv_create(xioq->ioq_uv.min_bsize, UIO_FLAG_FREE);
		(xioq->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
	} else {
		/* XXX empty buffer slot (not supported for now) */
		uv = xdr_ioq_uv_create(0, UIO_FLAG_NONE);
		(xioq->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
	}

	xdr_ioq_uv_update(xioq, uv);
	return (uv);
}

static bool
xdr_ioq_getunit(XDR *xdrs, uint32_t *p)
{
	struct xdr_ioq_uv *uv;
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	while (future > xdrs->x_v.vio_tail) {
		if (unlikely(xdrs->x_data != xdrs->x_v.vio_tail)) {
			/* FIXME: insufficient data or unaligned? stop! */
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() x_data != x_v.vio_tail\n",
				__func__);
			return (false);
		}

		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv) {
			return (false);
		}
		xdr_ioq_uv_update(XIOQ(xdrs), uv);
		/* fill pointer has changed */
		future = xdrs->x_data + sizeof(uint32_t);
	}

	*p = (uint32_t)ntohl(*((uint32_t *) (xdrs->x_data)));
	xdrs->x_data = future;
	return (true);
}

static bool
xdr_ioq_putunit(XDR *xdrs, const uint32_t v)
{
	struct xdr_ioq_uv *uv;
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	while (future > xdrs->x_v.vio_wrap) {
		/* advance fill pointer, skipping unaligned */
		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv) {
			uv = xdr_ioq_uv_append(XIOQ(xdrs), IOQ_FLAG_BALLOC);
		} else {
			xdr_ioq_uv_update(XIOQ(xdrs), uv);
		}
		/* fill pointer has changed */
		future = xdrs->x_data + sizeof(uint32_t);
	}

	*((uint32_t *) (xdrs->x_data)) = (uint32_t) htonl(v);
	xdrs->x_data = future;
	return (true);
}

/* in glibc 2.14+ x86_64, memcpy no longer tries to handle overlapping areas,
 * see Fedora Bug 691336 (NOTABUG); we dont permit overlapping segments,
 * so memcpy may be a small win over memmove.
 */

static bool
xdr_ioq_getbytes(XDR *xdrs, char *addr, u_int len)
{
	struct xdr_ioq_uv *uv;
	ssize_t delta;

	while (len > 0
		&& XIOQ(xdrs)->ioq_uv.pcount < XIOQ(xdrs)->ioq_uv.uvqh.qcount) {
		delta = (uintptr_t)xdrs->x_v.vio_tail
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (unlikely(!delta)) {
			/* advance fill pointer */
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv) {
				return (false);
			}
			xdr_ioq_uv_update(XIOQ(xdrs), uv);
			continue;
		}
		memcpy(addr, xdrs->x_data, delta);
		xdrs->x_data += delta;
		addr += delta;
		len -= delta;
	}

	/* assert(len == 0); */
	return (true);
}

#ifdef USE_RPC_RDMA

static void
xdr_ioq_destroy_internal_rdma(XDR *xdrs)
{
	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: no op for rdma",
	    __func__);
}

static bool
xdr_ioq_getbytes_rdma(XDR *xdrs, char *addr, u_int len)
{
	struct xdr_ioq_uv *uv;
	ssize_t delta;
	int pcount = 0;

	struct xdr_ioq *xioq = XIOQ(xdrs);
	XDR orig_xdr;
	int restore_xdr = 0;

	__warnx(TIRPC_DEBUG_FLAG_XDR, "enter %s: xdata %p xioq %p rdma %d len %u "
	    "pcount %d qcount %d", __func__, xdrs->x_data, xioq, xioq->rdma_ioq,
	    len, XIOQ(xdrs)->ioq_uv.pcount, XIOQ(xdrs)->ioq_uv.uvqh.qcount);

	/* Check if we are getting rdma_write bytes, we could have some
	 * header part to get next compound, so restore hdr xdr at end */
	if (xioq->rdma_ioq &&
	    (len > ((uintptr_t)xdrs->x_v.vio_tail - (uintptr_t)xdrs->x_data))) {

		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: rdma_write len %u hdr delta %u",
		    __func__, len, ((uintptr_t)xdrs->x_v.vio_tail -
		    (uintptr_t)xdrs->x_data));

		memcpy(&orig_xdr, xdrs, sizeof(XDR));
		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv) {
			__warnx(TIRPC_DEBUG_FLAG_XDR, "%s NULL uv", __func__);
			return (false);
		}
		xdr_ioq_uv_update(XIOQ(xdrs), uv);
		restore_xdr = 1;
	}

	while (len > 0
		&& XIOQ(xdrs)->ioq_uv.pcount < XIOQ(xdrs)->ioq_uv.uvqh.qcount) {
		delta = (uintptr_t)xdrs->x_v.vio_tail
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (unlikely(!delta)) {
			/* advance fill pointer */
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv) {
				return (false);
			}
			xdr_ioq_uv_update(XIOQ(xdrs), uv);
			continue;
		}
		memcpy(addr, xdrs->x_data, delta);
		xdrs->x_data += delta;
		addr += delta;
		len -= delta;
	}

	pcount = XIOQ(xdrs)->ioq_uv.pcount;

	if (restore_xdr) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: rdma_write restore xdr hdr",
		    __func__);
		memcpy(xdrs, &orig_xdr, sizeof(XDR));
		XIOQ(xdrs)->ioq_uv.pcount = pcount;
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR, "exit %s: xdata %p xioq %p rdma %d len %u "
	    "pcount %d qcount %d", __func__, xdrs->x_data, xioq, xioq->rdma_ioq,
	    len, XIOQ(xdrs)->ioq_uv.pcount, XIOQ(xdrs)->ioq_uv.uvqh.qcount);

	assert(len == 0);
	return (true);
}

#endif

static bool
xdr_ioq_putbytes(XDR *xdrs, const char *addr, u_int len)
{
	struct xdr_ioq_uv *uv;
	ssize_t delta;

	while (len > 0) {
		delta = (uintptr_t)xdrs->x_v.vio_wrap
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (!delta) {
			/* advance fill pointer */
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv) {
				uv = xdr_ioq_uv_append(XIOQ(xdrs),
							IOQ_FLAG_BALLOC);
			} else {
				xdr_ioq_uv_update(XIOQ(xdrs), uv);
			}
			continue;
		}
		memcpy(xdrs->x_data, addr, delta);
		xdrs->x_data += delta;
		addr += delta;
		len -= delta;
	}
	return (true);
}

/* Get buffers from the queue. */
static bool
xdr_ioq_getbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
    /* XXX finalize */
#if 0

	struct xdr_ioq_uv *uv;
	ssize_t delta;
	int ix;

	/* allocate sufficient slots to empty the queue, else MAX */
	uio->xbs_cnt = XIOQ(xdrs)->ioq_uv.uvqh.qsize - XIOQ(xdrs)->ioq_uv.pcount;
	if (uio->xbs_cnt > VREC_MAXBUFS) {
		uio->xbs_cnt = VREC_MAXBUFS;
	}

	/* fail if no segments available */
	if (unlikely(! uio->xbs_cnt))
		return (FALSE);

	uio->xbs_buf = mem_alloc(uio->xbs_cnt);
	uio->xbs_resid = 0;
	ix = 0;

	/* re-consuming bytes in a stream (after SETPOS/rewind) */
	while (len > 0
		&& XIOQ(xdrs)->ioq_uv.pcount < XIOQ(xdrs)->ioq_uv.uvqh.qcount) {
		delta = (uintptr_t)XDR_VIO(xioq->xdrs)->vio_tail
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (unlikely(!delta)) {
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv)
				return (false);

			xdr_ioq_uv_update(XIOQ(xdrs), uv);
			continue;
		}
		(uio->xbs_buf[ix]).xb_p1 = uv;
		uv->u.uio_references)++;
		(uio->xbs_buf[ix]).xb_base = xdrs->x_data;
		XIOQ(xdrs)->ioq_uv.plength += delta;
		xdrs->x_data += delta;
		len -= delta;
	}
#endif /* 0 */

	/* assert(len == 0); */
	return (TRUE);
}

/* Post buffers on the queue, or, if indicated in flags, return buffers
 * referenced with getbufs. */
static bool
xdr_ioq_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
	struct xdr_ioq_uv *uv;
	xdr_vio *v;
	int ix;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s Before putbufs - pos %lu",
		__func__, (unsigned long) XDR_GETPOS(xdrs));

	for (ix = 0; ix < uio->uio_count; ++ix) {
		/* advance fill pointer, do not allocate buffers, refs =1 */
		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv)
			uv = xdr_ioq_uv_append(XIOQ(xdrs), flags);
		else
			xdr_ioq_uv_update(XIOQ(xdrs), uv);

		v = &(uio->uio_vio[ix]);
		uv->u.uio_flags = UIO_FLAG_REFER;
		uv->v = *v;

		/* save original buffer sequence for rele */
		uv->u.uio_refer = uio;
		(uio->uio_references)++;

		/* Now update the XDR position */
		xdrs->x_data = uv->v.vio_tail;
		xdrs->x_base = &uv->v;
		xdrs->x_v = uv->v;

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s After putbufs Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p len %lu full %lu) pos %lu",
			__func__, uv, uv->v.vio_base, uv->v.vio_head,
			uv->v.vio_tail, uv->v.vio_wrap,
			(unsigned long) ioquv_length(uv),
			(unsigned long) (uintptr_t)xdrs->x_v.vio_wrap
					- (uintptr_t)xdrs->x_v.vio_head,
			(unsigned long) XDR_GETPOS(xdrs));
	}

	return (TRUE);
#if 0
Saved for later golden buttery results -- Matt
	if (flags & XDR_PUTBUFS_FLAG_BRELE) {
		/* the caller is returning buffers */
		for (ix = 0; ix < uio->xbs_cnt; ++ix) {
			uv = (struct xdr_ioq_uv *)(uio->xbs_buf[ix]).xb_p1;
			xdr_ioq_uv_release(uv);
		}
		mem_free(uio->xbs_buf, 0);
		break;
	} else {
		for (ix = 0; ix < uio->xbs_cnt; ++ix) {
			/* advance fill pointer, do not allocate buffers */
			*uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv)
				uv = xdr_ioq_uv_append(XIOQ(xdrs),
							IOQ_FLAG_NONE);
			else
				xdr_ioq_uv_update(XIOQ(xdrs), uv);

			xbuf = &(uio->xbs_buf[ix]);
			XIOQ(xdrs)->ioq_uv.plength += xbuf->xb_len;
			uv->u.uio_flags = UIO_FLAG_NONE;	/* !RECLAIM */
			uv->u.uio_references =
			    (xbuf->xb_flags & UIO_FLAG_GIFT) ? 0 : 1;
			uv->v.vio_base = xbuf->xb_base;
			uv->v.vio_wrap = xbuf->xb_len;
			uv->v.vio_tail = xbuf->xb_len;
			uv->v.vio_head = 0;
		}
	}
		/* save original buffer sequence for rele */
		if (ix == 0) {
			uv->u.uio_refer = uio;
			(uio->uio_references)++;
		}
	}

	return (TRUE);
#endif
}

/*
 * Get read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 */
static u_int
xdr_ioq_getpos(XDR *xdrs)
{
	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	return (XIOQ(xdrs)->ioq_uv.plength
		+ ((uintptr_t)xdrs->x_data
		   - (uintptr_t)xdrs->x_v.vio_head));
}

/*
 * Set read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 */
static bool
xdr_ioq_setpos(XDR *xdrs, u_int pos)
{
	struct poolq_entry *have;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	XIOQ(xdrs)->ioq_uv.plength =
	XIOQ(xdrs)->ioq_uv.pcount = 0;

	TAILQ_FOREACH(have, &(XIOQ(xdrs)->ioq_uv.uvqh.qh), q) {
		struct xdr_ioq_uv *uv = IOQ_(have);
		struct xdr_ioq_uv *next = IOQ_(TAILQ_NEXT(have, q));
		u_int len = ioquv_length(uv);
		u_int full = (uintptr_t)xdrs->x_v.vio_wrap
			   - (uintptr_t)xdrs->x_v.vio_head;

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p len %lu full %lu) - %s pos %lu",
			__func__, uv, uv->v.vio_base, uv->v.vio_head,
			uv->v.vio_tail, uv->v.vio_wrap,
			(unsigned long) len, (unsigned long) full,
			next ? "more" : "last",
			(unsigned long) pos);

		/* If we have a next buffer and pos would land exactly at the
		 * tail of this buffer, we want to force positioning in the
		 * next buffer. The space between the tail of this buffer and
		 * the wrap of this buffer is unused and MUST be skipped.
		 */
		if ((pos < len) || (next == NULL && pos <= full)) {
			/* allow up to the end of the buffer, unless there is
			 * a next buffer in which case only allow up to the
			 * tail assuming next operation will extend.
			 */
			xdrs->x_data = uv->v.vio_head + pos;
			xdrs->x_base = &uv->v;
			xdrs->x_v = uv->v;
			return (true);
		}
		pos -= len;
		XIOQ(xdrs)->ioq_uv.plength += len;
		XIOQ(xdrs)->ioq_uv.pcount++;
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s failing with remaining %lu",
		__func__, (unsigned long) pos);

	return (false);
}

void
xdr_ioq_release(struct poolq_head *ioqh)
{
	struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

	/* release queued buffers */
	while (have) {
		struct poolq_entry *next = TAILQ_NEXT(have, q);

		TAILQ_REMOVE(&ioqh->qh, have, q);
		(ioqh->qcount)--;

		if (have->qflags & IOQ_FLAG_SEGMENT) {
			xdr_ioq_destroy(_IOQ(have), have->qsize);
		} else {
			xdr_ioq_uv_release(IOQ_(have));
		}
		have = next;
	}
	assert(ioqh->qcount == 0);
}

void
xdr_ioq_destroy(struct xdr_ioq *xioq, size_t qsize)
{
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() xioq %p",
		__func__, xioq);

#ifdef USE_RPC_RDMA
	assert(!xioq->rdma_ioq);
#endif

	xdr_ioq_release(&xioq->ioq_uv.uvqh);

	if (xioq->ioq_pool) {
		xdr_ioq_uv_recycle(xioq->ioq_pool, &xioq->ioq_s);
		return;
	}
	poolq_head_destroy(&xioq->ioq_uv.uvqh);
	pthread_cond_destroy(&xioq->ioq_cond);

	if (xioq->xdrs[0].x_flags & XDR_FLAG_FREE) {
		mem_free(xioq, qsize);
	}
}

static void
xdr_ioq_destroy_internal(XDR *xdrs)
{
	xdr_ioq_destroy(XIOQ(xdrs), sizeof(struct xdr_ioq));
}

void
xdr_ioq_destroy_pool(struct poolq_head *ioqh)
{
	struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

	while (have) {
		struct poolq_entry *next = TAILQ_NEXT(have, q);

		TAILQ_REMOVE(&ioqh->qh, have, q);
		(ioqh->qcount)--;

		_IOQ(have)->ioq_pool = NULL;
		xdr_ioq_destroy(_IOQ(have), have->qsize);
		have = next;
	}
	assert(ioqh->qcount == 0);
	poolq_head_destroy(ioqh);
}

static bool
xdr_ioq_control(XDR *xdrs, /* const */ int rq, void *in)
{
	return (true);
}

static bool
xdr_ioq_newbuf(XDR *xdrs)
{
	struct xdr_ioq_uv *uv;

	/* We need to start a new buffer whether the current buffer is full or
	 * not.
	 */
	uv = xdr_ioq_uv_advance(XIOQ(xdrs));

	if (!uv)
		uv = xdr_ioq_uv_append(XIOQ(xdrs), IOQ_FLAG_BALLOC);
	else
		xdr_ioq_uv_update(XIOQ(xdrs), uv);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() uv %p",
		__func__, uv);

	/* At this point, the position has been updated to point to the
	 * start of the new buffer since xdr_ioq_uv_update has been called
	 * (it's called at the end of xdr_ioq_uv_append).
	 */
	return true;
}

static int
xdr_ioq_iovcount(XDR *xdrs, u_int start, u_int datalen)
{
	/* Buffers starts at -1 to indicate start has not yet been found */
	int buffers = -1;
	struct poolq_entry *have;
	struct xdr_ioq_uv *uv;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	TAILQ_FOREACH(have, &(XIOQ(xdrs)->ioq_uv.uvqh.qh), q) {
		u_int len;

		uv = IOQ_(have);
		len = ioquv_length(uv);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p) - start %lu len %lu buffers %d",
			__func__, uv, uv->v.vio_base, uv->v.vio_head,
			uv->v.vio_tail, uv->v.vio_wrap,
			(unsigned long) start, (unsigned long) len, buffers);

		if (buffers > 0) {
			/* Accumulate another buffer */
			buffers++;
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"Accumulated another buffer total = %d",
				buffers);
		} else if (start < len) {
			/* We have found the buffer that start begins. */
			buffers = 1;
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"Starting total = %d", buffers);
		} else {
			/* Keep looking, need to reduce start by the length of
			 * this buffer.
			 */
			start -= len;
		}
		if (buffers > 0) {
			/* Now we need to decrement the datalen to see if we're
			 * done. Note the first time we come in, start may not
			 * be zero, which represents the fact that start was in
			 * the middle of this buffer, just subtract the
			 * remaining start from the length of this buffer.
			 */
			u_int buflen = uv->v.vio_tail - uv->v.vio_head - start;
			if (buflen >= datalen) {
				/* We have found end. */
				datalen = 0;
				break;
			}

			/* Decrement the datalen, and zero out start for future
			 * buffers.
			 */
			datalen -= buflen;
			start = 0;
		}
	}

	if (datalen != 0) {
		/* There wasn't enough data... */
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s start %lu remain %lu",
			__func__, (unsigned long) start,
			(unsigned long) datalen);
			return -1;
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s start %lu buffers %d",
		__func__, (unsigned long) start, buffers);

	/* If start was not within the xdr stream, buffers will still be -1 */
	return buffers;
}

static bool
xdr_ioq_fillbufs(XDR *xdrs, u_int start, xdr_vio *vector, u_int datalen)
{
	bool found = false;
	struct poolq_entry *have;
	struct xdr_ioq_uv *uv;
	int idx = 0;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	TAILQ_FOREACH(have, &(XIOQ(xdrs)->ioq_uv.uvqh.qh), q) {
		u_int len;

		uv = IOQ_(have);
		len = ioquv_length(uv);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p len %lu) - %s start %lu remain %lu idx %d",
			__func__, uv, uv->v.vio_base, uv->v.vio_head,
			uv->v.vio_tail, uv->v.vio_wrap,
			(unsigned long) len,
			found ? "found" : "not found",
			(unsigned long) start, (unsigned long) datalen, idx);

		if (!found) {
			if (start < len) {
				/* We have found the buffer that start begins.
				 */
				found = true;
				__warnx(TIRPC_DEBUG_FLAG_XDR, "found");
			} else {
				/* Keep looking, need to reduce start by the
				 * length of this buffer.
				 */
				start -= len;
			}
		}

		if (found) {
			vector[idx] = uv->v;
			vector[idx].vio_type = VIO_DATA;

			if (start > 0) {
				/* The start position wasn't at the start of
				 * a buffer, adjust the vio_head of this buffer
				 * and len and then zero out start for
				 * future buffers.
				 */
				len -= start;
				vector[idx].vio_head += start;
				start = 0;
			}

			vector[idx].vio_length = len;

			if (datalen < vector[idx].vio_length) {
				/* This is the last buffer, and we're not using
				 * all of it, adjust vio_length and vio_tail.
				 */
				vector[idx].vio_length = datalen;
				vector[idx].vio_tail = vector[idx].vio_head
					+ datalen;
				datalen = 0;
				break;
			} else if (datalen == vector[idx].vio_length) {
				/* We have reached the end. */
				datalen = 0;
				break;
			}

			datalen -= vector[idx].vio_length;

			idx++;
		}
	}

	if (datalen != 0) {
		/* There wasn't enough data... */
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s start %lu remain %lu",
			__func__, (unsigned long) start,
			(unsigned long) datalen);
			return false;
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s %s start %lu remain %lu idx %d",
		__func__, found ? "found" : "not found",
		(unsigned long) start, (unsigned long) datalen, idx);

	return found;
}

static struct xdr_ioq_uv *
xdr_ioq_use_or_allocate(struct xdr_ioq *xioq, xdr_vio *v, struct xdr_ioq_uv *uv)
{
	struct poolq_entry *have = &uv->uvq, *have2;
	struct xdr_ioq_uv *uv2;

	/* We have a header or tailer, let's see if it fits in this buffer,
	 * otherwise allocate and insert a new buffer.
	 */
	uint32_t htlen = v->vio_length;

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p) size %lu length %lu has %lu looking for %lu",
		__func__, uv, uv->v.vio_base, uv->v.vio_head,
		uv->v.vio_tail, uv->v.vio_wrap,
		(unsigned long) ioquv_size(uv), (unsigned long) ioquv_length(uv),
		(unsigned long) ioquv_more(uv), htlen);

	if (ioquv_more(uv) >= htlen) {
		/* The HEADER or TRAILER will fit */
		v->vio_base = uv->v.vio_base;
		v->vio_head = uv->v.vio_tail;
		v->vio_tail = uv->v.vio_tail + htlen;
		v->vio_wrap = uv->v.vio_wrap;

		/* Fixup tail of this buffer */
		uv->v.vio_tail = v->vio_tail;
	} else {
		/* We have to allocate and insert a new buffer */
		if (xioq->ioq_uv.uvq_fetch) {
			/** @todo: does this actually work? */
			/* more of the same kind */
			have2 =
				xioq->ioq_uv.uvq_fetch(
					xioq, uv->u.uio_p1,
					"next buffer", 1,
					IOQ_FLAG_NONE);

			/* poolq_entry is the top element of xdr_ioq_uv
			 */
			uv2 = IOQ_(have2);
			assert((void *)uv2 == (void *)have2);
		} else {
			uv2 = xdr_ioq_uv_create(xioq->ioq_uv.min_bsize,
						UIO_FLAG_FREE);
			have2 = &uv2->uvq;
			(xioq->ioq_uv.uvqh.qcount)++;
			TAILQ_INSERT_AFTER(&xioq->ioq_uv.uvqh.qh,
					   have, have2, q);

			/* Advance to new buffer */
			uv = uv2;
			have = have2;
		}

		/* Now set up for the header in the new buffer */
		v->vio_base = uv->v.vio_base;
		v->vio_head = uv->v.vio_head;
		v->vio_tail = uv->v.vio_head + htlen;
		v->vio_wrap = uv->v.vio_wrap;

		/* Fixup tail of this buffer */
		uv->v.vio_tail = v->vio_tail;
	}

	if (v->vio_type == VIO_TRAILER_LEN) {
		/* Now that we have buffer space for the trailer len, we can
		 * peek ahead to the next buffer and get it's length and fill
		 * the length into the buffer. Note that this buffer is not
		 * part of the gss_iov.
		 */
		*((uint32_t *) (v[0].vio_head)) =
					(uint32_t) htonl(v[1].vio_length);
	}

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s Produced xdr_ioq_uv %p (base %p head %p tail %p wrap %p) size %lu length %lu",
		__func__, uv, uv->v.vio_base, uv->v.vio_head,
		uv->v.vio_tail, uv->v.vio_wrap,
		(unsigned long) ioquv_size(uv),
		(unsigned long) ioquv_length(uv));

	return uv;
}

static bool
xdr_ioq_allochdrs(XDR *xdrs, u_int start, xdr_vio *vector, int iov_count)
{
	bool found = false;
	struct xdr_ioq_uv *uv;
	int idx = 0;
	struct xdr_ioq *xioq = XIOQ(xdrs);
	struct poolq_entry *have;
	u_int totlen = start;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	TAILQ_FOREACH(have, &(XIOQ(xdrs)->ioq_uv.uvqh.qh), q) {
		u_int len;

		uv = IOQ_(have);
		len = ioquv_length(uv);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s Examining xdr_ioq_uv %p (base %p head %p tail %p wrap %p) - %s start %lu len %lu",
			__func__, uv, uv->v.vio_base, uv->v.vio_head,
			uv->v.vio_tail, uv->v.vio_wrap,
			found ? "found" : "not found",
			(unsigned long) start, (unsigned long) len);

		if (start < len) {
			/* start is in this buffer, but not at the start.
			 * This should be the first data buffer.
			 */
			found = true;
			break;
		}

		/* Keep looking, need to reduce start by the length of
		 * this buffer.
		 */
		start -= len;

		if (start == 0) {
			/* We have found the buffer prior to the one
			 * that begins at start.
			 */
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s found start after %p",
				__func__, uv);
			found = true;
			break;
		}
	}

	if (!found) {
		/* Failure */
		return false;
	}

	/* uv and have are the buffer just before start */

	if (vector[idx].vio_type == VIO_HEADER) {
		if (start != 0) {
			/* We are leading with a HEADER, but this buffer has
			 * data beyond start, so we can't insert the HEADER in
			 * the right place...
			 */
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"Oops, trying to insert HEADER in the middle of a buffer");
			return false;
		}

		/* We have a header, let's see if it fits in this buffer,
		 * otherwise allocate and insert a new buffer.
		 */
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"Calling xdr_ioq_use_or_allocate for idx %d for VIO_HEADER",
			idx);

		uv = xdr_ioq_use_or_allocate(xioq, &vector[idx], uv);

		/* Record used space */
		totlen += vector[idx].vio_length;

		/* Advance to next (DATA) buffer */
		idx++;
	}

	if (start == 0) {
		/* We have the buffer prior to the DATA buffer that should be
		 * at start, so advance to the next buffer so we will now have
		 * the first DATA buffer.
		 */
		uv = IOQ_(TAILQ_NEXT(&uv->uvq, q));
	}

	/* Now idx, uv, and have should be the first DATA buffer */
	while (idx < iov_count && vector[idx].vio_type == VIO_DATA) {
		/* Advance to next buffer */
		have = TAILQ_NEXT(have, q);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"Skipping idx %d for VIO_DATA",
			idx);

		/* Record used space */
		totlen += vector[idx].vio_length;

		if (have != NULL) {
			/* Next buffer exists */
			uv = IOQ_(have);
		} /* else leave the last DATA buffer */

		idx++;
	}

	/* Now idx, uv, and have are the last DATA buffer */

	while (idx < iov_count) {
		/* Another TRAILER buffer to manage */
		vio_type vt = vector[idx].vio_type;

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"Calling xdr_ioq_use_or_allocate for idx %d for %s",
			idx,
			vt == VIO_HEADER ? "VIO_HEADER"
			: vt == VIO_DATA ? "VIO_DATA"
			: vt == VIO_TRAILER_LEN ? "VIO_TRAILER_LEN"
			: vt == VIO_TRAILER ? "VIO_TRAILER"
			: "UNKNOWN");

		if (vt != VIO_TRAILER && vt != VIO_TRAILER_LEN) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"Oops, buffer other than a trailer found after all data");
			return false;
		}

		if (vt == VIO_TRAILER_LEN &&
		    ((idx + 1) == iov_count ||
		     vector[idx + 1].vio_type != VIO_TRAILER)) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"Oops, VIO_TRAILER_LEN not followed by VIO_TRAILER");
			return false;
		}

		uv = xdr_ioq_use_or_allocate(xioq, &vector[idx], uv);

		/* Record used space */
		totlen += vector[idx].vio_length;

		/* Next vector buffer */
		idx++;
	}

	/* Update position to end of the last buffer */
	XDR_SETPOS(xdrs, totlen);

	return true;
}

const struct xdr_ops xdr_ioq_ops = {
	xdr_ioq_getunit,
	xdr_ioq_putunit,
	xdr_ioq_getbytes,
	xdr_ioq_putbytes,
	xdr_ioq_getpos,
	xdr_ioq_setpos,
	xdr_ioq_destroy_internal,
	xdr_ioq_control,
	xdr_ioq_getbufs,
	xdr_ioq_putbufs,
	xdr_ioq_newbuf,		/* x_newbuf */
	xdr_ioq_iovcount,	/* x_iovcount */
	xdr_ioq_fillbufs,	/* x_fillbufs */
	xdr_ioq_allochdrs,	/* x_allochdrs */
};

#ifdef USE_RPC_RDMA
const struct xdr_ops xdr_ioq_ops_rdma = {
	xdr_ioq_getunit,
	xdr_ioq_putunit,
	xdr_ioq_getbytes_rdma,
	xdr_ioq_putbytes,
	xdr_ioq_getpos,
	xdr_ioq_setpos,
	xdr_ioq_destroy_internal_rdma,
	xdr_ioq_control,
	xdr_ioq_getbufs,
	xdr_ioq_putbufs,
	xdr_ioq_newbuf,		/* x_newbuf */
	xdr_ioq_iovcount,	/* x_iovcount */
	xdr_ioq_fillbufs,	/* x_fillbufs */
	xdr_ioq_allochdrs,	/* x_allochdrs */
};
#endif
