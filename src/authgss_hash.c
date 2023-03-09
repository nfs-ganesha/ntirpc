/*
 * Copyright (c) 2012 Linux Box Corporation.
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

/* Portions Copyright (c) 2010-2011, ** others, update */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/types.h>
#include "rpc_com.h"
#include <intrinsic.h>
#include <misc/abstract_atomic.h>
#include <intrinsic.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>
#include <rpc/gss_internal.h>
#include "svc_internal.h"

/* GSS context cache */

struct authgss_x_part {
	uint32_t gen;
	uint32_t size;
	 TAILQ_HEAD(ctx_tailq, svc_rpc_gss_data) lru_q;
};

struct authgss_hash_st {
	mutex_t lock;
	struct rbtree_x xt;
	uint32_t max_part;
	uint32_t size;
	bool initialized;
};

static struct authgss_hash_st authgss_hash_st = {
	MUTEX_INITIALIZER,	/* lock */
	{
	 0,			/* npart */
	 RBT_X_FLAG_NONE,	/* flags */
	 255,			/* cachesz */
	 NULL			/* tree */
	 },			/* xt */
	false			/* initialized */
};

static inline uint64_t
gss_ctx_hash(gss_union_ctx_id_desc *gss_ctx)
{
	/* Mash together two pointers, can overflow top bit.
	 * Our only concern here is that it be unique
	 * (or have a very rare collision rate).
	 */
	return ((uint64_t)(uintptr_t)gss_ctx->mech_type +
		(uint64_t)(uintptr_t)gss_ctx->internal_ctx_id);
}

static int
svc_rpc_gss_cmpf(const struct opr_rbtree_node *lhs,
		 const struct opr_rbtree_node *rhs)
{
	struct svc_rpc_gss_data *lk, *rk;
	gss_union_ctx_id_desc *gss_ctx_lk, *gss_ctx_rk;
	uint64_t mech_type_lk, internal_ctx_id_lk;
	uint64_t mech_type_rk, internal_ctx_id_rk;


	lk = opr_containerof(lhs, struct svc_rpc_gss_data, node_k);
	rk = opr_containerof(rhs, struct svc_rpc_gss_data, node_k);
	gss_ctx_lk = (gss_union_ctx_id_desc *) (lk->ctx);
	gss_ctx_rk = (gss_union_ctx_id_desc *) (rk->ctx);
	mech_type_lk = (uint64_t)(uintptr_t)gss_ctx_lk->mech_type;
	mech_type_rk = (uint64_t)(uintptr_t)gss_ctx_rk->mech_type;
	internal_ctx_id_lk = (uint64_t)(uintptr_t)gss_ctx_lk->internal_ctx_id;
	internal_ctx_id_rk = (uint64_t)(uintptr_t)gss_ctx_rk->internal_ctx_id;

	/* Move left OR right based on mech type */
	if (mech_type_lk < mech_type_rk)
		return -1;

	if (mech_type_lk > mech_type_rk)
		return 1;

	/* If mech_type are equal, move based on internal_ctx_id */
	if (internal_ctx_id_lk < internal_ctx_id_rk)
		return -1;

	if (internal_ctx_id_lk > internal_ctx_id_rk)
		return 1;

	/* If we have reached here, both mech_type and id are equal */
	return 0;
}

static void
authgss_hash_init(void)
{
	int ix, code = 0;

	mutex_lock(&authgss_hash_st.lock);

	if (authgss_hash_st.initialized) {
		mutex_unlock(&authgss_hash_st.lock);
		return;
	}

	code =
	    rbtx_init(&authgss_hash_st.xt, svc_rpc_gss_cmpf,
		      __svc_params->gss.ctx_hash_partitions,
		      RBT_X_FLAG_ALLOC | RBT_X_FLAG_CACHE_RT);
	if (code)
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s: rbtx_init failed",
			__func__);

	/* init read-through cache */
	for (ix = 0; ix < __svc_params->gss.ctx_hash_partitions; ++ix) {
		struct rbtree_x_part *xp = &(authgss_hash_st.xt.tree[ix]);
		struct authgss_x_part *axp;
		xp->cache =
		    mem_calloc(authgss_hash_st.xt.cachesz,
			       sizeof(struct opr_rbtree_node *));

		/* partition ctx LRU */
		axp = (struct authgss_x_part *)mem_zalloc(sizeof(*axp));
		TAILQ_INIT(&axp->lru_q);
		xp->u1 = axp;
	}

	authgss_hash_st.size = 0;
	authgss_hash_st.max_part =
	    __svc_params->gss.max_ctx / authgss_hash_st.xt.npart;
	authgss_hash_st.initialized = true;

	mutex_unlock(&authgss_hash_st.lock);
}

struct svc_rpc_gss_data *
authgss_ctx_hash_get(struct rpc_gss_cred *gc)
{
	struct svc_rpc_gss_data gk, *gd = NULL;
	gss_union_ctx_id_desc *gss_ctx;
	struct opr_rbtree_node *ngd;
	struct authgss_x_part *axp;
	struct rbtree_x_part *t;

	authgss_hash_init();

	gss_ctx = (gss_union_ctx_id_desc *) (gc->gc_ctx.value);
	gk.hk.k = gss_ctx_hash(gss_ctx);
	gk.ctx = (gc->gc_ctx.value);

	t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gk.hk.k);
	mutex_lock(&t->mtx);
	ngd =
	    rbtree_x_cached_lookup(&authgss_hash_st.xt, t, &gk.node_k, gk.hk.k);
	if (ngd) {
		gd = opr_containerof(ngd, struct svc_rpc_gss_data, node_k);
		/* lru adjust */
		axp = (struct authgss_x_part *)t->u1;
		TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
		TAILQ_INSERT_TAIL(&axp->lru_q, gd, lru_q);
		++(axp->gen);
		(void)atomic_inc_uint32_t(&gd->refcnt);
		(void)atomic_inc_uint32_t(&gd->gen);
	}
	mutex_unlock(&t->mtx);

	return (gd);
}

bool
authgss_ctx_hash_set(struct svc_rpc_gss_data *gd)
{
	struct rbtree_x_part *t;
	struct authgss_x_part *axp;
	gss_union_ctx_id_desc *gss_ctx;
	bool rslt;

	authgss_hash_init();

	gss_ctx = (gss_union_ctx_id_desc *) (gd->ctx);
	gd->hk.k = gss_ctx_hash(gss_ctx);

	(void)atomic_inc_uint32_t(&gd->refcnt);
	t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gd->hk.k);
	mutex_lock(&t->mtx);
	rslt =
	    rbtree_x_cached_insert(&authgss_hash_st.xt, t, &gd->node_k,
				   gd->hk.k);
	/* lru */
	axp = (struct authgss_x_part *)t->u1;
	TAILQ_INSERT_TAIL(&axp->lru_q, gd, lru_q);
	++(axp->size);
	mutex_unlock(&t->mtx);

	/* global size */
	(void)atomic_inc_uint32_t(&authgss_hash_st.size);

	return (rslt);
}

bool
authgss_ctx_hash_del(struct svc_rpc_gss_data *gd)
{
	struct rbtree_x_part *t;
	struct authgss_x_part *axp;

	authgss_hash_init();

	t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gd->hk.k);
	mutex_lock(&t->mtx);

	/* Another thread could have removed the entry from the hash.
	 * We use its presence in the lru list to detect this. @todo:
	 * can rbtree_x_cached_remove return a value indicating if such
	 * an entry exists in the hash or not? Also, should we deal with
	 * multiple gd's pointing to same context getting inserted into
	 * the hash as well?
	 */
	if (!TAILQ_IS_ENQUEUED(gd, lru_q)) {
		mutex_unlock(&t->mtx);
		return false;
	}

	rbtree_x_cached_remove(&authgss_hash_st.xt, t, &gd->node_k, gd->hk.k);
	axp = (struct authgss_x_part *)t->u1;
	TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
	TAILQ_INIT_ENTRY(gd, lru_q);
	--(axp->size);
	mutex_unlock(&t->mtx);

	/* global size */
	(void)atomic_dec_uint32_t(&authgss_hash_st.size);

	/* release gd */
	unref_svc_rpc_gss_data(gd);

	return (true);
}

static inline bool
authgss_ctx_expired(struct svc_rpc_gss_data *gd)
{
	OM_uint32 maj_stat, min_stat;
	maj_stat =
	    gss_inquire_context(&min_stat, gd->ctx, NULL, NULL, NULL, NULL,
				NULL, NULL, NULL);
	return (maj_stat == GSS_S_CONTEXT_EXPIRED);
}

static uint32_t idle_next;

#define IDLE_NEXT() \
	(atomic_inc_uint32_t(&(idle_next)) % authgss_hash_st.xt.npart)

void authgss_ctx_gc_idle(void)
{
	struct rbtree_x_part *xp;
	struct authgss_x_part *axp;
	struct svc_rpc_gss_data *gd;
	int ix, cnt, part;

	authgss_hash_init();

	for (ix = 0, cnt = 0, part = IDLE_NEXT();
	     ((ix < authgss_hash_st.xt.npart) &&
		     (cnt < __svc_params->gss.max_gc));
	     ++ix, part = IDLE_NEXT()) {
		xp = &(authgss_hash_st.xt.tree[part]);
		axp = (struct authgss_x_part *)xp->u1;
		mutex_lock(&xp->mtx);
 again:
		gd = TAILQ_FIRST(&axp->lru_q);
		if (!gd)
			goto next_t;

		/* Remove the least-recently-used entry in this hash
		 * partition iff it is expired, or the partition size
		 * limit is exceeded */
		if (unlikely((axp->size > authgss_hash_st.max_part)
				|| (authgss_ctx_expired(gd)))) {

			/* remove entry */
			rbtree_x_cached_remove(&authgss_hash_st.xt, xp,
					       &gd->node_k, gd->hk.k);
			TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
			TAILQ_INIT_ENTRY(gd, lru_q);
			--(axp->size);
			(void)atomic_dec_uint32_t(&authgss_hash_st.size);

			/* drop sentinel ref (may free gd) */
			unref_svc_rpc_gss_data(gd);

			if (++cnt < __svc_params->gss.max_gc)
				goto again;
		}
 next_t:
		mutex_unlock(&xp->mtx);
	}

	/* perturb by 1 */
	(void)IDLE_NEXT();
}
