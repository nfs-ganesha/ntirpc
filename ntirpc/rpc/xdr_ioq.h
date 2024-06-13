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
#include <misc/opr.h>
#include <misc/queue.h>
#include <rpc/pool_queue.h>
#include <rpc/work_pool.h>
#include <rpc/xdr.h>

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

#ifdef USE_RPC_RDMA
	bool rdma_ioq;
#endif

	struct rpc_dplx_rec *rec;
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
