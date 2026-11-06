/*
 * Copyright (c) 2013 Linux Box Corporation.
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

#ifndef XDR_IOQ_H
#define XDR_IOQ_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/uio.h>
#include <misc/opr.h>
#include <misc/queue.h>
#include <rpc/pool_queue.h>
#include <rpc/work_pool.h>
#include <rpc/xdr.h>

/**
 * Upper-layer allocator for WRITE payload pages.
 *
 * @param ctx       xioq->payload_alloc_ctx
 * @param data_len  exact payload byte count (no XDR pad)
 * @param iov_out   set to an iovec array owned with @p cookie_out
 * @param iovcnt_out
 * @param cookie_out opaque (e.g. ceph_bufferlist *); released via @p release_out
 * @param release_out called as release(cookie) if fill fails after alloc, or by
 *                    the FSAL when it declines ownership
 * @return true on success
 */
typedef bool (*xdr_payload_alloc_fn)(void *ctx, u_int data_len,
				     struct iovec **iov_out, int *iovcnt_out,
				     void **cookie_out,
				     void (**release_out)(void *cookie));

#ifdef USE_RPC_RDMA

typedef enum rpc_io_buf_type {
	IO_INBUF_DATA = 1,	/* Data Buffers used for recv */
	IO_OUTBUF_DATA,		/* Data Buffers used for send */
	IO_INBUF_HDR,		/* Hdr Buffers used for recv */
	IO_OUTBUF_HDR,		/* Hdr Buffers used for send */
	IO_BUF_ALL		/* Buffers use for all above */
} rpc_io_buf_type_t;

/* Track buffers allocated on demand */
struct rpc_io_bufs {
	struct ibv_mr *mr;
	uint32_t buffer_total;
	uint8_t *buffer_aligned;
	struct poolq_entry q;
	uint32_t refs;
	uint64_t buf_count;
	void *ctx;
	bool_t ready;
	rpc_io_buf_type_t type;
};

typedef enum xdr_ioq_uv_type {
	UV_DATA = 1,	/* Data buffers of big size */
	UV_HDR		/* Header buffers of smaller size */
} xdr_ioq_uv_type_t;

#endif

struct xdr_ioq_uv
{
	struct poolq_entry uvq;

#ifdef USE_RPC_RDMA
	bool rdma_uv; /* Flag to identify uv used for RDMA */
	struct xdr_vio rdma_v; /* Used to reset v after UIO_REFER */
	struct xdr_uio rdma_u; /* Used to reset u after UIO_REFER */
	xdr_ioq_uv_type_t uv_type;
#endif

	/* spliced buffers, if any */
	struct xdr_uio u;

	/* Each xdr_ioq_uv can have a different kind of buffer or data source,
	 * as indicated by the uio_flags, needing different release techniques.
	 * Note: overloads uio_release with uio_p1 for pool.
	 */
	struct xdr_vio v;	/* immediately follows u (uio_vio[0]) */
};

#define IOQ_(p) (opr_containerof((p), struct xdr_ioq_uv, uvq))
#define IOQU(p) (opr_containerof((p), struct xdr_ioq_uv, u))
#define IOQV(p) (opr_containerof((p), struct xdr_ioq_uv, v))

#define ioquv_length(uv) \
	((uintptr_t)((uv)->v.vio_tail) - (uintptr_t)((uv)->v.vio_head))
#define ioquv_more(uv) \
	((uintptr_t)((uv)->v.vio_wrap) - (uintptr_t)((uv)->v.vio_tail))
#define ioquv_size(uv) \
	((uintptr_t)((uv)->v.vio_wrap) - (uintptr_t)((uv)->v.vio_base))

struct xdr_ioq;

struct xdr_ioq_uv_head {
	struct poolq_head uvqh;

	/* Each xdr_ioq_uv can have a different kind of buffer or data source,
	 * as indicated by the uio_flags, needing different create techniques.
	 */
	struct poolq_entry *(*uvq_fetch)(struct xdr_ioq *xioq,
					 struct poolq_head *ioqh,
					 char *comment, u_int count,
					 u_int ioq_flags);

	size_t min_bsize;	/* multiple of pagesize */
	size_t max_bsize;	/* multiple of min_bsize */
	size_t plength;		/* sub-total of previous lengths, not including
				 * any length in this xdr_ioq_uv */
	u_int pcount;		/* fill index (0..m) in the current stream */
};

struct xdr_ioq {
	XDR xdrs[1];
	struct work_pool_entry ioq_wpe;
	struct poolq_entry ioq_s;	/* segment of stream */
	pthread_cond_t ioq_cond;
	struct poolq_head *ioq_pool;
	struct xdr_ioq_uv_head ioq_uv;	/* header/vectors */

	uint64_t id;
	uint32_t write_start; /* Position to start write at */
	int frag_hdr_bytes_sent; /* Indicates a fragment header has been sent */
	bool has_blocked;
	/*
	 * MSG_ZEROCOPY: kernel pins iov pages until errqueue completion.
	 * zc_frag_header must not be stack memory when MSG_ZEROCOPY is set.
	 * zc_outstanding counts successful ZC sendmsgs still awaiting notice.
	 */
	uint32_t zc_frag_header;
	uint32_t zc_cookie_lo;
	uint32_t zc_outstanding;
	uint32_t zc_bytes;	/* payload bytes covered by outstanding ZC sends */

	/*
	 * Reply timing cookie copied from svc_req before async send.
	 * Do not retain pointers to svc_req / nfs_request (freed earlier).
	 */
	struct timespec ioq_rq_stime;
	struct timespec ioq_reply_time;
	int32_t ioq_stats_opcode;
	bool ioq_stats_valid;
	/* Fine-grained IO stage stamps (ns); 0 = unset */
	uint64_t stage_t00_ns;
	uint64_t stage_t01_ns;
	uint64_t stage_t02_ns;
	/* Copied from svc_req at send for T11 (reply flush) delta */
	uint64_t ioq_stage_t10_ns;

#ifdef USE_RPC_RDMA
	bool rdma_ioq;
#endif

	struct rpc_dplx_rec *rec;

	/**
	 * Optional upper-layer WRITE payload allocator.
	 * When set, xdr_ioq_fill_payload() asks this callback for pages,
	 * copies any already-buffered body, and readvs deferred socket
	 * remainder into those pages.
	 */
	xdr_payload_alloc_fn payload_alloc;
	void *payload_alloc_ctx;
};

#define _IOQ(p) (opr_containerof((p), struct xdr_ioq, ioq_s))
#define XIOQ(p) (opr_containerof((p), struct xdr_ioq, xdrs))

/* avoid conflicts with UIO_FLAG */
#define IOQ_FLAG_NONE		0x0000
/* ioq_s.qflags */
#define IOQ_FLAG_SEGMENT	0x0100
#define IOQ_FLAG_WORKING	0x0200	/* (atomic) using ioq_wpe */
/* uint32_t instructions */
#define IOQ_FLAG_LOCKED		0x00010000
#define IOQ_FLAG_UNLOCK		0x00020000
#define IOQ_FLAG_BALLOC		0x00040000

extern struct xdr_ioq_uv *xdr_ioq_uv_create(size_t size, u_int uio_flags);
extern struct poolq_entry *xdr_ioq_uv_fetch(struct xdr_ioq *xioq,
					     struct poolq_head *ioqh,
					     char *comment,
					     u_int count,
					     u_int ioq_flags);
extern struct poolq_entry *xdr_ioq_uv_fetch_nothing(struct xdr_ioq *xioq,
						     struct poolq_head *ioqh,
						     char *comment,
						     u_int count,
						     u_int ioq_flags);
extern void xdr_ioq_uv_release(struct xdr_ioq_uv *uv);

/**
 * Opaque cookie for recv-buffer donate → claim_iov.
 *
 * After a successful @c xdr_ioq_donate_iovs, XDR_DESTROY will not free the
 * underlying UV payload. Pass the cookie as @c priv to the claim deleter
 * (@c xdr_ioq_donate_deleter). Cancel only if claim/submit never takes the
 * buffers (before async write consumes them).
 */
struct xdr_donate_cookie;

/**
 * Mark ioq UVs that back @p iov as @c UIO_FLAG_DONATED (clear FREE).
 *
 * @return cookie for @c xdr_ioq_donate_deleter, or NULL if @p xdrs is not an
 *         ioq stream or any iov cannot be mapped to a FREE UV (nothing changed).
 */
struct xdr_donate_cookie *xdr_ioq_donate_iovs(XDR *xdrs,
					      const struct iovec *iov,
					      int iovcnt);

/** Restore FREE on donated UVs and free @p cookie. No-op if @p cookie is NULL. */
void xdr_ioq_donate_cancel(struct xdr_donate_cookie *cookie);

/**
 * Thread-safe claim deleter: maps slice → UV allocation base and frees the
 * base when the last donated slice of that allocation is released. Frees the
 * cookie when every segment has been released.
 */
void xdr_ioq_donate_deleter(void *buf, size_t len, void *priv);

/**
 * Register upper-layer WRITE payload allocator on this decode stream.
 * Typically set by Ganesha before COMPOUND/WRITE decode when the export
 * wants allocate_own_write_buffer.
 */
void xdr_ioq_set_payload_alloc(XDR *xdrs, xdr_payload_alloc_fn fn, void *ctx);

/** @return true if @c xdr_ioq_set_payload_alloc installed a callback. */
bool xdr_ioq_has_payload_alloc(XDR *xdrs);

/**
 * Fill WRITE payload into upper-layer pages (Phase 1).
 *
 * Calls @c payload_alloc for @p data_len bytes, copies any body already
 * buffered at @p start, then readvs any VC-deferred remainder from the
 * socket. Appends @c UIO_FLAG_UPPER UVs for deferred bytes so SETPOS past
 * RNDUP(@p data_len) succeeds, then pulls any trailing deferred bytes
 * (compound ops after WRITE) into a FREE UV for continued decode. On
 * success, @p iov_out / @p cookie_out / @p release_out are owned by the
 * caller (do not free_buffer the iov bases). Call @p release_out(@p cookie_out)
 * if ownership is not transferred to the FSAL write path.
 *
 * @return false if no callback, alloc failure, or I/O error — when a
 *         payload_alloc callback is registered, callers must not fall back
 *         to XDR_FILLBUFS (deferred body is not visible to IOVCOUNT).
 */
bool xdr_ioq_fill_payload(XDR *xdrs, u_int start, u_int data_len,
			  struct iovec **iov_out, int *iovcnt_out,
			  void **cookie_out,
			  void (**release_out)(void *cookie));

extern struct xdr_ioq *xdr_ioq_create(size_t min_bsize, size_t max_bsize,
				      u_int uio_flags);
extern void xdr_ioq_release(struct poolq_head *ioqh);
extern void xdr_ioq_reset(struct xdr_ioq *xioq, u_int wh_pos);
extern void xdr_ioq_setup(struct xdr_ioq *xioq);

extern void xdr_ioq_destroy(struct xdr_ioq *xioq, size_t qsize);
extern void xdr_ioq_destroy_pool(struct poolq_head *ioqh);

extern const struct xdr_ops xdr_ioq_ops;

#ifdef USE_RPC_RDMA
extern const struct xdr_ops xdr_ioq_ops_rdma;
extern void xdr_rdma_ioq_uv_release(struct xdr_ioq_uv *uv);
extern void xdr_rdma_ioq_release(struct poolq_head *ioqh, bool xioq_recycle,
    struct xdr_ioq *xioq);
extern void xdr_rdma_buf_pool_destroy(struct poolq_head *ioqh,
    struct rpc_io_bufs *io_buf);
extern void xdr_rdma_buf_pool_destroy_locked(struct poolq_head *ioqh,
    struct rpc_io_bufs *io_buf);

extern struct poolq_entry *xdr_rdma_ioq_uv_fetch(struct xdr_ioq *xioq,
						 struct poolq_head *ioqh,
						 char *comment,
						 u_int count,
						 u_int ioq_flags);
extern struct poolq_entry *xdr_rdma_ioq_uv_fetch_nothing(struct xdr_ioq *xioq,
							 struct poolq_head *ioqh,
							 char *comment,
							 u_int count,
							 u_int ioq_flags);
#endif
#endif				/* XDR_IOQ_H */
