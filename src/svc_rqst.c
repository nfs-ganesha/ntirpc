/*
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

#include "config.h"

#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc_rqst.h>

#include "rpc_com.h"

#include <rpc/svc.h>
#include <misc/rbtree_x.h>
#include <misc/opr_queue.h>
#include <misc/timespec.h>
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include <rpc/svc_auth.h>
#include "svc_ioq.h"

#ifdef USE_RPC_RDMA
#include "rpc_rdma.h"
#endif

/**
 * @file svc_rqst.c
 * @contributeur William Allen Simpson <bill@cohortfs.com>
 * @brief Multi-channel event signal package
 *
 * @section DESCRIPTION
 *
 * Maintains a list of all extant transports by event (channel) id.
 *
 * Each SVCXPRT points to its own handler, however, so operations to
 * block/unblock events (for example) given an existing xprt handle
 * are O(1) without any ordered or hashed representation.
 */

#define SVC_RQST_TIMEOUT_MS (29 /* seconds (prime) was 120 */ * 1000)
#define SVC_RQST_WAKEUPS (1023)

/* > RPC_DPLX_LOCKED > SVC_XPRT_FLAG_LOCKED */
#define SVC_RQST_LOCKED		0x01000000
#define SVC_RQST_UNLOCK		0x02000000

static uint32_t round_robin;
/*static*/ uint32_t wakeups;

struct svc_rqst_rec {
	struct work_pool_entry ev_wpe;
	struct opr_rbtree call_expires;
	mutex_t ev_lock;

	int sv[2];
	uint32_t id_k;		/* chan id */

	/*
	 * union of event processor types
	 */
	enum svc_event_type ev_type;
	union {
#if defined(TIRPC_EPOLL)
		struct {
			int epoll_fd;
			struct epoll_event ctrl_ev;
			struct epoll_event *events;
			u_int max_events;	/* max epoll events */
			bool sv1_added;
		} epoll;
#endif
		struct {
			fd_set set;	/* select/fd_set (currently unhooked) */
		} fd;
	} ev_u;

	int32_t ev_refcnt;
	uint16_t ev_flags;
	struct xdr_ioq *xioq; /* IOQ for floating sr_rec */
};

void svc_rqst_rec_init(struct svc_rqst_rec *sr_rec)
{
	/* Pre-initialize stuff that needs to be non-zero */
	mutex_init(&sr_rec->ev_lock, NULL);
	sr_rec->sv[0] = -1;
	sr_rec->sv[1] = -1;
	sr_rec->id_k = UINT32_MAX;
#if defined(TIRPC_EPOLL)
	sr_rec->ev_u.epoll.epoll_fd = -1;
#endif
}

void svc_rqst_rec_destroy(struct svc_rqst_rec *sr_rec)
{
#if defined(TIRPC_EPOLL)
	if (sr_rec->ev_u.epoll.sv1_added) {
		int code;

		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_DEL,
				 sr_rec->sv[1], &sr_rec->ev_u.epoll.ctrl_ev);
		if (code) {
			code = errno;
			__warnx(TIRPC_DEBUG_FLAG_WARN,
				"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
				" epoll_fd %d control fd pair (%d:%d) unhook failed (%d)",
				__func__,
				sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
				sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1], code);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
				TIRPC_DEBUG_FLAG_REFCNT,
				"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
				" epoll_fd %d control fd pair (%d:%d) unhook event %p",
				__func__,
				sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
				sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1], &sr_rec->ev_u.epoll.ctrl_ev);
		}
	}
#endif

	if (sr_rec->sv[0] >= 0) {
		close(sr_rec->sv[0]);
		sr_rec->sv[0] = -1;
	}

	if (sr_rec->sv[1] >= 0) {
		close(sr_rec->sv[1]);
		sr_rec->sv[1] = -1;
	}

#if defined(TIRPC_EPOLL)
	if (sr_rec->ev_u.epoll.epoll_fd > 0) {
		close(sr_rec->ev_u.epoll.epoll_fd);
		sr_rec->ev_u.epoll.epoll_fd = -1;
	}
#endif
}

struct svc_rqst_set {
	mutex_t mtx;
	struct svc_rqst_rec *srr;
	uint32_t max_id;
	uint32_t next_id;
};

static struct svc_rqst_set svc_rqst_set = {
	MUTEX_INITIALIZER,
	NULL,
	0,
	0,
};

/*
 * Write 4-byte value to shared event-notification channel.  The
 * value as presently implemented can be interpreted only by one consumer,
 * so is not relied on.
 */
static inline void
ev_sig(int fd, uint32_t sig)
{
	int code = write(fd, &sig, sizeof(uint32_t));

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST, "%s: fd %d sig %d", __func__, fd,
		sig);
	if (code < 1)
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: error writing to event socket [%d:%d]", __func__,
			code, errno);
}

/*
 * Read a single 4-byte value from the shared event-notification channel,
 * the socket is in non-blocking mode.  The value read is returned.
 */
static inline uint32_t
consume_ev_sig_nb(int fd)
{
	uint32_t sig = 0;
	int code __attribute__ ((unused));

	code = read(fd, &sig, sizeof(uint32_t));
	return (sig);
}

static inline void
SetNonBlock(int fd)
{
	int s_flags = fcntl(fd, F_GETFL, 0);
	(void)fcntl(fd, F_SETFL, (s_flags | O_NONBLOCK));
}

void
svc_rqst_init(uint32_t channels)
{
	int i;

	mutex_lock(&svc_rqst_set.mtx);

	if (svc_rqst_set.srr)
		goto unlock;

	svc_rqst_set.max_id = channels;
	svc_rqst_set.next_id = channels;
	svc_rqst_set.srr = mem_zalloc(channels * sizeof(struct svc_rqst_rec));

	for (i = 0; i < channels; i++) {
		svc_rqst_rec_init(&svc_rqst_set.srr[i]);
	}

 unlock:
	mutex_unlock(&svc_rqst_set.mtx);
}

/**
 * @brief Lookup a channel
 */
static inline struct svc_rqst_rec *
svc_rqst_lookup_chan(uint32_t chan_id)
{
	struct svc_rqst_rec *sr_rec;

	if (chan_id >= svc_rqst_set.max_id)
		return (NULL);

	sr_rec = &svc_rqst_set.srr[chan_id];
	if (atomic_fetch_int32_t(&sr_rec->ev_refcnt) <= 0)
		return (NULL);

	/* do not pre-increment to avoid accidental new channel */
	atomic_inc_int32_t(&sr_rec->ev_refcnt);
	return (sr_rec);
}

/* forward declaration in lieu of moving code {WAS} */
static void svc_rqst_epoll_loop(struct work_pool_entry *wpe);
static void svc_complete_task(struct svc_rqst_rec *sr_rec, bool finished);

static int
svc_rqst_expire_cmpf(const struct opr_rbtree_node *lhs,
		     const struct opr_rbtree_node *rhs)
{
	struct clnt_req *lk, *rk;

	lk = opr_containerof(lhs, struct clnt_req, cc_rqst);
	rk = opr_containerof(rhs, struct clnt_req, cc_rqst);

	if (lk->cc_expire_ms < rk->cc_expire_ms)
		return (-1);

	if (lk->cc_expire_ms == rk->cc_expire_ms) {
		return (0);
	}

	return (1);
}

static inline int
svc_rqst_expire_ms(struct timespec *to)
{
	struct timespec ts;

	/* coarse nsec, not system time */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
	timespecadd(&ts, to, &ts);
	return timespec_ms(&ts);
}

void
svc_rqst_expire_insert(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);
	struct svc_rqst_rec *sr_rec = cx->cx_rec->ev_p;
	struct opr_rbtree_node *nv;

	cc->cc_expire_ms = svc_rqst_expire_ms(&cc->cc_timeout);

	mutex_lock(&sr_rec->ev_lock);
	cc->cc_flags = CLNT_REQ_FLAG_EXPIRING;
 repeat:
	nv = opr_rbtree_insert(&sr_rec->call_expires, &cc->cc_rqst);
	if (nv) {
		/* add this slightly later */
		cc->cc_expire_ms++;
		goto repeat;
	}
	mutex_unlock(&sr_rec->ev_lock);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: sv[0] fd %d before ev_sig (sr_rec %p)",
		__func__, sr_rec->sv[0],
		sr_rec);
	ev_sig(sr_rec->sv[0], 0);	/* send wakeup */
}

void
svc_rqst_expire_remove(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);
	struct svc_rqst_rec *sr_rec = cx->cx_rec->ev_p;

	mutex_lock(&sr_rec->ev_lock);
	opr_rbtree_remove(&sr_rec->call_expires, &cc->cc_rqst);
	mutex_unlock(&sr_rec->ev_lock);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: sv[0] fd %d before ev_sig (sr_rec %p)",
		__func__, sr_rec->sv[0],
		sr_rec);
	ev_sig(sr_rec->sv[0], 0);	/* send wakeup */
}

static void
svc_rqst_expire_task(struct work_pool_entry *wpe)
{
	struct clnt_req *cc = opr_containerof(wpe, struct clnt_req, cc_wpe);

	if (atomic_fetch_int32_t(&cc->cc_refcnt) > 1
	 && !(atomic_postset_uint16_t_bits(&cc->cc_flags,
					   CLNT_REQ_FLAG_BACKSYNC)
	      & (CLNT_REQ_FLAG_ACKSYNC | CLNT_REQ_FLAG_BACKSYNC))) {
		/* (idempotent) cc_flags and cc_refcnt are set atomic.
		 * cc_refcnt need more than 1 (this task).
		 */
		cc->cc_error.re_status = RPC_TIMEDOUT;
		(*cc->cc_process_cb)(cc);
	}

	clnt_req_release(cc);
}

static inline void
svc_rqst_release(struct svc_rqst_rec *sr_rec)
{
	if (atomic_dec_int32_t(&sr_rec->ev_refcnt) > 0)
		return;

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: remove evchan %d control fd pair (%d:%d)",
		__func__, sr_rec->id_k,
		sr_rec->sv[0], sr_rec->sv[1]);

	svc_rqst_rec_destroy(sr_rec);
}

int
svc_rqst_new_evchan(uint32_t *chan_id /* OUT */, void *u_data, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;
	uint32_t n_id;
	int code = 0, i;
	work_pool_fun_t fun = NULL;
	int32_t ref_rec;

	mutex_lock(&svc_rqst_set.mtx);
	if (!svc_rqst_set.next_id) {
		/* too many new channels, re-use global default, may be zero */
		*chan_id =
		svc_rqst_set.next_id = __svc_params->ev_u.evchan.id;
		mutex_unlock(&svc_rqst_set.mtx);
		return (0);
	}
	n_id = --(svc_rqst_set.next_id);

	sr_rec = &svc_rqst_set.srr[n_id];

	ref_rec = atomic_postinc_int32_t(&sr_rec->ev_refcnt);

	if (ref_rec > 0) {
		/* already exists */
		*chan_id = n_id;
		mutex_unlock(&svc_rqst_set.mtx);
		return (0);
	}

	/* Track the references we have */
	ref_rec++;

	flags |= SVC_RQST_FLAG_EPOLL;	/* XXX */

	/* create a pair of anonymous sockets for async event channel wakeups */
	code = socketpair(AF_UNIX, SOCK_STREAM, 0, sr_rec->sv);
	if (code) {
		code = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: failed creating event signal socketpair (%d) for sr_rec",
			__func__, code);
		goto fail;
	}

	/* set non-blocking */
	SetNonBlock(sr_rec->sv[0]);
	SetNonBlock(sr_rec->sv[1]);

#if defined(TIRPC_EPOLL)
	if (flags & SVC_RQST_FLAG_EPOLL) {
		sr_rec->ev_type = SVC_EVENT_EPOLL;
		fun = svc_rqst_epoll_loop;

		/* XXX improve this too */
		sr_rec->ev_u.epoll.max_events =
		    __svc_params->ev_u.evchan.max_events;
		sr_rec->ev_u.epoll.events = (struct epoll_event *)
		    mem_alloc(sr_rec->ev_u.epoll.max_events *
			      sizeof(struct epoll_event));

		/* create epoll fd */
		sr_rec->ev_u.epoll.epoll_fd =
		    epoll_create_wr(sr_rec->ev_u.epoll.max_events,
				    EPOLL_CLOEXEC);

		if (sr_rec->ev_u.epoll.epoll_fd == -1) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: epoll_create failed (%d)", __func__,
				errno);
			mem_free(sr_rec->ev_u.epoll.events,
				 sr_rec->ev_u.epoll.max_events *
				 sizeof(struct epoll_event));
			code = EINVAL;
			goto fail;
		}

		/* permit wakeup of threads blocked in epoll_wait, with a
		 * couple of possible semantics */
		sr_rec->ev_u.epoll.ctrl_ev.events = EPOLLIN | EPOLLRDHUP;
		sr_rec->ev_u.epoll.ctrl_ev.data.fd = sr_rec->sv[1];
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_ADD,
				 sr_rec->sv[1], &sr_rec->ev_u.epoll.ctrl_ev);
		if (code == -1) {
			code = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: add control socket failed (%d)", __func__,
				code);
			goto fail;
		}

		sr_rec->ev_u.epoll.sv1_added = true;

		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST | TIRPC_DEBUG_FLAG_REFCNT,
			"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
			" epoll_fd %d EPOLL_CTL_ADD code %d event %p",
			__func__,
			sr_rec, sr_rec->id_k, ref_rec,
			sr_rec->ev_u.epoll.epoll_fd, code,
			&sr_rec->ev_u.epoll.ctrl_ev);
	} else {
		/* legacy fdset (currently unhooked) */
		sr_rec->ev_type = SVC_EVENT_FDSET;
	}
#else
	sr_rec->ev_type = SVC_EVENT_FDSET;
#endif

	*chan_id = n_id;

	sr_rec->id_k = n_id;
	sr_rec->ev_flags = flags & SVC_RQST_FLAG_MASK;
	opr_rbtree_init(&sr_rec->call_expires, svc_rqst_expire_cmpf);
	atomic_inc_int32_t(&sr_rec->ev_refcnt);
	ref_rec++;
	sr_rec->ev_wpe.fun = fun;
	sr_rec->ev_wpe.arg = u_data;
	work_pool_submit(&svc_work_pool, &sr_rec->ev_wpe);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: create evchan %d control fd pair (%d:%d)",
		__func__, n_id,
		sr_rec->sv[0], sr_rec->sv[1]);

	if (code != 0) {
fail:
		/* Release this event channel id */
		++(svc_rqst_set.next_id);

		/* Release sr_rec */
		for (i = 0; i < ref_rec; i++)
			svc_rqst_release(sr_rec);
	}

	mutex_unlock(&svc_rqst_set.mtx);

	return (code);
}

/*
 * may be RPC_DPLX_LOCKED, and SVC_XPRT_FLAG_ADDED cleared
 */
static inline int
svc_rqst_unhook_events(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec,
		       uint16_t ev_flags)
{
	int code = EINVAL;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, unhook, __func__, __LINE__, &rec->xprt, ev_flags);
#endif /* USE_LTTNG_NTIRPC */
	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: xprt %p fd %d ev_flags%s%s%s%s%s%s%s%s%s",
		__func__, &rec->xprt, rec->xprt.xp_fd,
		ev_flags & SVC_XPRT_FLAG_ADDED_RECV  ? " ADDED_RECV" : "",
		ev_flags & SVC_XPRT_FLAG_ADDED_SEND  ? " ADDED_SEND" : "",
		ev_flags & SVC_XPRT_FLAG_INITIAL     ? " INITIAL" : "",
		ev_flags & SVC_XPRT_FLAG_INITIALIZED ? " INITIALIZED" : "",
		ev_flags & SVC_XPRT_FLAG_CLOSE       ? " CLOSE" : "",
		ev_flags & SVC_XPRT_FLAG_DESTROYING  ? " DESTROYING" : "",
		ev_flags & SVC_XPRT_FLAG_RELEASING   ? " RELEASING" : "",
		ev_flags & SVC_XPRT_FLAG_UREG        ? " UREG" : "",
		sr_rec->ev_flags & SVC_RQST_FLAG_SHUTDOWN
					? "sr_rec->ev_flags SHUTDOWN" : "");

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev;

		if (ev_flags & SVC_XPRT_FLAG_ADDED_RECV) {
			ev = &rec->ev_u.epoll.event_recv;

			/* clear epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_DEL, rec->xprt.xp_fd, ev);

			if (code) {
				code = errno;
				__warnx(TIRPC_DEBUG_FLAG_WARN,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) unhook failed (%d)",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) unhook event %p",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);

				atomic_clear_uint16_t_bits(
						&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_ADDED_RECV);
			}
		}

		if (ev_flags & SVC_XPRT_FLAG_ADDED_SEND) {
			ev = &rec->ev_u.epoll.event_send;

			/* clear epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_DEL, rec->xprt.xp_fd_send, ev);

			if (code) {
				code = errno;
				__warnx(TIRPC_DEBUG_FLAG_WARN,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) unhook failed (%d)",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) unhook event %p",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);

				atomic_clear_uint16_t_bits(
						&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_ADDED_SEND);
				close(rec->xprt.xp_fd_send);
				rec->xprt.xp_fd_send = -1;
			}
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	return (code);
}

/*
 * rpc_dplx_rec lock must be held
 */
int
svc_rqst_rearm_events_locked(SVCXPRT *xprt, uint16_t ev_flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec = rec->ev_p;
	int code = EINVAL;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, rearm, __func__, __LINE__, xprt, ev_flags);
#endif /* USE_LTTNG_NTIRPC */

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: xprt %p fd %d ev_flags%s%s%s%s%s%s%s%s%s",
		__func__, xprt, xprt->xp_fd,
		ev_flags & SVC_XPRT_FLAG_ADDED_RECV  ? " ADDED_RECV" : "",
		ev_flags & SVC_XPRT_FLAG_ADDED_SEND  ? " ADDED_SEND" : "",
		ev_flags & SVC_XPRT_FLAG_INITIAL     ? " INITIAL" : "",
		ev_flags & SVC_XPRT_FLAG_INITIALIZED ? " INITIALIZED" : "",
		ev_flags & SVC_XPRT_FLAG_CLOSE       ? " CLOSE" : "",
		ev_flags & SVC_XPRT_FLAG_DESTROYING  ? " DESTROYING" : "",
		ev_flags & SVC_XPRT_FLAG_RELEASING   ? " RELEASING" : "",
		ev_flags & SVC_XPRT_FLAG_UREG        ? " UREG" : "",
		sr_rec->ev_flags & SVC_RQST_FLAG_SHUTDOWN
					? "sr_rec->ev_flags SHUTDOWN" : "");

	if (xprt->xp_flags & (ev_flags | SVC_XPRT_FLAG_DESTROYED))
		return (0);

	/* MUST follow the destroyed check above */
	if (sr_rec->ev_flags & SVC_RQST_FLAG_SHUTDOWN)
		return (0);

	/* Don't take a ref on the xprt.  We take a ref in hook, and release it
	 * in unhook. */

	/* assuming success */
	atomic_set_uint16_t_bits(&xprt->xp_flags, ev_flags);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev;

		if (ev_flags & SVC_XPRT_FLAG_ADDED_RECV) {
			ev = &rec->ev_u.epoll.event_recv;

			/* set up epoll user data */
			ev->events = EPOLLIN | EPOLLONESHOT;

			/* rearm in epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
					 EPOLL_CTL_MOD, rec->xprt.xp_fd, ev);
			if (code) {
				code = errno;
				atomic_clear_uint16_t_bits(
						&xprt->xp_flags,
						SVC_XPRT_FLAG_ADDED_RECV);
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) rearm failed (%d)",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
				SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %"PRId32
					" epoll_fd %d control fd pair (%d:%d) rearm event %p",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);
			}
		}

		if (ev_flags & SVC_XPRT_FLAG_ADDED_SEND) {
			ev = &rec->ev_u.epoll.event_send;

			/* wait for write events, edge triggered, oneshot */
			ev->events = EPOLLONESHOT | EPOLLOUT | EPOLLET;

			/* rearm in epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
					 EPOLL_CTL_MOD, rec->xprt.xp_fd_send,
					 ev);

			if (code) {
				code = errno;
				atomic_clear_uint16_t_bits(
						&xprt->xp_flags,
						SVC_XPRT_FLAG_ADDED_SEND);
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: %p fd_send %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) rearm failed (%d)",
					__func__, rec, rec->xprt.xp_fd_send,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
				SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd_send %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %"PRId32
					" epoll_fd %d control fd pair (%d:%d) rearm event %p",
					__func__, rec, rec->xprt.xp_fd_send,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);
			}
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	return (code);
}

/*
 * RPC_DPLX_LOCKED, and SVC_XPRT_FLAG_ADDED set
 */
static inline int
svc_rqst_hook_events(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec,
		     uint16_t ev_flags)
{
	int code = EINVAL;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, hook, __func__, __LINE__, &rec->xprt, ev_flags);
#endif /* USE_LTTNG_NTIRPC */

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: xprt %p fd %d ev_flags%s%s%s%s%s%s%s%s%s",
		__func__, &rec->xprt, rec->xprt.xp_fd,
		ev_flags & SVC_XPRT_FLAG_ADDED_RECV  ? " ADDED_RECV" : "",
		ev_flags & SVC_XPRT_FLAG_ADDED_SEND  ? " ADDED_SEND" : "",
		ev_flags & SVC_XPRT_FLAG_INITIAL     ? " INITIAL" : "",
		ev_flags & SVC_XPRT_FLAG_INITIALIZED ? " INITIALIZED" : "",
		ev_flags & SVC_XPRT_FLAG_CLOSE       ? " CLOSE" : "",
		ev_flags & SVC_XPRT_FLAG_DESTROYING  ? " DESTROYING" : "",
		ev_flags & SVC_XPRT_FLAG_RELEASING   ? " RELEASING" : "",
		ev_flags & SVC_XPRT_FLAG_UREG        ? " UREG" : "",
		sr_rec->ev_flags & SVC_RQST_FLAG_SHUTDOWN
					? "sr_rec->ev_flags SHUTDOWN" : "");

	/* assuming success */
	atomic_set_uint16_t_bits(&rec->xprt.xp_flags, ev_flags);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev;

		/* For epoll, we no longer need a ref on the xprt.  epoll uses
		 * the FD as a key now, and the xprt is looked up, which gets a
		 * ref for the event.  The xprt can therefore be freed while in
		 * epoll, with no consequences. */

		if (ev_flags & SVC_XPRT_FLAG_ADDED_RECV) {
			ev = &rec->ev_u.epoll.event_recv;

			/* set up epoll user data */
			ev->data.fd = rec->xprt.xp_fd;

			/* wait for read events, level triggered, oneshot */
			ev->events = EPOLLONESHOT | EPOLLIN;

			/* add to epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
					 EPOLL_CTL_ADD, rec->xprt.xp_fd, ev);
			if (code) {
				code = errno;
				atomic_clear_uint16_t_bits(
						&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_ADDED_RECV);
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) direction in hook failed (%d)",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) direction in hook event %p",
					__func__, rec, rec->xprt.xp_fd,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);
			}
		}

		if (ev_flags & SVC_XPRT_FLAG_ADDED_SEND) {
			ev = &rec->ev_u.epoll.event_send;

			/* set up epoll user data.  Lookup needs the primary FD */
			ev->data.fd = rec->xprt.xp_fd;

			/* wait for write events, edge triggered, oneshot */
			ev->events = EPOLLONESHOT | EPOLLOUT | EPOLLET;

			/* add to epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
					 EPOLL_CTL_ADD, rec->xprt.xp_fd_send,
					 ev);
			if (code) {
				code = errno;
				atomic_clear_uint16_t_bits(
						&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_ADDED_SEND);
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: %p fd_send %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) direction out hook failed (%d)",
					__func__, rec, rec->xprt.xp_fd_send,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], code);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
					TIRPC_DEBUG_FLAG_REFCNT,
					"%s: %p fd_send %d xp_refcnt %" PRId32
					" sr_rec %p evchan %d ev_refcnt %" PRId32
					" epoll_fd %d control fd pair (%d:%d) direction out hook event %p",
					__func__, rec, rec->xprt.xp_fd_send,
					rec->xprt.xp_refcnt,
					sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
					sr_rec->ev_u.epoll.epoll_fd,
					sr_rec->sv[0], sr_rec->sv[1], ev);
			}
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: sv[0] fd %d before ev_sig (sr_rec %p)",
		__func__, sr_rec->sv[0],
		sr_rec);
	ev_sig(sr_rec->sv[0], 0);	/* send wakeup */

	return (code);
}

void
svc_rqst_unhook(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec = (struct svc_rqst_rec *)rec->ev_p;
	uint16_t xp_flags =
		atomic_postclear_uint16_t_bits(&xprt->xp_flags,
					       SVC_XPRT_FLAG_ADDED_RECV |
					       SVC_XPRT_FLAG_ADDED_SEND);

	/* clear events */
	if (xp_flags & (SVC_XPRT_FLAG_ADDED_RECV | SVC_XPRT_FLAG_ADDED_SEND))
		(void)svc_rqst_unhook_events(rec, sr_rec, xp_flags);
}

/*
 * RPC_DPLX_LOCKED
 */
static void
svc_rqst_unreg(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec)
{
	svc_rqst_unhook(&rec->xprt);

	/* Unlinking after debug message ensures both the xprt and the sr_rec
	 * are still present, as the xprt unregisters before release.
	 */
	if (rec->ev_p == sr_rec) {
		rec->ev_p = NULL;
		svc_rqst_release(sr_rec);
	}
}

void svc_rqst_xprt_send_complete(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec;

	sr_rec = rec->ev_p;

	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p has no attached sr_rec",
			__func__, xprt);
		return;
	}

	(void)svc_rqst_unhook_events(rec, sr_rec, SVC_XPRT_FLAG_ADDED_SEND);
}

int
svc_rqst_evchan_write(SVCXPRT *xprt, struct xdr_ioq *xioq, bool has_blocked)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec;
	int code;

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: xprt %p xioq %p has_blocked %s",
		__func__, xprt, xioq, has_blocked ? "TRUE" : "FALSE");

	sr_rec = rec->ev_p;

	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p has no attached sr_rec",
			__func__, xprt);
		return (ENOENT);
	}

	rec->ev_u.epoll.xioq_send = xioq;

#if defined(TIRPC_EPOLL)
	if (sr_rec->ev_type == SVC_EVENT_EPOLL) {
		/* For send we need to dup the xprt fd */
		if (xprt->xp_fd_send == -1) {
			xprt->xp_fd_send = dup(xprt->xp_fd);

			if (xprt->xp_fd_send< 0) {
				code = errno;
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: failed duplicating fd (%d)",
					__func__, code);
				return (code);
			}

			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: xp_fd_send %d dup of xp_fd %d",
				__func__, xprt->xp_fd_send, xprt->xp_fd);
		}
	}
#endif

	rpc_dplx_rli(rec);

	/* register on event channel */
	if (has_blocked) {
		code = svc_rqst_rearm_events_locked(xprt,
						    SVC_XPRT_FLAG_ADDED_SEND);
	} else {
		code = svc_rqst_hook_events(rec, sr_rec,
					    SVC_XPRT_FLAG_ADDED_SEND);
	}

	if (unlikely(code)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: failed evchan write control (%d)",
			__func__, code);
	} else {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: create evchan write control fd pair (%d:%d)",
			__func__,
			sr_rec->sv[0], sr_rec->sv[1]);
	}

	rpc_dplx_rui(rec);

	return (code);
}

/*
 * flags indicate locking state
 */
int
svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec;
	int code;
	uint16_t bits = SVC_XPRT_FLAG_ADDED_RECV | (flags & SVC_XPRT_FLAG_UREG);

	if (chan_id == 0) {
		/* Create a global/legacy event channel */
		code = svc_rqst_new_evchan(&(__svc_params->ev_u.evchan.id),
					   NULL /* u_data */ ,
					   SVC_RQST_FLAG_CHAN_AFFINITY);
		if (code) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p failed to create global/legacy channel (%d)",
				__func__, xprt, code);
			return (code);
		}
		chan_id = __svc_params->ev_u.evchan.id;
	}

	sr_rec = svc_rqst_lookup_chan(chan_id);

	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p unknown evchan %d",
			__func__, xprt, chan_id);
		return (ENOENT);
	}

	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rli(rec);

	if (rec->ev_p) {
		if (rec->ev_p == sr_rec) {
			if (!(flags & RPC_DPLX_LOCKED))
				rpc_dplx_rui(rec);
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p already registered evchan %d",
				__func__, xprt, chan_id);
			return (0);
		}
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: %p unregistering evchan %d",
			__func__, xprt, rec->ev_p->id_k);
		svc_rqst_unreg(rec, rec->ev_p);
	}

	/* link from xprt */
	rec->ev_p = sr_rec;

	/* register sr_rec on event channel */
	code = svc_rqst_hook_events(rec, sr_rec, bits);

	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rui(rec);

	return (code);
}

/*
 * not locked
 */
int
svc_rqst_xprt_register(SVCXPRT *newxprt, SVCXPRT *xprt)
{
	struct svc_rqst_rec *sr_rec;

	/* if no parent xprt, use global/legacy event channel */
	if (!xprt)
		return svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id,
					   newxprt,
					   SVC_RQST_FLAG_CHAN_AFFINITY);

	sr_rec = REC_XPRT(xprt)->ev_p;

	/* or if parent xprt has no dedicated event channel */
	if (!sr_rec)
		return svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id,
					   newxprt,
					   SVC_RQST_FLAG_CHAN_AFFINITY);

	/* if round robin policy, begin with global/legacy event channel */
	if (!(sr_rec->ev_flags & SVC_RQST_FLAG_CHAN_AFFINITY)) {
		int code = svc_rqst_evchan_reg(round_robin, newxprt,
					       SVC_RQST_FLAG_NONE);

		if (!code) {
			/* advance round robin channel */
			code = svc_rqst_new_evchan(&round_robin, NULL,
						   SVC_RQST_FLAG_NONE);
		}
		return (code);
	}

	return svc_rqst_evchan_reg(sr_rec->id_k, newxprt, SVC_RQST_FLAG_NONE);
}

/*
 * flags indicate locking state
 *
 * @note Locking
 *	Called via svc_release_it() with once-only semantic.
 */
void
svc_rqst_xprt_unregister(SVCXPRT *xprt, uint32_t flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec = (struct svc_rqst_rec *)rec->ev_p;

	/* Remove from the transport list here (and only here)
	 * before clearing the registration to ensure other
	 * lookups cannot re-use this transport.
	 */
	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rli(rec);

	svc_xprt_clear(xprt);

	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rui(rec);

	if (!sr_rec) {
		/* not currently registered */
		return;
	}
	svc_rqst_unreg(rec, sr_rec);
}

#if defined(USE_RPC_RDMA)

/*
 * flags indicate locking state
 *
 * @note Locking
 *	Called via svc_release_it() with once-only semantic.
 */
void
svc_rqst_xprt_unregister_rdma(SVCXPRT *xprt, uint32_t flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec = (struct svc_rqst_rec *)rec->ev_p;

	assert(!sr_rec);

	/* Remove from the transport list here (and only here)
	 * before clearing the registration to ensure other
	 * lookups cannot re-use this transport.
	 */
	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rli(rec);

	svc_xprt_clear(xprt);

	if (!(flags & RPC_DPLX_LOCKED))
		rpc_dplx_rui(rec);
}

#endif

/*static*/ void
svc_rqst_xprt_task_recv(struct work_pool_entry *wpe)
{
	struct xdr_ioq *ioq =
			opr_containerof(wpe, struct xdr_ioq, ioq_wpe);
	struct rpc_dplx_rec *rec = ioq->rec;

	atomic_clear_uint16_t_bits(&ioq->ioq_s.qflags, IOQ_FLAG_WORKING);

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, recv, __func__, __LINE__,
		   &rec->xprt,
		   (unsigned int)(rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED),
		   (unsigned int) rec->xprt.xp_refcnt);
#endif /* USE_LTTNG_NTIRPC */

	/* atomic barrier (above) should protect following values */
	if (rec->xprt.xp_refcnt > 1
	    && !(rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
		/* (idempotent) xp_flags and xp_refcnt are set atomic.
		 * xp_refcnt need more than 1 (this task).
		 */
		(void)clock_gettime(CLOCK_MONOTONIC_FAST, &rec->recv.ts);
		(void)SVC_RECV(&rec->xprt);
	}

	/* Release the ref taken on the event */
	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
}

enum xprt_stat svc_request(SVCXPRT *xprt, XDR *xdrs)
{
	enum xprt_stat stat;
	struct svc_req *req = __svc_params->alloc_cb(xprt, xdrs);
	struct rpc_dplx_rec *rpc_dplx_rec = REC_XPRT(xprt);

	/* Track the request we are processing */
	rpc_dplx_rec->svc_req = req;

	/* All decode functions basically do a
	 * return xprt->xp_dispatch.process_cb(req);
	 */
	stat = SVC_DECODE(req);

	if (stat == XPRT_SUSPEND) {
		/* The rquest is suspended, don't touch the request in any way
		 * because the resume may already be scheduled and running on
		 * another thread.
		 */
		return XPRT_SUSPEND;
	}

	if (req->rq_auth)
		SVCAUTH_RELEASE(req);

	XDR_DESTROY(req->rq_xdrs);

	__svc_params->free_cb(req, stat);

	return stat;
}

static void svc_resume_task(struct work_pool_entry *wpe)
{
	struct svc_req *req =
			opr_containerof(wpe, struct svc_req, rq_wpe);
	enum xprt_stat stat;

	/* Resume the request. */
	stat  = req->rq_resume_cb(req);

	if (stat == XPRT_SUSPEND) {
		/* The rquest is suspended, don't touch the request in any way
		 * because the resume may already be scheduled and running on
		 * another thread.
		 */
		return;
	}

	if (req->rq_auth)
		SVCAUTH_RELEASE(req);

	XDR_DESTROY(req->rq_xdrs);

	__svc_params->free_cb(req, stat);
}

void svc_resume(struct svc_req *req)
{
	req->rq_wpe.fun = svc_resume_task;
	work_pool_submit(&svc_work_pool, &req->rq_wpe);
}

/*static*/ void
svc_rqst_xprt_task_send(struct work_pool_entry *wpe)
{
	struct xdr_ioq *ioq =
			opr_containerof(wpe, struct xdr_ioq, ioq_wpe);
	struct rpc_dplx_rec *rec = ioq->rec;

	atomic_clear_uint16_t_bits(&ioq->ioq_s.qflags, IOQ_FLAG_WORKING);

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, send, __func__, __LINE__,
		   &rec->xprt,
		   (unsigned int)(rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED),
		   (unsigned int) rec->xprt.xp_refcnt);
#endif /* USE_LTTNG_NTIRPC */

	/* atomic barrier (above) should protect following values */
	if (rec->xprt.xp_refcnt > 1
	    && !(rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
		/* (idempotent) xp_flags and xp_refcnt are set atomic.
		 * xp_refcnt need more than 1 (this task).
		 */
		svc_ioq_write(&rec->xprt);
	}

	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
}

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */

struct svc_rqst_clean_arg {
	struct timespec ts;
	int timeout;
	int cleaned;
};

static bool
svc_rqst_clean_func(SVCXPRT *xprt, void *arg)
{
	struct svc_rqst_clean_arg *acc = (struct svc_rqst_clean_arg *)arg;

	if (xprt->xp_ops == NULL)
		return (false);

	if (xprt->xp_flags & (SVC_XPRT_FLAG_DESTROYED | SVC_XPRT_FLAG_UREG))
		return (false);

	if ((acc->ts.tv_sec - REC_XPRT(xprt)->recv.ts.tv_sec) < acc->timeout)
		return (false);

	SVC_DESTROY(xprt);
	acc->cleaned++;
	return (true);
}

void authgss_ctx_gc_idle(void);

static void
svc_rqst_clean_idle(int timeout)
{
	struct svc_rqst_clean_arg acc;
	static mutex_t active_mtx = MUTEX_INITIALIZER;
	static uint32_t active;

	if (mutex_trylock(&active_mtx) != 0)
		return;

	if (active > 0)
		goto unlock;

	++active;

#ifdef _HAVE_GSSAPI
	/* trim gss context cache */
	authgss_ctx_gc_idle();
#endif /* _HAVE_GSSAPI */

	if (timeout <= 0)
		goto unlock;

	/* trim xprts (not sorted, not aggressive [but self limiting]) */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &acc.ts);
	acc.timeout = timeout;
	acc.cleaned = 0;

	svc_xprt_foreach(svc_rqst_clean_func, (void *)&acc);

 unlock:
	--active;
	mutex_unlock(&active_mtx);
	return;
}

#ifdef TIRPC_EPOLL

static struct xdr_ioq *
svc_rqst_epoll_event(struct svc_rqst_rec *sr_rec, struct epoll_event *ev)
{
	SVCXPRT *xprt;
	struct rpc_dplx_rec *rec;
	uint16_t xp_flags, ev_flag = 0;
	struct xdr_ioq *ioq = NULL;
	work_pool_fun_t fun;

	if (unlikely(ev->data.fd == sr_rec->sv[1])) {
		/* signalled -- there was a wakeup on ctrl_ev (see
		 * top-of-loop) */
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: fd %d wakeup (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
		(void)consume_ev_sig_nb(sr_rec->sv[1]);
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: fd %d after consume sig (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
		return (NULL);
	}

	xprt = svc_xprt_lookup(ev->data.fd, NULL);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: fd %d no associated xprt",
			__func__, ev->data.fd);
		return (NULL);
	}
	/* At this point, we have a ref on the xprt, and know it's valid */
	rec = REC_XPRT(xprt);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: event %p %08x%s%s rpc_dplx_rec %p (sr_rec %p)",
		__func__, ev, ev->events,
		ev->events & EPOLLIN ? " RECV" : "",
		ev->events & EPOLLOUT ? " SEND" : "",
		rec, sr_rec);

	if (ev->events & EPOLLIN) {
		/* This is a RECV event */
		ev_flag = SVC_XPRT_FLAG_ADDED_RECV;
		ioq = &rec->ioq;
		fun = svc_rqst_xprt_task_recv;
	} else if (ev->events & EPOLLOUT) {
		/* This is a SEND event */
		ev_flag = SVC_XPRT_FLAG_ADDED_SEND;
		ioq = rec->ev_u.epoll.xioq_send;
		fun = svc_rqst_xprt_task_send;
	} else {
		/* This is some other event... */
		SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
		return NULL;
	}

	/* MUST handle flags after reference.
	 * Although another task may unhook, the error is non-fatal.
	 */
	xp_flags = atomic_postclear_uint16_t_bits(&rec->xprt.xp_flags, ev_flag);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
		TIRPC_DEBUG_FLAG_REFCNT,
		"%s: %p fd %d xp_refcnt %" PRId32
		" event %08x xp_flags%s%s clear flag%s%s",
		__func__, rec, rec->xprt.xp_fd, rec->xprt.xp_refcnt,
		ev->events,
		xp_flags & SVC_XPRT_FLAG_ADDED_RECV ? " ADDED_RECV" : "",
		xp_flags & SVC_XPRT_FLAG_ADDED_SEND ? " ADDED_SEND" : "",
		ev_flag & SVC_XPRT_FLAG_ADDED_RECV ? " ADDED_RECV" : "",
		ev_flag & SVC_XPRT_FLAG_ADDED_SEND ? " ADDED_SEND" : "");

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, event, __func__, __LINE__, &rec->xprt, xp_flags,
		   ev_flag);
#endif /* USE_LTTNG_NTIRPC */

	if (rec->xprt.xp_refcnt > 1
	    && (xp_flags & ev_flag)
	    && !(xp_flags & SVC_XPRT_FLAG_DESTROYED)
	    && !(atomic_postset_uint16_t_bits(&ioq->ioq_s.qflags,
					      IOQ_FLAG_WORKING)
			& IOQ_FLAG_WORKING)) {
		/* (idempotent) xp_flags and xp_refcnt are set atomic.
		 * xp_refcnt need more than 1 (this event).
		 */
		ioq->ioq_wpe.fun = fun;
		ioq->rec = rec;
		return ioq;
	}

	/* Do not return destroyed transports.
	 * Probably log non-fatal "WARNING! already destroying!"
	 */
	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
	return (NULL);
}

/*
 * not locked
 */
static inline struct xdr_ioq *
svc_rqst_epoll_events(struct svc_rqst_rec *sr_rec, int n_events)
{
	struct xdr_ioq *ioq = NULL;
	int ix = 0;

	/* Find the first RECV or SEND event */
	while (ix < n_events) {
		ioq = svc_rqst_epoll_event(sr_rec,
					   &sr_rec->ev_u.epoll.events[ix++]);
		if (ioq)
			break;
	}

	if (!ioq) {
		/* continue waiting for events with this task */
		return NULL;
	}

	while (ix < n_events) {
		/* Queue up additional RECV or SEND events */
		struct xdr_ioq *ioq = svc_rqst_epoll_event(sr_rec,
					    &(sr_rec->ev_u.epoll.events[ix++]));
		if (ioq)
			work_pool_submit(&svc_work_pool, &ioq->ioq_wpe);
	}

	/* submit another task to handle events in order */
	atomic_inc_int32_t(&sr_rec->ev_refcnt);
	work_pool_submit(&svc_work_pool, &sr_rec->ev_wpe);

	return ioq;
}

static void svc_rqst_epoll_loop(struct work_pool_entry *wpe)
{
	struct svc_rqst_rec *sr_rec = 
		opr_containerof(wpe, struct svc_rqst_rec, ev_wpe);
	struct clnt_req *cc;
	struct opr_rbtree_node *n;
	struct timespec ts;
	int timeout_ms;
	int expire_ms;
	int n_events;
	bool finished;

	for (;;) {
		timeout_ms = SVC_RQST_TIMEOUT_MS;

		/* coarse nsec, not system time */
		(void)clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
		expire_ms = timespec_ms(&ts);

		/* before epoll_wait will accumulate events during scan */
		mutex_lock(&sr_rec->ev_lock);
		while ((n = opr_rbtree_first(&sr_rec->call_expires))) {
			cc = opr_containerof(n, struct clnt_req, cc_rqst);

			if (cc->cc_expire_ms > expire_ms) {
				timeout_ms = cc->cc_expire_ms - expire_ms;
				break;
			}

			/* order dependent */
			atomic_clear_uint16_t_bits(&cc->cc_flags,
						   CLNT_REQ_FLAG_EXPIRING);
			opr_rbtree_remove(&sr_rec->call_expires, &cc->cc_rqst);
			cc->cc_expire_ms = 0;	/* atomic barrier(s) */

			atomic_inc_uint32_t(&cc->cc_refcnt);
			cc->cc_wpe.fun = svc_rqst_expire_task;
			cc->cc_wpe.arg = NULL;
			work_pool_submit(&svc_work_pool, &cc->cc_wpe);
		}
		mutex_unlock(&sr_rec->ev_lock);

		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: epoll_fd %d before epoll_wait (%d)",
			__func__,
			sr_rec->ev_u.epoll.epoll_fd,
			timeout_ms);

		n_events = epoll_wait(sr_rec->ev_u.epoll.epoll_fd,
				      sr_rec->ev_u.epoll.events,
				      sr_rec->ev_u.epoll.max_events,
				      timeout_ms);

		if (unlikely(sr_rec->ev_flags & SVC_RQST_FLAG_SHUTDOWN)) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: epoll_fd %d epoll_wait shutdown (%d)",
				__func__,
				sr_rec->ev_u.epoll.epoll_fd,
				n_events);
			finished = true;
			break;
		}
		if (n_events > 0) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
				TIRPC_DEBUG_FLAG_REFCNT,
				"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
				" epoll_fd %d n_events %d",
				__func__,
				sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
				sr_rec->ev_u.epoll.epoll_fd, n_events);

			atomic_add_uint32_t(&wakeups, n_events);
			struct xdr_ioq *ioq;

			ioq = svc_rqst_epoll_events(sr_rec, n_events);

			if (ioq != NULL) {
				/* use this hot thread for the first event */
				ioq->ioq_wpe.fun(&ioq->ioq_wpe);

				/* failsafe idle processing after work task */
				if (atomic_postclear_uint32_t_bits(
					&wakeups, ~SVC_RQST_WAKEUPS)
				    > SVC_RQST_WAKEUPS) {
					svc_rqst_clean_idle(
						__svc_params->idle_timeout);
				}
				finished = false;
				break;
			}
			continue;
		}
		if (!n_events) {
			/* timed out (idle) */
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
				TIRPC_DEBUG_FLAG_REFCNT,
				"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
				" epoll_fd %d idle",
				__func__,
				sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
				sr_rec->ev_u.epoll.epoll_fd);
			atomic_inc_uint32_t(&wakeups);
			continue;
		}
		n_events = errno;
		if (n_events != EINTR) {
			__warnx(TIRPC_DEBUG_FLAG_WARN,
				"%s: epoll_fd %d epoll_wait failed (%d)",
				__func__,
				sr_rec->ev_u.epoll.epoll_fd,
				n_events);
			finished = true;
			break;
		}
	}
	if (finished) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST |
			TIRPC_DEBUG_FLAG_REFCNT,
			"%s: sr_rec %p evchan %d ev_refcnt %" PRId32
			" epoll_fd %d finished",
			__func__,
			sr_rec, sr_rec->id_k, sr_rec->ev_refcnt,
			sr_rec->ev_u.epoll.epoll_fd);

		close(sr_rec->ev_u.epoll.epoll_fd);
		mem_free(sr_rec->ev_u.epoll.events,
			 sr_rec->ev_u.epoll.max_events *
			 sizeof(struct epoll_event));
	}

	svc_complete_task(sr_rec, finished);
}
#endif

static void svc_complete_task(struct svc_rqst_rec *sr_rec, bool finished)
{
	if (finished) {
		/* reference count here should be 2:
		 *	1	svc_rqst_set
		 *	+1	this work_pool thread
		 * so, DROP one here so the final release will go to 0.
		 */
		atomic_dec_int32_t(&sr_rec->ev_refcnt);	/* svc_rqst_set */
	}
	svc_rqst_release(sr_rec);
}

int
svc_rqst_thrd_signal(uint32_t chan_id, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;

	sr_rec = svc_rqst_lookup_chan(chan_id);
	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: unknown evchan %d",
			__func__, chan_id);
		return (ENOENT);
	}

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: sv[0] fd %d before ev_sig (sr_rec %p) evchan %d",
		__func__, sr_rec->sv[0],
		sr_rec, chan_id);
	ev_sig(sr_rec->sv[0], flags);	/* send wakeup */

	svc_rqst_release(sr_rec);
	return (0);
}

static int
svc_rqst_delete_evchan(uint32_t chan_id)
{
	struct svc_rqst_rec *sr_rec;
	int code = 0;

	sr_rec = svc_rqst_lookup_chan(chan_id);
	if (!sr_rec) {
		return (ENOENT);
	}
	atomic_set_uint16_t_bits(&sr_rec->ev_flags, SVC_RQST_FLAG_SHUTDOWN);
	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: sv[0] fd %d before ev_sig (sr_rec %p)",
		__func__, sr_rec->sv[0],
		sr_rec);
	ev_sig(sr_rec->sv[0], SVC_RQST_FLAG_SHUTDOWN);

	svc_rqst_release(sr_rec);
	return (code);
}

void
svc_rqst_shutdown(void)
{
	uint32_t channels = svc_rqst_set.max_id;

	while (channels > 0) {
		svc_rqst_delete_evchan(--channels);
	}
}
