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

/**
 * \file	rpc_rdma.h
 * \brief	rdma helper include file
 *
 * This was (very) loosely based on the Mooshika library, which in turn
 * was a mix of diod, rping (librdmacm/examples), and Linux kernel's
 * net/9p/trans_rdma.c (dual BSD/GPL license). No vestiges remain.
 */

#ifndef _TIRPC_RPC_RDMA_H
#define _TIRPC_RPC_RDMA_H

#include <semaphore.h>
#include <rdma/rdma_cma.h>
#include <rpc/svc.h>
#include <rpc/xdr_ioq.h>
#include <rpc/work_pool.h>

#include "rpc_dplx_internal.h"

/*
 * Number of epoll threads for RDMA completion queue event channel fds.
 * For each RDMA session, a single completion queue even channel is
 * used for both send queue as well as receive queue. Each connection
 * (since only a single fd is used by each connection) is assined to
 * one of the NUM_CQ_EPOLL_THREADS epoll threads.
 *
 * 8 is sufficient to maximized performance for IO bound
 * workload. Will tune this in future based on performance
 * testing.
 */
#define NUM_CQ_EPOLL_THREADS 8

typedef union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in sa_in;
	struct sockaddr_in6 sa_int6;
	struct sockaddr_storage sa_stor;
} sockaddr_union_t;

struct msk_stats {
	uint64_t rx_bytes;
	uint64_t rx_pkt;
	uint64_t rx_err;
	uint64_t tx_bytes;
	uint64_t tx_pkt;
	uint64_t tx_err;
	/* times only set if debug has MSK_DEBUG_SPEED */
	uint64_t nsec_callback;
	uint64_t nsec_compevent;
};

typedef enum rpc_extra_io_buf_type {
	IO_INBUF = 1,	/* Buffers used for recv */
	IO_OUTBUF	/* Buffers used for send */
} rpc_extra_io_buf_type_t;

/* Track buffers allocated on demand */
struct rpc_extra_io_bufs {
        struct ibv_mr *mr;
        uint32_t buffer_total;
        uint8_t *buffer_aligned;
        struct poolq_entry q;
	rpc_extra_io_buf_type_t type;
};

typedef struct rpc_rdma_xprt RDMAXPRT;

struct rpc_rdma_cbc;
typedef int (*rpc_rdma_callback_t)(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt);

#define CBC_FLAG_NONE		0x0000
#define CBC_FLAG_RELEASE	0x0001
#define CBC_FLAG_RELEASING	0x0002

#define RDMA_CB_TIMEOUT_SEC 10

/**
 * \struct rpc_rdma_cbc
 * Context data we can use during recv/send callbacks
 */
struct rpc_rdma_cbc {
	/* recvq should always be first, since we use xdrs to get cbc */
	struct xdr_ioq recvq; /* queue to hold requests and rdma_read bufs */
	struct xdr_ioq sendq; /* queue to hold responses and rdma_write bufs */
	struct xdr_ioq dataq; /* queue to hold data_bufs from protocol */
	struct xdr_ioq freeq; /* queue to hold used bufs */

	struct xdr_ioq_uv *call_uv;
	void *call_head;
	void *read_chunk;	/* current in indexed list of arrays */
	void *write_chunk;	/* current in list of arrays */
	void *reply_chunk;	/* current in array */
	void *call_data;
	bool_t call_inline;
	int32_t refcnt;
	int32_t read_waits;
	uint16_t cbc_flags;
	struct poolq_entry *have;
	struct xdr_ioq_uv *data_chunk_uv;
	struct poolq_entry cbc_list;
	uint8_t *non_registered_buf;
	int non_registered_buf_len;
	pthread_cond_t cb_done;
	pthread_mutex_t cb_done_mutex;

	struct work_pool_entry wpe;
	rpc_rdma_callback_t positive_cb;
	rpc_rdma_callback_t negative_cb;
	void *callback_arg;

	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;

	enum ibv_wc_opcode opcode;
	enum ibv_wc_status status;

	struct ibv_sge sg_list[0];	/**< this is actually an array.
					note that when you malloc
					you have to add its size */
};

struct rpc_rdma_pd {
	LIST_ENTRY(rpc_rdma_pd) pdl;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_srq *srq;
	struct poolq_head srqh;		/**< shared read contexts */

	uint32_t pd_used;
};

#define RDMAX_CLIENT 0
#define RDMAX_SERVER_CHILD -1

#define RDMA_HDR_CHUNK_SZ 8192
#define MAX_CBC_OUTSTANDING(xa) (xa)->credits
#define MAX_CBC_ALLOCATION(xa) (MAX_CBC_OUTSTANDING(xa) * 2)
#define MAX_RECV_OUTSTANDING(xa) MAX_CBC_OUTSTANDING(xa)
#define RDMA_DATA_CHUNKS 32
#define RDMA_DATA_CHUNK_SZ 1048576
#define RDMA_HDR_CHUNKS(xa) MAX_CBC_OUTSTANDING(xa)

/**
 * \struct rpc_rdma_xprt
 * RDMA transport instance
 */
struct rpc_rdma_xprt {
	struct rpc_dplx_rec sm_dr;

	const struct rpc_rdma_attr *xa;	/**< (shared) configured attributes */

	struct rdma_event_channel *event_channel;
	struct rdma_cm_id *cm_id;	/**< RDMA CM ID */
	struct rpc_rdma_pd *pd;		/**< RDMA PD entry */

	struct ibv_comp_channel *comp_channel;
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_srq *srq;		/**< Shared Receive Queue pointer */

	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;

	struct ibv_mr *mr;
	u_int8_t *buffer_aligned;
	size_t buffer_total;

	struct xdr_ioq_uv_head inbufs_hdr;	/* Buffers to hold request headers */
	struct xdr_ioq_uv_head inbufs_data;	/* Buffers to hold request data */
	struct xdr_ioq_uv_head outbufs_hdr;	/* Buffers to hold response headers */
	struct xdr_ioq_uv_head outbufs_data;	/* Buffers to hold response data */

	struct poolq_head cbqh;		/**< combined callback contexts */

	struct poolq_head extra_bufs;

	u_int extra_bufs_count;

	uint32_t active_requests;

	struct poolq_head cbclist;

	mutex_t cm_lock;		/**< lock for connection events */
	cond_t cm_cond;			/**< cond for connection events */

	struct msk_stats stats;
	int stats_sock;

	u_int conn_type;		/**< RDMA Port space (RDMA_PS_TCP) */
	int server;			/**< connection backlog on server,
					 * 0 (RDMAX_CLIENT):
					 * client,
					 * -1 (RDMAX_SERVER_CHILD):
					 * server has accepted connection
					 */

	enum rdma_transport_state {
		RDMAXS_INITIAL, 	/* assumes zero, never set */
		RDMAXS_LISTENING,
		RDMAXS_ADDR_RESOLVED,
		RDMAXS_ROUTE_RESOLVED,
		RDMAXS_CONNECT_REQUEST,
		RDMAXS_CONNECTED,
		RDMAXS_ERROR
	} state;			/**< transport state machine */

	/* FIXME why configurable??? */
	bool destroy_on_disconnect;	/**< should perform cleanup */
};
#define RDMA_DR(p) (opr_containerof((p), struct rpc_rdma_xprt, sm_dr))

struct connection_requests {
	struct rdma_cm_id **id_queue;
	sem_t	q_sem;
	sem_t	u_sem;
	uint32_t q_head;
	uint32_t q_tail;
	u_int	q_size;
};

struct rpc_rdma_state {
	LIST_HEAD(pdh_s, rpc_rdma_pd) pdh;	/**< Protection Domain list */
	mutex_t lock;

	struct connection_requests c_r;		/* never freed??? */

	pthread_t cm_thread_id;		/**< Thread id for connection manager */

	uint16_t cq_thread_count; /* number of active cq epoll threads */

	/* Thread ids for completion queue */
	pthread_t cq_thread_ids[NUM_CQ_EPOLL_THREADS];

	pthread_t stats_thread_id;

	int cm_epollfd;
	int cq_epollfd[NUM_CQ_EPOLL_THREADS];
	int stats_epollfd;

	int32_t run_count;
};

static inline void *xdr_encode_hyper(uint32_t *iptr, uint64_t val)
{
	*iptr++ = htonl((uint32_t)((val >> 32) & 0xffffffff));
	*iptr++ = htonl((uint32_t)(val & 0xffffffff));
	return iptr;
}

static inline uint64_t xdr_decode_hyper(uint64_t *iptr)
{
	return ((uint64_t) ntohl(((uint32_t*)iptr)[0]) << 32)
		| (ntohl(((uint32_t*)iptr)[1]));
}

/* Take ref on cbc before we do ibv_post */
static inline void cbc_ref_it(struct rpc_rdma_cbc *cbc, RDMAXPRT *rdma_xprt)
{
	int32_t refs =
		atomic_inc_int32_t(&cbc->refcnt);

	SVC_REF(&rdma_xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA, "%s: take_ref cbc %p ref %d "
		"refs %d rdma xprt %p",
		__func__, cbc, cbc->refcnt, refs, rdma_xprt);
}

#define x_rdma_xprt(xdrs) ((RDMAXPRT *)((xdrs)->x_lib[1]))

/* Release ref on cbc on callback from ibv_post */
static inline void cbc_release_it(struct rpc_rdma_cbc *cbc)
{
	int32_t refs =
		atomic_dec_int32_t(&cbc->refcnt);
	RDMAXPRT *rdma_xprt = x_rdma_xprt(cbc->recvq.xdrs);

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA, "%s: release_ref cbc %p ref %d "
		"refs %d",
		__func__, cbc, cbc->refcnt, refs);

	if ((refs == 0) && (cbc->cbc_flags & CBC_FLAG_RELEASE)) {
		pthread_mutex_lock(&rdma_xprt->cbclist.qmutex);

		/* we could race here with rdma_cleanup_cbc_tasks
		 * check if buffer is reused */

		if (!(cbc->cbc_flags & CBC_FLAG_RELEASE)) {
			__warnx(TIRPC_DEBUG_FLAG_EVENT,
			    "%s cbc %p reused ref %d flags %x", __func__, cbc,
			    cbc->refcnt, cbc->cbc_flags);

			return;
		}

		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA, "%s: destroy_cbc "
			"cbc %p ref %d flags %x",
			__func__, cbc, cbc->refcnt, cbc->cbc_flags);

		uint16_t flags = atomic_postset_uint16_t_bits(&cbc->cbc_flags,
		    CBC_FLAG_RELEASING);

		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA, "%s: destroy_cbc "
			" destroying cbc %p ref %d cbc_flags %x flags %x",
			__func__, cbc, cbc->refcnt, cbc->cbc_flags, flags);

		TAILQ_REMOVE(&rdma_xprt->cbclist.qh, &cbc->cbc_list, q);
		rdma_xprt->cbclist.qcount--;

		pthread_mutex_unlock(&rdma_xprt->cbclist.qmutex);

		if (cbc->non_registered_buf) {
			mem_free(cbc->non_registered_buf, cbc->non_registered_buf_len);
		}

		/* cbqh is pointed by recvq */
		xdr_rdma_ioq_release(&cbc->sendq.ioq_uv.uvqh, false, &cbc->sendq);
		xdr_rdma_ioq_release(&cbc->dataq.ioq_uv.uvqh, false, &cbc->dataq);
		xdr_rdma_ioq_release(&cbc->freeq.ioq_uv.uvqh, false, &cbc->freeq);
		xdr_rdma_ioq_release(&cbc->recvq.ioq_uv.uvqh, false, &cbc->recvq);

		/* Remove cbc from ioq before we add it back to cbqh */
		pthread_mutex_lock(&rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);
		TAILQ_REMOVE(&rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qh, &cbc->recvq.ioq_s, q);
		(rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qcount)--;
		pthread_mutex_unlock(&rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qmutex);

		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: cbc_track end %p recvq %p %d sendq %p %d "
			"dataq %p %d freeq %p %d ioq %p %d rdma_xprt %p",
			__func__, cbc, &cbc->recvq, cbc->recvq.ioq_uv.uvqh.qcount,
			&cbc->sendq, cbc->sendq.ioq_uv.uvqh.qcount, &cbc->dataq,
			cbc->dataq.ioq_uv.uvqh.qcount, &cbc->freeq,
			cbc->freeq.ioq_uv.uvqh.qcount, &rdma_xprt->sm_dr.ioq,
			rdma_xprt->sm_dr.ioq.ioq_uv.uvqh.qcount, rdma_xprt);

		/* Add cbc back to cbqh */
		xdr_rdma_ioq_release(&cbc->recvq.ioq_uv.uvqh, true, &cbc->recvq);
	}

	SVC_RELEASE(&rdma_xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
}

extern struct rpc_rdma_state rpc_rdma_state;

void rpc_rdma_internals_init(void);
void rpc_rdma_internals_fini(void);

/* server specific */
int rpc_rdma_accept_finalize(RDMAXPRT *);
RDMAXPRT *rpc_rdma_accept_wait(RDMAXPRT *, int);
void rpc_rdma_destroy(RDMAXPRT *);

enum xprt_stat svc_rdma_rendezvous(SVCXPRT *);

/* client */
int rpc_rdma_connect(RDMAXPRT *);
int rpc_rdma_connect_finalize(RDMAXPRT *);

/* XDR functions */
int xdr_rdma_create(RDMAXPRT *);
void xdr_rdma_add_inbufs_data(RDMAXPRT *rdma_xprt);
void xdr_rdma_add_outbufs_data(RDMAXPRT *rdma_xprt);
void xdr_rdma_add_inbufs_hdr(RDMAXPRT *rdma_xprt);
void xdr_rdma_add_outbufs_hdr(RDMAXPRT *rdma_xprt);
void xdr_rdma_callq(RDMAXPRT *);

bool xdr_rdma_clnt_reply(XDR *, u_int32_t);
bool xdr_rdma_clnt_flushout(struct rpc_rdma_cbc *);

bool xdr_rdma_svc_recv(struct rpc_rdma_cbc *, u_int32_t);
bool xdr_rdma_svc_reply(struct rpc_rdma_cbc *, u_int32_t,
    bool rdma_buf_used);
bool xdr_rdma_svc_flushout(struct rpc_rdma_cbc *, bool rdma_buf_used);
void rpc_rdma_allocate_cbc_locked(struct poolq_head *ioqh);

int rpc_rdma_fd_del(int fd, int epollfd);

void svc_rdma_unlink(SVCXPRT *xprt, u_int flags, const char *tag,
    const int line);
void svc_rdma_destroy(SVCXPRT *xprt, u_int flags, const char *tag,
    const int line);

void rdma_cleanup_cbcs_task(struct work_pool_entry *wpe);

void rpc_rdma_close_connection(RDMAXPRT *rdma_xprt);

#endif /* !_TIRPC_RPC_RDMA_H */
