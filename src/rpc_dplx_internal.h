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

#include <misc/queue.h>
#include <misc/rbtree.h>
#include <misc/wait_queue.h>
#include <rpc/svc.h>
#include <rpc/xdr_ioq.h>
#include <rpc/pool_queue.h>

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
	struct opr_rbtree call_replies;
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

static inline void
rpc_dplx_rec_init(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_init(&rec->recv.lock);
	opr_rbtree_init(&rec->call_replies, clnt_req_xid_cmpf);
	mutex_init(&rec->xprt.xp_lock, NULL);
	TAILQ_INIT(&rec->writeq.qh);
	mutex_init(&rec->writeq.qmutex, NULL);
	rec->writeq.qcount = 0;
	/* Stop this xprt being cleaned immediately */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &(rec->recv.ts));

	rec->xprt.xp_refcnt = 1;
}

static inline void
rpc_dplx_rec_destroy(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_destroy(&rec->recv.lock);
	mutex_destroy(&rec->xprt.xp_lock);
	mutex_destroy(&rec->writeq.qmutex);

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
