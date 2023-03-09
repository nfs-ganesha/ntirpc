/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2013-2018 Red Hat, Inc. and/or its affiliates.
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
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include "rpc_com.h"
#include "svc_internal.h"
#include "svc_xprt.h"

/**
 * @file svc_xprt.c
 * @contributeur William Allen Simpson <bill@cohortfs.com>
 * @brief Service transports package
 *
 * @section DESCRIPTION
 *
 * Maintains a tree of all extant transports by fd.
 *
 * Each SVCXPRT has its own instance, however, so operations to
 * close and delete (for example) given an existing xprt handle
 * are O(1) without any ordered or hashed representation.
 *
 * @note currently static sizes
 *	partitions should be largish prime, relative to connections.
 *	no cache slots, as rpc_dplx_rec has fd_node for direct access.
 */

#define SVC_XPRT_PARTITIONS 193

static bool initialized;

struct svc_xprt_fd {
	mutex_t lock;
	struct rbtree_x xt;
	uint32_t connections;
};

static struct svc_xprt_fd svc_xprt_fd = {
	MUTEX_INITIALIZER /* svc_xprt_lock */ ,
	{
	 SVC_XPRT_PARTITIONS,	/* npart */
	 RBT_X_FLAG_NONE,	/* flags */
	 0,			/* cachesz */
	 NULL			/* tree */
	}			/* xt */
};

static inline int
svc_xprt_fd_cmpf(const struct opr_rbtree_node *lhs,
		 const struct opr_rbtree_node *rhs)
{
	struct rpc_dplx_rec *lk, *rk;

	lk = opr_containerof(lhs, struct rpc_dplx_rec, fd_node);
	rk = opr_containerof(rhs, struct rpc_dplx_rec, fd_node);

	if (lk->xprt.xp_fd < rk->xprt.xp_fd)
		return (-1);

	if (lk->xprt.xp_fd == rk->xprt.xp_fd)
		return (0);

	return (1);
}

int
svc_xprt_init(void)
{
	int code = 0;

	mutex_lock(&svc_xprt_fd.lock);

	if (initialized)
		goto unlock;

	/* one of advantages of this RBT is convenience of external
	 * iteration, we'll go to that shortly */
	code =
	    rbtx_init(&svc_xprt_fd.xt, svc_xprt_fd_cmpf /* NULL (inline) */ ,
		      SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
	if (code)
		__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
			"svc_xprt_init: rbtx_init failed");

	initialized = true;

 unlock:
	mutex_unlock(&svc_xprt_fd.lock);
	return (code);
}

static inline bool
svc_xprt_init_failure(void)
{
	if (initialized)
		return (false);
	return (svc_xprt_init() != 0);
}

/*
 * On success, returns with RPC_DPLX_LOCKED
 */
SVCXPRT *
svc_xprt_lookup(int fd, svc_xprt_setup_t setup)
{
	struct rpc_dplx_rec sk;
	struct rpc_dplx_rec *rec;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *nv;
	SVCXPRT *xprt = NULL;
	uint16_t xp_flags;

	if (svc_xprt_init_failure())
		return (NULL);

	sk.xprt.xp_fd = fd;
	t = rbtx_partition_of_scalar(&svc_xprt_fd.xt, fd);

	rwlock_rdlock(&t->lock);
	nv = opr_rbtree_lookup(&t->t, &sk.fd_node);
	if (!nv) {
		rwlock_unlock(&t->lock);
		if (!setup)
			return (NULL);

		rwlock_wrlock(&t->lock);
		nv = opr_rbtree_lookup(&t->t, &sk.fd_node);
		if (!nv) {
			if (atomic_inc_uint32_t(&svc_xprt_fd.connections)
			    > __svc_params->max_connections) {
				atomic_dec_uint32_t(&svc_xprt_fd.connections);
				rwlock_unlock(&t->lock);
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: fd %d max_connections %u exceeded\n",
					__func__, fd,
					__svc_params->max_connections);
				return (NULL);
			}
			(*setup)(&xprt); /* zalloc, xp_refcnt = 1 */
			xprt->xp_fd = fd;
			xprt->xp_fd_send = -1;
			xprt->xp_flags = SVC_XPRT_FLAG_INITIAL;

			/* Get ref for caller */
			SVC_REF(xprt, SVC_REF_FLAG_NONE);

			rec = REC_XPRT(xprt);
			rpc_dplx_rli(rec);
			if (opr_rbtree_insert(&t->t, &rec->fd_node)) {
				/* cant happen */
				rpc_dplx_rui(rec);
				__warnx(TIRPC_DEBUG_FLAG_LOCK,
					"%s: collision inserting in locked rbtree partition",
					__func__);
				(*setup)(&xprt);	/* free, sets NULL */
				atomic_dec_uint32_t(&svc_xprt_fd.connections);
			}
			rwlock_unlock(&t->lock);
			return (xprt);
		}
		/* raced, fallthru */
	}
	rec = opr_containerof(nv, struct rpc_dplx_rec, fd_node);
	xprt = &rec->xprt;

	/* lookup reference before unlock ensures shutdown cannot release */
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	rwlock_unlock(&t->lock);

	/* unlocked window here permits shutdown to destroy without release;
	 * then duplex lock is required to match allocation return,
	 * ensuring SVC_XPRT_FLAG_INITIAL cleared in this thread only
	 * (obviating extra atomic fetch).
	 */
	rpc_dplx_rli(rec);
	xp_flags = atomic_clear_uint16_t_bits(&xprt->xp_flags,
					      SVC_XPRT_FLAG_INITIAL);
	rpc_dplx_rui(rec);

	if (!(xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
		/* do not return destroyed xprts */
		return (xprt);
	}

	/* unlock before release permits releasing here after destroy */
	SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
	return (NULL);
}

/**
 * Clear an xprt
 *
 * @note Locking
 * - xprt is locked
 *   returned locked
 */
void
svc_xprt_clear(SVCXPRT *xprt)
{
	struct rbtree_x_part *t;

	if (svc_xprt_init_failure())
		return;

	/* xprt lock ensures only one active thread here */
	if (opr_rbtree_node_valid(&REC_XPRT(xprt)->fd_node)) {
		t = rbtx_partition_of_scalar(&svc_xprt_fd.xt, xprt->xp_fd);

		/* if another thread passes test during svc_xprt_shutdown(),
		 * this lock (and generation test) prevents repeats.
		 */
		atomic_dec_uint32_t(&svc_xprt_fd.connections);
		rwlock_wrlock(&t->lock);
		opr_rbtree_remove(&t->t, &REC_XPRT(xprt)->fd_node);
		rwlock_unlock(&t->lock);
	}
}

int
svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg)
{
	struct rpc_dplx_rec sk;
	struct rpc_dplx_rec *rec;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *n;
	uint64_t tgen;
	int p_ix;
	int x_ix;
	int restarts;

	if (svc_xprt_init_failure())
		return (-1);

	/* concurrent, restartable iteration over t */
	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];
		restarts = 0;
		/* TI-RPC __svc_clean_idle held global svc_fd_lock
		 * exclusive locked for a full scan of the legacy svc_xprts
		 * array.  We avoid this via tree partitioning and by
		 * operating mostly unlocked. */
 restart:
		if (++restarts > 5)
			return (1);

		/* start with rlock */
		rwlock_rdlock(&t->lock);	/* t RLOCKED */
		tgen = t->t.gen;
		x_ix = 0;
		n = opr_rbtree_first(&t->t);
		while (n != NULL) {
			++x_ix;	/* diagnostic, index into logical srec
				 * sequence */
			rec = opr_containerof(n, struct rpc_dplx_rec, fd_node);
			sk.xprt.xp_fd = rec->xprt.xp_fd;

			/* call each_func with t !LOCKED */
			rwlock_unlock(&t->lock);

			/* restart if each_f disposed xprt */
			if (each_f(&rec->xprt, arg))
				goto restart;

			/* validate */
			rwlock_rdlock(&t->lock);

			if (tgen != t->t.gen) {
				n = opr_rbtree_lookup(&t->t, &sk.fd_node);
				if (!n) {
					/* invalidated, try harder */
					rwlock_unlock(&t->lock);
							/* t !LOCKED */
					goto restart;
				}
			}
			n = opr_rbtree_next(n);
		}		/* curr partition */
		rwlock_unlock(&t->lock); /* t !LOCKED */
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */

	return (0);
}

void
svc_xprt_dump_xprts(const char *tag)
{
	struct rbtree_x_part *t = NULL;
	struct opr_rbtree_node *n;
	struct rpc_dplx_rec *rec;
	int p_ix;

	if (!initialized)
		goto out;

	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];
		rwlock_rdlock(&t->lock);	/* t RLOCKED */
		__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
			"xprts at %s: tree %d size %d", tag, p_ix, t->t.size);
		n = opr_rbtree_first(&t->t);
		while (n != NULL) {
			rec = opr_containerof(n, struct rpc_dplx_rec, fd_node);
			__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
				"xprts at %s: %p xp_fd %d",
				tag, &rec->xprt, rec->xprt.xp_fd);
			n = opr_rbtree_next(n);
		}		/* curr partition */
		rwlock_unlock(&t->lock);	/* t !LOCKED */
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */
 out:
	return;
}

void
svc_xprt_shutdown(void)
{
	struct rbtree_x_part *t;
	struct opr_rbtree_node *n;
	struct rpc_dplx_rec *rec;
	int p_ix;

	if (!initialized)
		return;

	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];

		rwlock_wrlock(&t->lock);	/* t WLOCKED */
		while ((n = opr_rbtree_first(&t->t))) {
			rec = opr_containerof(n, struct rpc_dplx_rec, fd_node);

			/* prevent repeats, see svc_xprt_clear() */
			opr_rbtree_remove(&t->t, &rec->fd_node);

			/* fd_node is counted by initial xp_refcnt = 1,
			 * SVC_DESTROY() decrements that reference.
			 */
			rwlock_unlock(&t->lock);
			SVC_DESTROY(&rec->xprt);
			rwlock_wrlock(&t->lock);
		}		/* curr partition */
		rwlock_unlock(&t->lock);	/* t !LOCKED */
		rwlock_destroy(&t->lock);
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */

	/* free tree */
	mem_free(svc_xprt_fd.xt.tree,
		 SVC_XPRT_PARTITIONS * sizeof(struct rbtree_x_part));
}

void
svc_xprt_trace(SVCXPRT *xprt, const char *func, const char *tag, const int line)
{
	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p fd %d fd_send %d xp_refcnt %" PRId32
		" af %u port %u @%s:%d",
		func, xprt, xprt->xp_fd, xprt->xp_fd_send, xprt->xp_refcnt,
		xprt->xp_remote.ss.ss_family,
		__rpc_address_port(&xprt->xp_remote),
		tag, line);
}
