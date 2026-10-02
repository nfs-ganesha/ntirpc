/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
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

#ifndef RPC_DPLX_INTERNAL_H
#define RPC_DPLX_INTERNAL_H

#include <string.h>
#include <misc/queue.h>
#include <misc/rbtree.h>
#include <misc/wait_queue.h>
#include <rpc/svc.h>
#include <rpc/xdr_ioq.h>
#include <rpc/pool_queue.h>
#include <rpc/haproxy.h>
#include "rpc_com.h"

/* Svc event strategy */
enum svc_event_type {
	SVC_EVENT_FDSET /* trad. using select and poll (currently unhooked) */ ,
	SVC_EVENT_EPOLL		/* Linux epoll interface */
};

typedef struct rpc_dplx_lock {
	struct waitq_entry we;
	struct {
		const char *func;
		int line;
	} locktrace;
} rpc_dplx_lock_t;

struct svc_rqst_rec;

/* new unified state */
struct rpc_dplx_rec {
	struct svc_xprt xprt;		/**< Transport Independent handle */
	struct xdr_ioq ioq;
	struct poolq_head writeq;	/**< poolq for write requests */
	/*
	 * xioqs whose bytes are in the kernel but still pinned for
	 * MSG_ZEROCOPY. zc_next_cookie matches the per-socket counter the
	 * kernel assigns on each successful MSG_ZEROCOPY sendmsg.
	 *
	 * Completions may arrive out of order (retransmission, or a send that
	 * fell back to copy completing ahead of true ZC sends).  zc_acked is
	 * the watermark: every cookie below it is confirmed unpinned, and it
	 * is the *only* evidence svc_ioq_zc_xioq_done() has that the kernel
	 * has let go of an xioq's pages.  It must therefore never be advanced
	 * across a cookie we have not seen confirmed — doing so frees pages
	 * still sitting in the TCP retransmit queue.
	 *
	 * Completions that arrive ahead of the watermark are recorded in
	 * zc_ooo_bits, a bitmap of the SVC_ZC_ACK_WINDOW cookies starting at
	 * zc_acked (bit k == cookie zc_acked + k).  It slides as zc_acked
	 * advances, absorbs arbitrary reordering within the window, and
	 * cannot fragment the way a fixed array of [lo,hi] ranges did.
	 *
	 * zcq uses a plain TAILQ rather than poolq_head because the
	 * poolq_head mutex and qsize fields are not needed here.
	 * zc_lock protects zcq and associated zero-copy tracking fields
	 * to isolate zero-copy churning from writeq.qmutex.
	 */
	mutex_t zc_lock;		/* protects ZC queues & tracking fields */
	TAILQ_HEAD(, poolq_entry) zcq;	/**< deferred ZC xioqs */
	uint32_t zc_inflight;		/* xioqs on zcq waiting for kernel ACK; atomic */
	uint32_t zc_next_cookie;	/* next cookie to assign on ZC sendmsg */
	uint32_t zc_acked;		/* all cookies < this are completed */
#define SVC_ZC_ACK_WINDOW 256	/* cookies trackable ahead of zc_acked */
#define SVC_ZC_ACK_WORDS (SVC_ZC_ACK_WINDOW / 64)
	uint64_t zc_ooo_bits[SVC_ZC_ACK_WORDS];
	uint32_t zc_window_drops;	/* completions lost past the window */
	bool zc_sock_ok;		/* SO_ZEROCOPY succeeded on this fd */
	struct opr_rbtree call_replies;
	struct opr_rbtree rdma_call_expires;	/**< call expiration tree for RDMA */
	struct opr_rbtree_node fd_node;
	struct {
		rpc_dplx_lock_t lock;
		struct timespec ts;
	} recv;

	/*
	 * union of event processor types
	 */
	union {
#if defined(TIRPC_EPOLL)
		struct {
			struct epoll_event event_recv;
			struct epoll_event event_send;
			struct xdr_ioq *xioq_send;
		} epoll;
#endif
	} ev_u;
	struct svc_rqst_rec *ev_p;	/* struct svc_rqst_rec (internal) */

	size_t maxrec;
	long pagesz;
#ifdef USE_RPC_RDMA
	u_int recv_hdr_sz;
	u_int send_hdr_sz;
#endif
	u_int recvsz;
	u_int sendsz;
	uint32_t call_xid;		/**< current call xid */
	uint32_t ev_count;		/**< atomic count of waiting events */
	struct svc_req *svc_req;	/**< svc_req we are processing */
};
#define REC_XPRT(p) (opr_containerof((p), struct rpc_dplx_rec, xprt))

/* > SVC_XPRT_FLAG_LOCKED */
#define RPC_DPLX_LOCKED		0x00100000
#define RPC_DPLX_UNLOCK		0x00200000

/* in clnt_generic.c */
enum xprt_stat clnt_req_process_reply(SVCXPRT *, struct svc_req *);
int clnt_req_xid_cmpf(const struct opr_rbtree_node *lhs,
		      const struct opr_rbtree_node *rhs);

static inline void
rpc_dplx_lock_init(struct rpc_dplx_lock *lock)
{
	mutex_init(&lock->we.mtx, NULL);
	cond_init(&lock->we.cv, 0, NULL);
}

static inline void
rpc_dplx_lock_destroy(struct rpc_dplx_lock *lock)
{
	mutex_destroy(&lock->we.mtx);
	cond_destroy(&lock->we.cv);
}

/* Forward declaration for call_expires comparison function */
int svc_rqst_expire_cmpf(const struct opr_rbtree_node *lhs,
			 const struct opr_rbtree_node *rhs);

static inline void
rpc_dplx_rec_init(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_init(&rec->recv.lock);
	opr_rbtree_init(&rec->call_replies, clnt_req_xid_cmpf);
	opr_rbtree_init(&rec->rdma_call_expires, svc_rqst_expire_cmpf);
	mutex_init(&rec->xprt.xp_lock, NULL);
	TAILQ_INIT(&rec->writeq.qh);
	mutex_init(&rec->writeq.qmutex, NULL);
	rec->writeq.qcount = 0;
	mutex_init(&rec->zc_lock, NULL);
	TAILQ_INIT(&rec->zcq);
	rec->zc_inflight = 0;
	rec->zc_next_cookie = 0;
	rec->zc_acked = 0;
	memset(rec->zc_ooo_bits, 0, sizeof(rec->zc_ooo_bits));
	rec->zc_window_drops = 0;
	rec->zc_sock_ok = false;
	/* Stop this xprt being cleaned immediately */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &(rec->recv.ts));

	rec->xprt.xp_refcnt = 1;
	rec->call_xid = __RPC_GETXID(&(rec->recv.ts));

	// Init TLV headers, network id values in case they are not set later on
	rec->xprt.proxy_protocol_tlv_headers.tlv_count = 0;
	rec->xprt.proxy_protocol_tlv_headers.tlvs = NULL;
	rec->xprt.xp_remote_network_id.source = PP2_TYPE_UNSET;
	rec->xprt.xp_remote_network_id.gcp_psc_connection_id = 0;
}

static inline void
rpc_dplx_rec_destroy(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_destroy(&rec->recv.lock);
	mutex_destroy(&rec->xprt.xp_lock);
	mutex_destroy(&rec->writeq.qmutex);
	mutex_destroy(&rec->zc_lock);

	if (rec->xprt.proxy_protocol_tlv_headers.tlv_count > 0) {
		for (uint16_t i = 0; i < rec->xprt.proxy_protocol_tlv_headers.tlv_count; i++)
			free(rec->xprt.proxy_protocol_tlv_headers.tlvs[i].value);
		free(rec->xprt.proxy_protocol_tlv_headers.tlvs);
		rec->xprt.proxy_protocol_tlv_headers.tlvs = NULL;
		rec->xprt.proxy_protocol_tlv_headers.tlv_count = 0;
	}

#if defined(HAVE_BLKIN)
	if (rec->xprt.blkin.svc_name)
		mem_free(rec->xprt.blkin.svc_name, 2*INET6_ADDRSTRLEN);
#endif
}

/* rlt: recv lock trace */
static inline void
rpc_dplx_rlt(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	if (__debug_flag(TIRPC_DEBUG_FLAG_LOCK)) {
		if (lk->locktrace.line) {
			__warnx(TIRPC_DEBUG_FLAG_LOCK,
				"%s:%d locking @%s:%d",
				func, line,
				lk->locktrace.func,
				lk->locktrace.line);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_LOCK,
				"%s:%d locking",
				func, line);
		}
	}
	mutex_lock(&lk->we.mtx);
	lk->locktrace.func = (char *)func;
	lk->locktrace.line = line;
}

/* rli: recv lock impl */
#define rpc_dplx_rli(rec) \
	rpc_dplx_rlt(rec, __func__, __LINE__)

/* rui: recv unlock trace */
static inline void
rpc_dplx_rut(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	__warnx(TIRPC_DEBUG_FLAG_LOCK,
		"%s:%d unlocking @%s:%d",
		func, line,
		lk->locktrace.func,
		lk->locktrace.line);
	lk->locktrace.line = 0;
	mutex_unlock(&lk->we.mtx);
}

/* rli: recv lock impl */
#define rpc_dplx_rui(rec) \
	rpc_dplx_rut(rec, __func__, __LINE__)

/* rwi:  recv wait trace */
static inline void
rpc_dplx_rwt(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	__warnx(TIRPC_DEBUG_FLAG_LOCK,
		"%s:%d waiting @%s:%d",
		func, line,
		lk->locktrace.func,
		lk->locktrace.line);
	lk->locktrace.line = 0;
	cond_wait(&lk->we.cv, &lk->we.mtx);
	lk->locktrace.func = (char *)func;
	lk->locktrace.line = line;
}

/* rwi:  recv wait impl */
#define rpc_dplx_rwi(rec) \
	rpc_dplx_rwt(rec, __func__, __LINE__)

/* rsi: recv signal impl */
static inline void
rpc_dplx_rsi(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	cond_signal(&lk->we.cv);
}

#endif				/* RPC_DPLX_INTERNAL_H */
