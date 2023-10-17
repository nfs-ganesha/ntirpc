/*
 * Copyright (c) 2012-2014 CEA
 * contributeur : Dominique Martinet <dominique.martinet@cea.fr>
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
 * \file    rpc_rdma.c
 * \brief   rdma helper
 *
 * This was (very) loosely based on the Mooshika library, which in turn
 * was a mix of diod, rping (librdmacm/examples), and Linux kernel's
 * net/9p/trans_rdma.c (dual BSD/GPL license). No vestiges remain.
 */

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>	//printf
#include <limits.h>	//INT_MAX
#include <sys/socket.h> //sockaddr
#include <sys/un.h>     //sockaddr_un
#include <pthread.h>	//pthread_* (think it's included by another one)
#include <semaphore.h>  //sem_* (is it a good idea to mix sem and pthread_cond/mutex?)
#include <arpa/inet.h>  //inet_ntop
#include <netinet/in.h> //sock_addr_in
#include <unistd.h>	//fcntl
#include <fcntl.h>	//fcntl
#include <sys/epoll.h>
#include <urcu-bp.h>

#define EPOLL_SIZE (10)
/*^ expected number of fd, must be > 0 */
#define EPOLL_EVENTS (16)
/*^ maximum number of events per poll */
#define EPOLL_WAIT_MS (1000)
/*^ ms check for rpc_rdma_state.run_count (was 100) */
#define IBV_POLL_EVENTS (16)
/*^ maximum number of events per poll */
#define NSEC_IN_SEC (1000*1000*1000)

#include "misc/portable.h"
#include <rdma/rdma_cma.h>
#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/xdr_ioq.h>
#include <rpc/rpc.h>

#include "misc/abstract_atomic.h"
#include "rpc_rdma.h"
#include "svc_internal.h"

#ifdef HAVE_VALGRIND_MEMCHECK_H
#  include <valgrind/memcheck.h>
#  ifndef VALGRIND_MAKE_MEM_DEFINED
#    warning "Valgrind support requested, but VALGRIND_MAKE_MEM_DEFINED not available"
#  endif
#endif
#ifndef VALGRIND_MAKE_MEM_DEFINED
#  define VALGRIND_MAKE_MEM_DEFINED(addr, len)
#endif

/** defaults **/
#define WORKER_STACK_SIZE (65535) /* was 2116488 */

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

	pthread_t cm_thread;		/**< Thread id for connection manager */
	pthread_t cq_thread;		/**< Thread id for completion queue */
	pthread_t stats_thread;

	int cm_epollfd;
	int cq_epollfd;
	int stats_epollfd;

	int32_t run_count;
};

/* GLOBAL VARIABLES */

static struct rpc_rdma_state rpc_rdma_state;

void
rpc_rdma_internals_init(void)
{
	memset(&rpc_rdma_state, 0, sizeof(rpc_rdma_state));

	sem_init(&rpc_rdma_state.c_r.q_sem, 0, 0);

	mutex_init(&rpc_rdma_state.lock, NULL);
	LIST_INIT(&rpc_rdma_state.pdh);
}

static void
rpc_rdma_internals_join(void)
{
	if (rpc_rdma_state.cm_thread) {
		pthread_join(rpc_rdma_state.cm_thread, NULL);
		rpc_rdma_state.cm_thread = 0;
	}
	if (rpc_rdma_state.cq_thread) {
		pthread_join(rpc_rdma_state.cq_thread, NULL);
		rpc_rdma_state.cq_thread = 0;
	}
	if (rpc_rdma_state.stats_thread) {
		pthread_join(rpc_rdma_state.stats_thread, NULL);
		rpc_rdma_state.stats_thread = 0;
	}
}

void
rpc_rdma_internals_fini(void)
{
	rpc_rdma_state.run_count = 0;
	rpc_rdma_internals_join();

	sem_destroy(&rpc_rdma_state.c_r.q_sem);
	sem_destroy(&rpc_rdma_state.c_r.u_sem);

	mutex_destroy(&rpc_rdma_state.lock);
}

/* forward declarations */

static int rpc_rdma_bind_server(RDMAXPRT *xprt);
static struct xp_ops rpc_rdma_ops;

/* UTILITY FUNCTIONS */

/**
 * rpc_rdma_pd_by_verbs: register the protection domain for a given xprt
 *
 * Since the protection domains seem to be related to the interfaces,
 * and those are hot-swappable and variable, need a dynamic list of them.
 * (Cannot reliably count them during initialization.)
 *
 * LFU the list to keep access near O(1).
 *
 * @param[IN] xprt	connection handle
 *
 * @return 0, ENOSPC, or EINVAL.
 */
static int
rpc_rdma_pd_by_verbs(RDMAXPRT *xprt)
{
	struct rpc_rdma_pd *pd;
	struct rpc_rdma_pd *lf;
	int rc;

	if (!xprt->cm_id) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() transport missing cm_id",
			__func__);
		return EINVAL;
	}
	if (!xprt->cm_id->verbs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() cm_id missing verbs",
			__func__);
		/* return EINVAL; legal value for dispatcher??? */
	}

	if (xprt->pd && xprt->pd->context == xprt->cm_id->verbs) {
		atomic_inc_uint32_t(&xprt->pd->pd_used);
		return (0);
	}

	mutex_lock(&rpc_rdma_state.lock);
	LIST_FOREACH(pd, &rpc_rdma_state.pdh, pdl) {
		if (pd->context == xprt->cm_id->verbs) {
			atomic_inc_uint32_t(&pd->pd_used);
			lf = LIST_FIRST(&rpc_rdma_state.pdh);
			if (pd->pd_used > lf->pd_used) {
				LIST_REMOVE(pd, pdl);
				LIST_INSERT_HEAD(&rpc_rdma_state.pdh, pd, pdl);
			}
			mutex_unlock(&rpc_rdma_state.lock);
			xprt->pd = pd;
			return (0);
		}
	}

	pd = mem_zalloc(sizeof(*pd));
	rc = 0;
	pd->context = xprt->cm_id->verbs;
	pd->pd_used = 1;

	LIST_INSERT_HEAD(&rpc_rdma_state.pdh, pd, pdl);
	mutex_unlock(&rpc_rdma_state.lock);
	xprt->pd = pd;
	return rc;
}

/**
 * rpc_rdma_pd_get: register and setup the protection domain
 *
 * @param[IN] xprt	connection handle
 *
 * @return 0, errno, ENOSPC, or EINVAL.
 */
static int
rpc_rdma_pd_get(RDMAXPRT *xprt)
{
	int rc = rpc_rdma_pd_by_verbs(xprt);

	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return rc;
	}
	if (!xprt->pd->pd) {
		xprt->pd->pd = ibv_alloc_pd(xprt->cm_id->verbs);
		if (!xprt->pd->pd) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p[%u] ibv_alloc_pd failed: %s (%d)",
				__func__, xprt, xprt->state, strerror(rc), rc);
			return rc;
		}
	}
	return (0);
}

/**
 * rpc_rdma_pd_put: de-register and teardown the protection domain
 *
 * @param[IN] xprt	connection handle
 */
static inline void
rpc_rdma_pd_put(RDMAXPRT *xprt)
{
	if (!xprt->pd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() missing protection domain?",
			__func__);
		return;
	}

	if (atomic_dec_uint32_t(&xprt->pd->pd_used) == 0) {
		mutex_lock(&rpc_rdma_state.lock);
		LIST_REMOVE(xprt->pd, pdl);
		mutex_unlock(&rpc_rdma_state.lock);

		if (xprt->pd->pd)
			ibv_dealloc_pd(xprt->pd->pd);
		xprt->pd->pd = NULL;

		if (xprt->pd->srq)
			ibv_destroy_srq(xprt->pd->srq);
		xprt->pd->srq = NULL;

		if (!TAILQ_EMPTY(&xprt->pd->srqh.qh)) {
			xdr_ioq_destroy_pool(&xprt->pd->srqh);
		}
		mem_free(xprt->pd, sizeof(*xprt->pd));
	}
	xprt->pd = NULL;
}

#ifdef UNUSED
void
rpc_rdma_print_devinfo(RDMAXPRT *xprt)
{
	struct ibv_device_attr device_attr;
	ibv_query_device(xprt->cm_id->verbs, &device_attr);
	uint64_t node_guid = ntohll(device_attr.node_guid);
	printf("guid: %04x:%04x:%04x:%04x\n",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
}
#endif /* UNUSED */

/**
 * rpc_rdma_thread_create: Simple wrapper around pthread_create
 */
static int
rpc_rdma_thread_create(pthread_t *thrid, size_t stacksize,
		       void *(*routine)(void *), void *arg)
{

	pthread_attr_t attr;
	int rc;

	/* Init for thread parameter (mostly for scheduling) */
	rc = pthread_attr_init(&attr);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't init pthread's attributes: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's scope: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's join state: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setstacksize(&attr, stacksize);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's stack size: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	return pthread_create(thrid, &attr, routine, arg);
}

static int
rpc_rdma_thread_create_epoll(pthread_t *thrid, void *(*routine)(void *),
			      void *arg, int *epollfd)
{
	int rc = 0;

	/* all calls set a thrid in rpc_rdma_state, but unlikely conflict */
	mutex_lock(&rpc_rdma_state.lock);

	if (*thrid == 0) do {
		*epollfd = epoll_create(EPOLL_SIZE);
		if (*epollfd == -1) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() epoll_create failed: %s (%d)",
				__func__, strerror(rc), rc);
			break;
		}

		rc = rpc_rdma_thread_create(thrid, WORKER_STACK_SIZE,
					routine, arg);
		if (rc) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() could not create thread: %s (%d)",
				__func__, strerror(rc), rc);
			*thrid = 0;
			break;
		}
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() thread %lx spawned for epoll %d",
			__func__,
			(unsigned long)*thrid,
			*epollfd);
	} while (0);

	mutex_unlock(&rpc_rdma_state.lock);
	return rc;
}

static void
rpc_rdma_worker_callback(struct work_pool_entry *wpe)
{
	struct rpc_rdma_cbc *cbc =
		opr_containerof(wpe, struct rpc_rdma_cbc, wpe);
	RDMAXPRT *xprt = (RDMAXPRT *)wpe->arg;

	if (cbc->status) {
		if (cbc->negative_cb) {
			__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
				"%s() %p[%u] cbc %p status: %d",
				__func__, xprt, xprt->state, cbc, cbc->status);
			cbc->negative_cb(cbc, xprt);
		}

		/* wpe->arg referenced before work_pool_submit() */
		SVC_RELEASE(&xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
		return;
	}

	switch (cbc->opcode) {
	case IBV_WC_SEND:
	case IBV_WC_RDMA_WRITE:
	case IBV_WC_RDMA_READ:
	case IBV_WC_RECV:
	case IBV_WC_RECV_RDMA_WITH_IMM:
		if (cbc->positive_cb) {
			__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
				"%s() %p[%u] cbc %p opcode: %d",
				__func__, xprt, xprt->state, cbc, cbc->opcode);
			cbc->positive_cb(cbc, xprt);
		}
		break;

	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p opcode: %d unknown",
			__func__, xprt, xprt->state, cbc, cbc->opcode);
		break;
	}

	/* wpe->arg referenced before work_pool_submit() */
	SVC_RELEASE(&xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
}

/**
 * rpc_rdma_fd_add: Adds fd to the epoll wait
 *
 * Returns 0 on success, errno value on error.
 */
static int
rpc_rdma_fd_add(RDMAXPRT *xprt, int fd, int epollfd)
{
	struct epoll_event ev;
	int flags = fcntl(fd, F_GETFL);
	int rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (rc < 0) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p Failed to make the channel nonblock: %s (%d)",
			__func__, xprt, strerror(rc), rc);
		return rc;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = xprt;

	rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
	if (rc == -1) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p Failed to add fd to epoll: %s (%d)",
			__func__, xprt, strerror(rc), rc);
		return rc;
	}

	return 0;
}

static int
rpc_rdma_fd_del(int fd, int epollfd)
{
	int rc = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);

	/* Let epoll deal with multiple deletes of the same fd */
	if (rc == -1 && errno != ENOENT) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Failed to del fd to epoll: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	return 0;
}

static inline int
rpc_rdma_stats_add(RDMAXPRT *xprt)
{
	struct sockaddr_un sockaddr;
	int rc;

	/* no stats if no prefix */
	if (!xprt->xa->statistics_prefix)
		return 0;

	/* setup xprt->stats_sock here */
	xprt->stats_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (xprt->stats_sock == -1) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() socket on stats socket failed, quitting thread: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sun_family = AF_UNIX;
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path)-1, "%s%p",
		xprt->xa->statistics_prefix, xprt);

	unlink(sockaddr.sun_path);

	rc = bind(xprt->stats_sock, (struct sockaddr*)&sockaddr,
						sizeof(sockaddr));
	if (rc == -1) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() bind on stats socket failed, quitting thread: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = listen(xprt->stats_sock, 5);
	if (rc == -1) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() listen on stats socket failed, quitting thread: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	return rpc_rdma_fd_add(xprt, xprt->stats_sock,
				rpc_rdma_state.stats_epollfd);
}

static inline int
rpc_rdma_stats_del(RDMAXPRT *xprt)
{
	/* rpc_rdma_fd_del is called in stats thread on a close event */
	char sun_path[108];

	snprintf(sun_path, sizeof(sun_path)-1, "%s%p",
		xprt->xa->statistics_prefix, xprt);
	unlink(sun_path);

	return close(xprt->stats_sock);
}

/**
 * rpc_rdma_stats_thread: unix socket thread
 *
 * Well, a thread. arg = xprt
 */
static void *
rpc_rdma_stats_thread(void *arg)
{
	RDMAXPRT *xprt;
	struct epoll_event epoll_events[EPOLL_EVENTS];
	char stats_str[256];
	int childfd;
	int i;
	int n;
	int rc;

	rcu_register_thread();
	while (rpc_rdma_state.run_count > 0) {
		n = epoll_wait(rpc_rdma_state.stats_epollfd,
				epoll_events, EPOLL_EVENTS, EPOLL_WAIT_MS);
		if (n == 0)
			continue;

		if (n == -1) {
			if (errno == EINTR)
				continue;

			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() epoll_wait failed: %s (%d)",
				__func__, strerror(rc), rc);
			break;
		}

		for (i = 0; i < n; ++i) {
			xprt = (RDMAXPRT*)epoll_events[i].data.ptr;
			if (!xprt) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() no xprt: got an event on a fd that should have been removed!",
					__func__);
				continue;
			}

			if (epoll_events[i].events == EPOLLERR
			 || epoll_events[i].events == EPOLLHUP) {
				rpc_rdma_fd_del(xprt->stats_sock,
						rpc_rdma_state.stats_epollfd);
				continue;
			}

			childfd = accept(xprt->stats_sock, NULL, NULL);
			if (childfd == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s() accept on stats socket failed: %s (%d)",
						__func__, strerror(errno), errno);
					continue;
				}
			}

			rc = snprintf(stats_str, sizeof(stats_str), "stats:\n"
				"	tx_bytes\ttx_pkt\ttx_err\n"
				"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
				"	rx_bytes\trx_pkt\trx_err\n"
				"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
				"	callback time:   %lu.%09lu s\n"
				"	completion time: %lu.%09lu s\n",
				xprt->stats.tx_bytes, xprt->stats.tx_pkt,
				xprt->stats.tx_err, xprt->stats.rx_bytes,
				xprt->stats.rx_pkt, xprt->stats.rx_err,
				xprt->stats.nsec_callback / NSEC_IN_SEC,
				xprt->stats.nsec_callback % NSEC_IN_SEC,
				xprt->stats.nsec_compevent / NSEC_IN_SEC,
				xprt->stats.nsec_compevent % NSEC_IN_SEC);
			rc = write(childfd, stats_str, rc);
			rc = close(childfd);
		}
	}
	rcu_unregister_thread();
	pthread_exit(NULL);
}

/**
 * rpc_rdma_cq_event_handler: completion queue event handler.
 *
 * marks contexts back out of use,
 * calls the appropriate callbacks for each kind of event.
 *
 * @return 0 on success, work completion status if not 0
 */
static int
rpc_rdma_cq_event_handler(RDMAXPRT *xprt)
{
	struct ibv_wc wc[IBV_POLL_EVENTS];
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	struct rpc_rdma_cbc *cbc;
	struct xdr_ioq_uv *data;
	int i;
	int rc;
	int npoll = 0;
	uint32_t len;

	rc = ibv_get_cq_event(xprt->comp_channel, &ev_cq, &ev_ctx);
	if (rc) {
		rc = errno;
		if (rc != EAGAIN) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() ibv_get_cq_event failed: %d.",
				__func__, rc);
		}
		return rc;
	}
	if (ev_cq != xprt->cq) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Unknown cq %p",
			__func__, ev_cq);
		ibv_ack_cq_events(ev_cq, 1);
		return EINVAL;
	}

	rc = ibv_req_notify_cq(xprt->cq, 0);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() ibv_req_notify_cq failed: %d.",
			__func__, rc);
	}

	while (rc == 0
	       && (npoll = ibv_poll_cq(xprt->cq, IBV_POLL_EVENTS, wc)) > 0) {
		for (i = 0; i < npoll; i++) {
			if (xprt->bad_recv_wr) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() Something was bad on that recv",
					__func__);
			}
			if (xprt->bad_send_wr) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() Something was bad on that send",
					__func__);
			}
			cbc = (struct rpc_rdma_cbc *)wc[i].wr_id;
			cbc->opcode = wc[i].opcode;
			cbc->status = wc[i].status;
			cbc->wpe.arg = xprt;

			if (wc[i].status) {
				switch (wc[i].opcode) {
					case IBV_WC_SEND:
					case IBV_WC_RDMA_WRITE:
					case IBV_WC_RDMA_READ:
						xprt->stats.tx_err++;
						break;
					case IBV_WC_RECV:
					case IBV_WC_RECV_RDMA_WITH_IMM:
						xprt->stats.rx_err++;
						break;
					default:
						break;
				}

				SVC_REF(&xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
				work_pool_submit(&svc_work_pool, &cbc->wpe);

				if (xprt->state != RDMAXS_CLOSING
				 && xprt->state != RDMAXS_CLOSED
				 && xprt->state != RDMAXS_ERROR) {
					rc = wc[i].status;
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s() cq completion failed status: %s (%d)",
						__func__,
						ibv_wc_status_str(rc), rc);

				}
				continue;
			}

			switch (wc[i].opcode) {
			case IBV_WC_SEND:
			case IBV_WC_RDMA_WRITE:
			case IBV_WC_RDMA_READ:
				len = wc[i].byte_len;
				xprt->stats.tx_bytes += len;
				xprt->stats.tx_pkt++;

				__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
					"%s() WC_SEND/RDMA_WRITE/RDMA_READ: %d len %u",
					__func__,
					wc[i].opcode,
					len);

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME cbc->data->imm_data = ntohl(wc.imm_data);
					__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
						"%s() imm_data: %d",
						__func__,
						ntohl(wc[i].imm_data));
				}

				SVC_REF(&xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
				work_pool_submit(&svc_work_pool, &cbc->wpe);
				break;

			case IBV_WC_RECV:
			case IBV_WC_RECV_RDMA_WITH_IMM:
				len = wc[i].byte_len;
				xprt->stats.rx_bytes += len;
				xprt->stats.rx_pkt++;

				__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
					"%s() WC_RECV: %d len %u",
					__func__,
					wc[i].opcode,
					len);

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME cbc->data->imm_data = ntohl(wc.imm_data);
					__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
						"%s() imm_data: %d",
						__func__,
						ntohl(wc[i].imm_data));
				}

				/* fill all the sizes in case of multiple sge
				 * assumes _tail was set to _wrap before call
				 */
				data = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
				while (data && ioquv_length(data) < len) {
					VALGRIND_MAKE_MEM_DEFINED(data->v.vio_head, ioquv_length(data));
					len -= ioquv_length(data);
					data = IOQ_(TAILQ_NEXT(&data->uvq, q));
				}
				if (data) {
					data->v.vio_tail = data->v.vio_head + len;
					VALGRIND_MAKE_MEM_DEFINED(data->v.vio_head, ioquv_length(data));
				} else if (len) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s() ERROR %d leftover bytes?",
						__func__, len);
				}

				SVC_REF(&xprt->sm_dr.xprt, SVC_REF_FLAG_NONE);
				work_pool_submit(&svc_work_pool, &cbc->wpe);
				break;

			default:
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() unknown opcode: %d",
					__func__, wc[i].opcode);
				rc = EINVAL;
			}
		}
	}

	if (npoll < 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] ibv_poll_cq failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(-npoll), -npoll);
		rc = -npoll;
	}

	ibv_ack_cq_events(xprt->cq, 1);

	return -rc;
}

/**
 * rpc_rdma_cq_thread: thread function which waits for new completion events
 * and gives them to handler (then ack the event)
 *
 */
static void *
rpc_rdma_cq_thread(void *arg)
{
	RDMAXPRT *xprt;
	struct epoll_event epoll_events[EPOLL_EVENTS];
	int i;
	int n;
	int rc;

	rcu_register_thread();
	while (rpc_rdma_state.run_count > 0) {
		n = epoll_wait(rpc_rdma_state.cq_epollfd,
				epoll_events, EPOLL_EVENTS, EPOLL_WAIT_MS);
		if (n == 0)
			continue;

		if (n == -1) {
			if (errno == EINTR)
				continue;

			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() epoll_wait failed: %s (%d)",
				__func__, strerror(rc), rc);
			break;
		}

		for (i = 0; i < n; ++i) {
			xprt = (RDMAXPRT*)epoll_events[i].data.ptr;
			if (!xprt) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() got an event on a fd that should have been removed! (no xprt)",
					__func__);
				continue;
			}

			if (epoll_events[i].events == EPOLLERR
			 || epoll_events[i].events == EPOLLHUP) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() epoll error or hup (%d)",
					__func__, epoll_events[i].events);
				continue;
			}

			mutex_lock(&xprt->cm_lock);
			if (xprt->state >= RDMAXS_CLOSING) {
				/* CLOSING, CLOSED, ERROR */
				// closing xprt, skip this, will be done on flush
				rpc_rdma_fd_del(xprt->comp_channel->fd,
						rpc_rdma_state.cq_epollfd);
				mutex_unlock(&xprt->cm_lock);
				continue;
			}

			rc = rpc_rdma_cq_event_handler(xprt);
			if (rc) {
				if (xprt->state != RDMAXS_CLOSING
				 && xprt->state != RDMAXS_CLOSED
				 && xprt->state != RDMAXS_ERROR) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s() something went wrong with our cq_event_handler",
						__func__);
					xprt->state = RDMAXS_ERROR;
					cond_broadcast(&xprt->cm_cond);
				}
			}
			mutex_unlock(&xprt->cm_lock);
		}
	}
	rcu_unregister_thread();
	pthread_exit(NULL);
}

/**
 * rpc_rdma_cm_event_handler: handles addr/route resolved events (client side)
 * and disconnect (everyone)
 *
 */
static int
rpc_rdma_cm_event_handler(RDMAXPRT *ep_xprt, struct rdma_cm_event *event)
{
	struct rdma_cm_id *cm_id = event->id;
	RDMAXPRT *xprt = cm_id->context;
	uint32_t u;
	int rc = 0;

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() %p cma_event type %s",
		__func__, ep_xprt, rdma_event_str(event->event));

	if (xprt->bad_recv_wr) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Something was bad on that recv",
			__func__);
	}
	if (xprt->bad_send_wr) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Something was bad on that send",
			__func__);
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p ADDR_RESOLVED",
			__func__, xprt);
		mutex_lock(&xprt->cm_lock);
		xprt->state = RDMAXS_ADDR_RESOLVED;
		cond_broadcast(&xprt->cm_cond);
		mutex_unlock(&xprt->cm_lock);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p ROUTE_RESOLVED",
			__func__, xprt);
		mutex_lock(&xprt->cm_lock);
		xprt->state = RDMAXS_ROUTE_RESOLVED;
		cond_broadcast(&xprt->cm_cond);
		mutex_unlock(&xprt->cm_lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p ESTABLISHED",
			__func__, xprt);

		xprt->state = RDMAXS_CONNECTED;

		rc = rpc_rdma_fd_add(xprt, xprt->comp_channel->fd,
					rpc_rdma_state.cq_epollfd);
		if (rc) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
		}
		rpc_rdma_stats_add(xprt);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p CONNECT_REQUEST",
			__func__, xprt);

		rc = sem_trywait(&rpc_rdma_state.c_r.u_sem);
		if (rc) {
			rc = errno;
			if (EAGAIN != rc) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() %p[%u] sem_trywait failed: %s (%d)",
					__func__, xprt, xprt->state,
					strerror(rc), rc);
				return rc;
			}

			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p WARNING too many connection requests! "
				"Need to increase backlog parameter.\n",
				__func__, xprt);

			/* After advisory message, wait for available slot.
			 */
			rc = sem_wait(&rpc_rdma_state.c_r.u_sem);
			if (rc) {
				rc = errno;
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() %p[%u] sem_wait failed: %s (%d)",
					__func__, xprt, xprt->state,
					strerror(rc), rc);
				return rc;
			}
		}

		u = atomic_postinc_uint32_t(&rpc_rdma_state.c_r.q_tail);
		if (u >= rpc_rdma_state.c_r.q_size) {
			u_int q_mask = rpc_rdma_state.c_r.q_size - 1;

			/* masking allows update by lock-free tasks,
			 * as long as overrun never 2 * q_size
			 */
			u &= q_mask;
			atomic_clear_uint32_t_bits(&rpc_rdma_state.c_r.q_tail,
						   ~q_mask);
		}
		rpc_rdma_state.c_r.id_queue[u] = cm_id;

		/* signal accept handler */
		rc = sem_post(&rpc_rdma_state.c_r.q_sem);
		if (rc) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() sem_post failed",
				__func__, strerror(rc), rc);
			return rc;
		}
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cma event %s, error %d",
			__func__, xprt, xprt->state,
			rdma_event_str(event->event), event->status);
		mutex_lock(&xprt->cm_lock);
		xprt->state = RDMAXS_ERROR;
		cond_broadcast(&xprt->cm_cond);
		mutex_unlock(&xprt->cm_lock);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p[%u] DISCONNECT EVENT...",
			__func__, xprt, xprt->state);

		// don't call completion again
		if (xprt->comp_channel)
			rpc_rdma_fd_del(xprt->comp_channel->fd,
					rpc_rdma_state.cq_epollfd);

		mutex_lock(&xprt->cm_lock);
		xprt->state = RDMAXS_CLOSED;
		cond_broadcast(&xprt->cm_cond);
		mutex_unlock(&xprt->cm_lock);

		if (__svc_params->disconnect_cb)
			__svc_params->disconnect_cb(&xprt->sm_dr.xprt);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
			"%s() %p[%u] cma detected device removal!!!!",
			__func__, xprt, xprt->state);
		rc = ENODEV;
		break;

	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] unhandled event: %s, ignoring %d\n",
			__func__, xprt, xprt->state,
			rdma_event_str(event->event), event->event);
		break;
	}

	return rc;
}

/**
 * rpc_rdma_cm_thread: thread function which waits for new connection events
 * and gives them to handler (then ack the event)
 *
 */
static void *
rpc_rdma_cm_thread(void *nullarg)
{
	RDMAXPRT *xprt;
	RDMAXPRT *cm_xprt;
	struct rdma_cm_event *event;
	struct epoll_event epoll_events[EPOLL_EVENTS];
	int i;
	int n;
	int rc;

	rcu_register_thread();
	while (rpc_rdma_state.run_count > 0) {
		n = epoll_wait(rpc_rdma_state.cm_epollfd,
				epoll_events, EPOLL_EVENTS, EPOLL_WAIT_MS);
		if (n == 0)
			continue;

		if (n == -1) {
			if (errno == EINTR)
				continue;

			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p[%u] epoll_wait failed: %s (%d)",
				__func__, xprt, xprt->state, strerror(rc), rc);
			break;
		}

		for (i = 0; i < n; ++i) {
			xprt = (RDMAXPRT*)epoll_events[i].data.ptr;
			if (!xprt) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() got an event on a fd that should have been removed! (no xprt)",
					__func__);
				continue;
			}

			if (epoll_events[i].events == EPOLLERR
			 || epoll_events[i].events == EPOLLHUP) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() epoll error or hup (%d)",
					__func__, epoll_events[i].events);
				continue;
			}

			if (xprt->state == RDMAXS_CLOSED) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() got a cm event on a closed xprt?",
					__func__);
				continue;
			}

			if (!xprt->event_channel) {
				if (xprt->state != RDMAXS_CLOSED)
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s() no event channel? :D",
						__func__);
				continue;
			}

			rc = rdma_get_cm_event(xprt->event_channel, &event);
			if (rc) {
				rc = errno;
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() rdma_get_cm_event failed: %d.",
					__func__, rc);
				continue;
			}

			cm_xprt = event->id->context;
			rc = rpc_rdma_cm_event_handler(xprt, event);
			rdma_ack_cm_event(event);

			if (rc
			 && (cm_xprt->state != RDMAXS_LISTENING
			  || cm_xprt == xprt)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() rpc_rdma_cm_event_handler: %d.",
					__func__, rc);
			}

			if (cm_xprt->state == RDMAXS_CLOSED
			 && cm_xprt->destroy_on_disconnect)
				SVC_DESTROY(&cm_xprt->sm_dr.xprt);
		}
	}
	rcu_unregister_thread();
	pthread_exit(NULL);
}

/**
 * rpc_rdma_flush_buffers: Flush all pending recv/send
 *
 * @param[IN] xprt
 *
 * @return void
 */
static void
rpc_rdma_flush_buffers(RDMAXPRT *xprt)
{
	int rc;

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() %p[%u]",
		__func__, xprt, xprt->state);

	mutex_lock(&xprt->cm_lock);

	if (xprt->state != RDMAXS_ERROR) {
		do {
			rc = rpc_rdma_cq_event_handler(xprt);
		} while (rc == 0);

		if (rc != EAGAIN)
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() couldn't flush pending data in cq: %d",
				__func__, rc);
	}
#ifdef FIXME
	/* only flush rx if client or accepting server */
	if (xprt->server >= 0)
	    for (i = 0, ctx = xprt->rcb;
		 i < xprt->xa->rq_depth;
		 i++,
		 ctx = (struct rpc_rdma_cbc*)((uint8_t*)ctx + sizeof(struct rpc_rdma_cbc) + xprt->xa->max_recv_sge*sizeof(struct ibv_sge)))
			if (ctx->used == MSK_CTX_PENDING)
				rpc_rdma_worker_signal(xprt, ctx, IBV_WC_FATAL_ERR, IBV_WC_RECV);

	for (i = 0, ctx = (struct rpc_rdma_cbc *)xprt->wcb;
	     i < xprt->xa->sq_depth;
	     i++, ctx = (struct rpc_rdma_cbc*)((uint8_t*)ctx + sizeof(struct rpc_rdma_cbc) + xprt->xa->max_send_sge*sizeof(struct ibv_sge)))
		if (ctx->used == MSK_CTX_PENDING)
			rpc_rdma_worker_signal(xprt, ctx, IBV_WC_FATAL_ERR, IBV_WC_SEND);

	/* only flush rx if client or accepting server */
	if (xprt->server >= 0) do {
		wait = 0;
			for (i = 0, ctx = xprt->rcb;
			     i < xprt->xa->rq_depth;
			     i++, ctx = rpc_rdma_next_ctx(ctx, xprt->xa->max_recv_sge))
				if (ctx->used != MSK_CTX_FREE)
					wait++;

	} while (wait && usleep(100000));
	do {
		wait = 0;
		for (i = 0, ctx = (struct rpc_rdma_cbc *)xprt->wcb;
		     i < xprt->xa->sq_depth;
		     i++, ctx = rpc_rdma_next_ctx(ctx, xprt->xa->max_recv_sge))
			if (ctx->used != MSK_CTX_FREE)
				wait++;

	} while (wait && usleep(100000));
#endif
	mutex_unlock(&xprt->cm_lock);
}

/**
 * rpc_rdma_destroy_stuff: destroys all qp-related stuff for us
 *
 * @param[INOUT] xprt
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void
rpc_rdma_destroy_stuff(RDMAXPRT *xprt)
{
	if (xprt->qp) {
		// flush all pending receive/send buffers to error callback
		rpc_rdma_flush_buffers(xprt);

		ibv_destroy_qp(xprt->qp);
		xprt->qp = NULL;
	}

	if (xprt->cq) {
		ibv_destroy_cq(xprt->cq);
		xprt->cq = NULL;
	}

	if (xprt->comp_channel) {
		ibv_destroy_comp_channel(xprt->comp_channel);
		xprt->comp_channel = NULL;
	}

	if (!TAILQ_EMPTY(&xprt->cbqh.qh)) {
		xdr_ioq_destroy_pool(&xprt->cbqh);
	}
}

/**
 * rpc_rdma_destroy: disconnects and free transport data
 *
 * @param[IN] xd	pointer to the service transport to destroy
 */
void
rpc_rdma_destroy(RDMAXPRT *xd)
{
	/* inhibit repeated destroy */
	xd->destroy_on_disconnect = false;

	if (xd->state == RDMAXS_CONNECTED
	 || xd->state == RDMAXS_CLOSED) {
		mutex_lock(&xd->cm_lock);
		if (xd->state != RDMAXS_CLOSED
		 && xd->state != RDMAXS_LISTENING
		 && xd->state != RDMAXS_ERROR)
			xd->state = RDMAXS_CLOSING;

		if (xd->cm_id && xd->cm_id->verbs)
			rdma_disconnect(xd->cm_id);

		while (xd->state != RDMAXS_CLOSED
			&& xd->state != RDMAXS_LISTENING
			&& xd->state != RDMAXS_ERROR) {
			__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
				"%s() we're not closed yet, "
				"waiting for disconnect_event",
				__func__);
			cond_wait(&xd->cm_cond, &xd->cm_lock);
		}
		xd->state = RDMAXS_CLOSED;
		mutex_unlock(&xd->cm_lock);
	}

	if (xd->cm_id) {
		rdma_destroy_id(xd->cm_id);
		xd->cm_id = NULL;
	}

	if (xd->stats_sock)
		rpc_rdma_stats_del(xd);

	/* event channel is shared between all children,
	 * so don't close it unless it's its own.
	 */
	if ((xd->server != RDMAX_SERVER_CHILD)
	 && xd->event_channel) {
		rpc_rdma_fd_del(xd->event_channel->fd,
				rpc_rdma_state.cm_epollfd);
		rdma_destroy_event_channel(xd->event_channel);
		xd->event_channel = NULL;
	}

	rpc_rdma_destroy_stuff(xd);
	rpc_rdma_pd_put(xd);

	if (atomic_dec_int32_t(&rpc_rdma_state.run_count) <= 0) {
		mutex_lock(&rpc_rdma_state.lock);
		rpc_rdma_internals_join();
		mutex_unlock(&rpc_rdma_state.lock);
	}

	/* destroy locking last, was initialized first (below).
	 */
	cond_destroy(&xd->cm_cond);
	mutex_destroy(&xd->cm_lock);
	XDR_DESTROY(xd->sm_dr.ioq.xdrs);
	rpc_dplx_rec_destroy(&xd->sm_dr);

	mem_free(xd, sizeof(*xd));
}

/**
 * rpc_rdma_allocate: allocate rdma transport structures
 *
 * @param[IN] xa	parameters
 *
 * @return xprt on success, NULL on failure
 */
static RDMAXPRT *
rpc_rdma_allocate(const struct rpc_rdma_attr *xa)
{
	RDMAXPRT *xd;
	int rc;

	if (!xa) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Invalid argument",
			__func__);
		return NULL;
	}

	xd = mem_zalloc(sizeof(*xd));

	xd->sm_dr.xprt.xp_type = XPRT_RDMA;
	xd->sm_dr.xprt.xp_refcnt = 1;
	xd->sm_dr.xprt.xp_ops = &rpc_rdma_ops;

	xd->xa = xa;
	xd->conn_type = RDMA_PS_TCP;
	xd->destroy_on_disconnect = xa->destroy_on_disconnect;

	/* initialize locking first, will be destroyed last (above).
	 */
	xdr_ioq_setup(&xd->sm_dr.ioq);
	rpc_dplx_rec_init(&xd->sm_dr);

	rc = mutex_init(&xd->cm_lock, NULL);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() mutex_init failed: %s (%d)",
			__func__, strerror(rc), rc);
		goto cm_lock;
	}

	rc = cond_init(&xd->cm_cond, NULL, NULL);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() cond_init failed: %s (%d)",
			__func__, strerror(rc), rc);
		goto cm_cond;
	}

	return (xd);

cm_cond:
	mutex_destroy(&xd->cm_lock);
cm_lock:
	mutex_destroy(&xd->sm_dr.xprt.xp_lock);

	mem_free(xd, sizeof(*xd));
	return NULL;
}

/**
 * rpc_rdma_ncreatef: initialize rdma transport structures
 *
 * @param[IN] xa		parameters
 * @param[IN] sendsize;		max send size
 * @param[IN] recvsize;		max recv size
 * @param[IN] flags; 		unused
 *
 * @return xprt on success, NULL on failure
 */
SVCXPRT *
rpc_rdma_ncreatef(const struct rpc_rdma_attr *xa,
		  const u_int sendsize, const u_int recvsize,
		  const uint32_t flags)
{
	RDMAXPRT *xd;
	int rc;

	if (xa->backlog > 4096) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() backlog (%u) much too large",
			__func__, xa->backlog);
		return NULL;
	}

	xd = rpc_rdma_allocate(xa);
	if (!xd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return NULL;
	}
	xd->server = xa->backlog; /* convenient number > 0 */

	/* presence of event_channel confirms RDMA,
	 * otherwise, cleanup and quit RDMA dispatcher.
	 */
	xd->event_channel = rdma_create_event_channel();
	if (!xd->event_channel) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() create_event_channel failed: %s (%d)",
			__func__, strerror(rc), rc);
		goto failure;
	}

	rc = rdma_create_id(xd->event_channel, &xd->cm_id, xd,
			    xd->conn_type);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() create_id failed: %s (%d)",
			__func__, strerror(rc), rc);
		goto failure;
	}

	pthread_mutex_lock(&svc_work_pool.pqh.qmutex);
	if (!svc_work_pool.params.thrd_max) {
		pthread_mutex_unlock(&svc_work_pool.pqh.qmutex);

		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() svc_work_pool already shutdown",
			__func__);
		goto failure;
	}
	pthread_mutex_unlock(&svc_work_pool.pqh.qmutex);

	/* buffer sizes MUST be page sized */
	xd->sm_dr.pagesz = sysconf(_SC_PAGESIZE);
	if (recvsize) {
		/* round up */
		xd->sm_dr.recvsz = recvsize + (xd->sm_dr.pagesz - 1);
		xd->sm_dr.recvsz &= ~(xd->sm_dr.pagesz - 1);
	} else {
		/* default */
		xd->sm_dr.recvsz = xd->sm_dr.pagesz;
	}
	if (sendsize) {
		/* round up */
		xd->sm_dr.sendsz = sendsize + (xd->sm_dr.pagesz - 1);
		xd->sm_dr.sendsz &= ~(xd->sm_dr.pagesz - 1);
	} else {
		/* default */
		xd->sm_dr.recvsz = xd->sm_dr.pagesz;
	}

	/* round up to the next power of two */
	rpc_rdma_state.c_r.q_size = 2;
	while (rpc_rdma_state.c_r.q_size < xa->backlog) {
		rpc_rdma_state.c_r.q_size <<= 1;
	}
	rpc_rdma_state.c_r.id_queue = mem_alloc(rpc_rdma_state.c_r.q_size
						* sizeof(struct rdma_cm_id *));
	sem_init(&rpc_rdma_state.c_r.u_sem, 0, rpc_rdma_state.c_r.q_size);

	rc = rpc_rdma_bind_server(xd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() NFS/RDMA dispatcher could not bind engine",
			__func__);
		goto failure;
	}
	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() NFS/RDMA engine bound",
		__func__);

	return (&xd->sm_dr.xprt);

failure:
	rpc_rdma_destroy(xd);
	return NULL;
}

/**
 * rpc_rdma_create_qp: create a qp associated with a xprt
 *
 * @param[INOUT] xprt
 * @param[IN] cm_id
 *
 * @return 0 on success, errno value on error
 */
static int
rpc_rdma_create_qp(RDMAXPRT *xprt, struct rdma_cm_id *cm_id)
{
	int rc;
	struct ibv_qp_init_attr qp_attr = {
		.cap.max_send_wr = xprt->xa->sq_depth,
		.cap.max_send_sge = xprt->xa->max_send_sge,
		.cap.max_recv_wr = xprt->xa->rq_depth,
		.cap.max_recv_sge = xprt->xa->max_recv_sge,
		.cap.max_inline_data = 0, // change if IMM
		.qp_type = (xprt->conn_type == RDMA_PS_UDP
			? IBV_QPT_UD : IBV_QPT_RC),
		.sq_sig_all = 1,
		.send_cq = xprt->cq,
		.recv_cq = xprt->cq,
		.srq = xprt->srq,
	};

	rc = rdma_create_qp(cm_id, xprt->pd->pd, &qp_attr);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_create_qp failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	xprt->qp = cm_id->qp;
	return 0;
}

/**
 * rpc_rdma_setup_stuff: setup pd, qp an' stuff
 *
 * @param[INOUT] xprt
 *
 * @return 0 on success, errno value on failure
 */
static int
rpc_rdma_setup_stuff(RDMAXPRT *xprt)
{
	int rc;

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() %p[%u]",
		__func__, xprt, xprt->state);

	/* Located in this function for convenience, called by both
	 * client and server. Each is only done once for all connections.
	 */
	rc = rpc_rdma_thread_create_epoll(&rpc_rdma_state.cq_thread,
		rpc_rdma_cq_thread, xprt, &rpc_rdma_state.cq_epollfd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return rc;
	}

	if (xprt->xa->statistics_prefix != NULL
	 && (rc = rpc_rdma_thread_create_epoll(&rpc_rdma_state.stats_thread,
		rpc_rdma_stats_thread, xprt, &rpc_rdma_state.stats_epollfd))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return rc;
	}

	xprt->comp_channel = ibv_create_comp_channel(xprt->cm_id->verbs);
	if (!xprt->comp_channel) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] ibv_create_comp_channel failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	xprt->cq = ibv_create_cq(xprt->cm_id->verbs,
				xprt->xa->sq_depth + xprt->xa->rq_depth,
				xprt, xprt->comp_channel, 0);
	if (!xprt->cq) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] ibv_create_cq failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	rc = ibv_req_notify_cq(xprt->cq, 0);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] ibv_req_notify_cq failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	rc = rpc_rdma_create_qp(xprt, xprt->cm_id);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return rc;
	}

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() %p[%u] created qp %p",
		__func__, xprt, xprt->state, xprt->qp);
	return 0;
}

/**
 * rpc_rdma_setup_cbq
 */
static int
rpc_rdma_setup_cbq(struct poolq_head *ioqh, u_int depth, u_int sge)
{
	struct rpc_rdma_cbc *cbc;

	if (ioqh->qsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() contexts already allocated",
			__func__);
		return EINVAL;
	}
	ioqh->qsize = sizeof(struct rpc_rdma_cbc)
		    + sizeof(struct ibv_sge) * sge;
	TAILQ_INIT(&ioqh->qh);

	/* individual entries is less efficient than big array -- but uses
	 * "standard" IOQ operations, xdr_ioq_destroy_pool(), and
	 * debugging memory bounds checking of trailing ibv_sge array.
	 */
	while (depth--) {
		cbc = mem_zalloc(ioqh->qsize);

		xdr_ioq_setup(&cbc->workq);
		xdr_ioq_setup(&cbc->holdq);

		cbc->workq.ioq_uv.uvq_fetch =
		cbc->holdq.ioq_uv.uvq_fetch = xdr_ioq_uv_fetch_nothing;
		cbc->workq.xdrs[0].x_ops =
		cbc->holdq.xdrs[0].x_ops = &xdr_ioq_ops;
		cbc->workq.xdrs[0].x_op =
		cbc->holdq.xdrs[0].x_op = XDR_FREE; /* catch setup errors */

		cbc->workq.ioq_pool = ioqh;
		cbc->wpe.fun = rpc_rdma_worker_callback;

		(ioqh->qcount)++;
		TAILQ_INSERT_TAIL(&ioqh->qh, &cbc->workq.ioq_s, q);
	}
	return 0;
}

/**
 * rpc_rdma_bind_server
 *
 * @param[INOUT] xprt
 *
 * @return 0 on success, errno value on failure
 */
static int
rpc_rdma_bind_server(RDMAXPRT *xprt)
{
	struct rdma_addrinfo *res;
	struct rdma_addrinfo hints;
	int rc;

	if (!xprt || xprt->state != RDMAXS_INITIAL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] must be initialized first!",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	if (xprt->server <= 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() Must be on server side to call this function",
			__func__);
		return EINVAL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = xprt->conn_type;

	rc = rdma_getaddrinfo(xprt->xa->node, xprt->xa->port, &hints, &res);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_getaddrinfo: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	rc = rdma_bind_addr(xprt->cm_id, res->ai_src_addr);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_bind_addr: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}
	rdma_freeaddrinfo(res);

	/* at this point, the cm_id->verbs aren't filled */
	rc = rpc_rdma_pd_by_verbs(xprt);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] register pd failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	rc = rdma_listen(xprt->cm_id, xprt->xa->backlog);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_listen failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	xprt->state = RDMAXS_LISTENING;
	atomic_inc_int32_t(&rpc_rdma_state.run_count);

	rc = rpc_rdma_thread_create_epoll(&rpc_rdma_state.cm_thread,
		rpc_rdma_cm_thread, xprt, &rpc_rdma_state.cm_epollfd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rpc_rdma_thread_create_epoll failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		(void) atomic_dec_int32_t(&rpc_rdma_state.run_count);
		return rc;
	}

	return rpc_rdma_fd_add(xprt, xprt->event_channel->fd,
				rpc_rdma_state.cm_epollfd);
}

/**
 * rpc_rdma_clone: clone child from listener parent
 *
 * @param[IN] l_xprt	listening (parent) transport
 * @param[IN] cm_id	new rdma connection manager identifier
 *
 * @return 0 on success, errno value on failure
 */
static RDMAXPRT *
rpc_rdma_clone(RDMAXPRT *l_xprt, struct rdma_cm_id *cm_id)
{
	RDMAXPRT *xd = rpc_rdma_allocate(l_xprt->xa);
	int rc;

	if (!xd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return NULL;
	}

	xd->cm_id = cm_id;
	xd->cm_id->context = xd;
	xd->state = RDMAXS_CONNECT_REQUEST;
	xd->server = RDMAX_SERVER_CHILD;

	xd->event_channel = l_xprt->event_channel;

	rc = rpc_rdma_pd_get(xd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		goto failure;
	}

	if (l_xprt->xa->use_srq) {
		if (!xd->pd->srq) {
			struct ibv_srq_init_attr srq_attr = {
				.attr.max_wr = xd->xa->rq_depth,
				.attr.max_sge = xd->xa->max_recv_sge,
			};
			xd->pd->srq =
				ibv_create_srq(xd->pd->pd, &srq_attr);
			if (!xd->pd->srq) {
				rc = errno;
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() ibv_create_srq failed: %s (%d)",
					__func__, strerror(rc), rc);
				goto failure;
			}
		}

		if (!xd->pd->srqh.qcount) {
			rc = rpc_rdma_setup_cbq(&xd->pd->srqh,
						xd->xa->rq_depth,
						xd->xa->max_recv_sge);
			if (rc) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR (return)",
					__func__, __LINE__);
				goto failure;
			}
		}

		/* only send contexts */
		rc = rpc_rdma_setup_cbq(&xd->cbqh,
					xd->xa->sq_depth,
					xd->xa->credits);
		if (rc) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			goto failure;
		}
	} else {
		rc = rpc_rdma_setup_cbq(&xd->cbqh,
					xd->xa->rq_depth +
					xd->xa->sq_depth,
					xd->xa->credits);
		if (rc) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			goto failure;
		}
	}

	/* srq only used as a boolean here */
	xd->srq = xd->pd->srq;

	rc = rpc_rdma_setup_stuff(xd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		goto failure;
	}

	atomic_inc_int32_t(&rpc_rdma_state.run_count);
	return xd;

failure:
	rpc_rdma_destroy(xd);
	return (NULL);
}

/**
 * rpc_rdma_accept_finalize: does the real connection acceptance
 * N.B. no wait for CM result. CM event thread will handle setup/teardown.
 *
 * @param[IN] xprt
 *
 * @return 0 on success, the value of errno on error
 */
int
rpc_rdma_accept_finalize(RDMAXPRT *xprt)
{
	struct rdma_conn_param conn_param;
	int rc;

	if (!xprt || xprt->state != RDMAXS_CONNECT_REQUEST) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] isn't from a connection request?",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.private_data = NULL;
	conn_param.private_data_len = 0;
	conn_param.rnr_retry_count = 10;

	rc = rdma_accept(xprt->cm_id, &conn_param);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_accept failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
	}

	return rc;
}

/**
 * rpc_rdma_accept_timedwait: given a listening xprt,
 * waits till any connection is requested and accepts one,
 * then clones the listener.
 *
 * @param[IN] l_xprt	listening (parent) transport
 * @param[IN] abstime	time to wait
 *
 * @return 0 on success, errno value on failure
 */
static RDMAXPRT *
rpc_rdma_accept_timedwait(RDMAXPRT *l_xprt, struct timespec *abstime)
{
	struct rdma_cm_id *cm_id;
	uint32_t u;
	int rc;

	if (!l_xprt || l_xprt->state != RDMAXS_LISTENING) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] isn't listening (after bind_server)?",
			__func__, l_xprt, l_xprt->state);
		return (NULL);
	}

	/* Drain connection_requests */
	if (abstime) {
		rc = sem_timedwait(&rpc_rdma_state.c_r.q_sem, abstime);
		if (rc) {
			rc = errno;
			if (ETIMEDOUT == rc) {
				__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
					"%s() ETIMEDOUT",
					__func__);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s() sem_timedwait failed",
					__func__, strerror(rc), rc);
			}
			return (NULL);
		}
	} else {
		rc = sem_wait(&rpc_rdma_state.c_r.q_sem);
		if (rc) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() sem_wait failed",
				__func__, strerror(rc), rc);
			return (NULL);
		}
	}

	u = atomic_postinc_uint32_t(&rpc_rdma_state.c_r.q_head);
	if (u >= rpc_rdma_state.c_r.q_size) {
		u_int q_mask = rpc_rdma_state.c_r.q_size - 1;

		/* masking allows update by lock-free tasks,
		 * as long as overrun never 2 * q_size
		 */
		u &= q_mask;
		atomic_clear_uint32_t_bits(&rpc_rdma_state.c_r.q_head, ~q_mask);
	}
	cm_id = rpc_rdma_state.c_r.id_queue[u];

	/* Increase available count */
	rc = sem_post(&rpc_rdma_state.c_r.u_sem);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() sem_post failed",
			__func__, strerror(rc), rc);
		return (NULL);
	}

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() thread %lx, q %u, cm_id %p",
		__func__,
		pthread_self(),
		u,
		cm_id);

	if (!cm_id) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() missing cm_id",
			__func__);
		return (NULL);
	}

	return rpc_rdma_clone(l_xprt, cm_id);
}

RDMAXPRT *
rpc_rdma_accept_wait(RDMAXPRT *l_xprt,int msleep)
{
	struct timespec ts;

	if (msleep == 0)
		return rpc_rdma_accept_timedwait(l_xprt, NULL);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += msleep / 1000;
	ts.tv_nsec += (msleep % 1000) * NSEC_IN_SEC;
	if (ts.tv_nsec >= NSEC_IN_SEC) {
		ts.tv_nsec -= NSEC_IN_SEC;
		ts.tv_sec++;
	}

	return rpc_rdma_accept_timedwait(l_xprt, &ts);
}

/**
 * rpc_rdma_bind_client: resolve addr and route for the client and waits till it's done
 * (the route and cond_signal is done in the cm thread)
 *
 */
static int
rpc_rdma_bind_client(RDMAXPRT *xprt)
{
	struct rdma_addrinfo hints;
	struct rdma_addrinfo *res;
	int rc;

	mutex_lock(&xprt->cm_lock);

	do {
		memset(&hints, 0, sizeof(hints));
		hints.ai_port_space = xprt->conn_type;

		rc = rdma_getaddrinfo(xprt->xa->node, xprt->xa->port,
					&hints, &res);
		if (rc) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p[%u] rdma_getaddrinfo: %s (%d)",
				__func__, xprt, xprt->state, strerror(rc), rc);
			break;
		}

		rc = rdma_resolve_addr(xprt->cm_id, res->ai_src_addr,
					res->ai_dst_addr,
					__svc_params->idle_timeout);
		if (rc) {
			rc = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p[%u] rdma_resolve_addr failed: %s (%d)",
				__func__, xprt, xprt->state, strerror(rc), rc);
			break;
		}
		rdma_freeaddrinfo(res);

		while (xprt->state == RDMAXS_INITIAL) {
			cond_wait(&xprt->cm_cond, &xprt->cm_lock);
			__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
				"%s() %p[%u] after cond_wait",
				__func__, xprt, xprt->state);
		}

		if (xprt->state != RDMAXS_ADDR_RESOLVED) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Could not resolve addr",
				__func__);
			rc = EINVAL;
			break;
		}

		rc = rdma_resolve_route(xprt->cm_id,
					__svc_params->idle_timeout);
		if (rc) {
			xprt->state = RDMAXS_ERROR;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() %p[%u] rdma_resolve_route failed: %s (%d)",
				__func__, xprt, xprt->state, strerror(rc), rc);
			break;
		}

		while (xprt->state == RDMAXS_ADDR_RESOLVED) {
			cond_wait(&xprt->cm_cond, &xprt->cm_lock);
			__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
				"%s() %p[%u] after cond_wait",
				__func__, xprt, xprt->state);
		}

		if (xprt->state != RDMAXS_ROUTE_RESOLVED) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Could not resolve route",
				__func__);
			rc = EINVAL;
			break;
		}
	} while (0);

	mutex_unlock(&xprt->cm_lock);

	return rc;
}

/**
 * rpc_rdma_connect_finalize: tells the other side we're ready to receive stuff
 * (does the actual rdma_connect)
 * N.B. no wait for CM result. CM event thread will handle setup/teardown.
 *
 * @param[IN] xprt
 *
 * @return 0 on success, errno value on failure
 */
int
rpc_rdma_connect_finalize(RDMAXPRT *xprt)
{
	struct rdma_conn_param conn_param;
	int rc;

	if (!xprt || xprt->state != RDMAXS_ROUTE_RESOLVED) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] isn't half-connected?",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 10;
	conn_param.retry_count = 10;

	rc = rdma_connect(xprt->cm_id, &conn_param);
	if (rc) {
		rc = errno;
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rdma_connect failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
	}

	return rc;
}

/**
 * rpc_rdma_connect: connects a client to a server
 *
 * @param[INOUT] xprt	must be init first
 *
 * @return 0 on success, errno value on failure
 */
int
rpc_rdma_connect(RDMAXPRT *xprt)
{
	int rc;

	if (!xprt || xprt->state != RDMAXS_INITIAL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] must be initialized first!",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	if (xprt->server) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() only called from client side!",
			__func__);
		return EINVAL;
	}

	rc = rpc_rdma_thread_create_epoll(&rpc_rdma_state.cm_thread,
		rpc_rdma_cm_thread, xprt, &rpc_rdma_state.cm_epollfd);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] rpc_rdma_thread_create_epoll failed: %s (%d)",
			__func__, xprt, xprt->state, strerror(rc), rc);
		return rc;
	}

	rc = rpc_rdma_bind_client(xprt);
	if (rc)
		return rc;
	rc = rpc_rdma_pd_get(xprt);
	if (rc)
		return rc;
	rc = rpc_rdma_setup_stuff(xprt);
	if (rc) {
		rpc_rdma_destroy_stuff(xprt);
		return rc;
	}
	rc = rpc_rdma_setup_cbq(&xprt->cbqh,
				xprt->xa->rq_depth + xprt->xa->sq_depth,
				xprt->xa->credits);
	if (rc)
		return rc;

	return rpc_rdma_fd_add(xprt, xprt->event_channel->fd,
				rpc_rdma_state.cm_epollfd);
}

static void
rpc_rdma_unlink_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	return;
}

static void
rpc_rdma_destroy_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}
	rpc_rdma_destroy(RDMA_DR(REC_XPRT(xprt)));
}

extern mutex_t ops_lock;

static bool
/*ARGSUSED*/
rpc_rdma_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_UNREF_USER_DATA:
	    mutex_lock(&ops_lock);
	    *(svc_xprt_void_fun_t *) in = xprt->xp_ops->xp_unref_user_data;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_UNREF_USER_DATA:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_unref_user_data = *(svc_xprt_void_fun_t) in;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCGET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    *(svc_xprt_fun_t *)in = xprt->xp_ops->xp_free_user_data;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_free_user_data = *(svc_xprt_fun_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	default:
	    return (FALSE);
	}
	return (TRUE);
}

static struct xp_ops rpc_rdma_ops = {
	.xp_recv = svc_rdma_rendezvous,
	.xp_stat = svc_rendezvous_stat,
	.xp_decode = (svc_req_fun_t)abort,
	.xp_reply = (svc_req_fun_t)abort,
	.xp_checksum = NULL,		/* not used */
	.xp_unlink = rpc_rdma_unlink_it,
	.xp_unref_user_data = NULL,	/* no default */
	.xp_destroy = rpc_rdma_destroy_it,
	.xp_control = rpc_rdma_control,
	.xp_free_user_data = NULL,	/* no default */
};
