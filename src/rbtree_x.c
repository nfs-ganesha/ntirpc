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

#include "config.h"

#include <sys/types.h>
#if !defined(_WIN32)
#include <sys/poll.h>
#include <err.h>
#endif
#include <stdint.h>
#include <errno.h>
#include <rpc/types.h>
#include <reentrant.h>
#include <misc/rbtree_x.h>

#define RBTX_REC_MAXPART 23

int rbtx_init(struct rbtree_x *xt, opr_rbtree_cmpf_t cmpf, uint32_t npart,
	      uint32_t flags)
{
	int ix, code = 0;
	pthread_rwlockattr_t rwlock_attr;
	struct rbtree_x_part *t;

	xt->flags = flags;

	if ((npart > RBTX_REC_MAXPART) || (npart % 2 == 0)) {
		__warnx(TIRPC_DEBUG_FLAG_RBTREE,
			"rbtx_init: value %d is an unlikely value for npart "
			"(suggest a small prime)", npart);
	}

	if (flags & RBT_X_FLAG_ALLOC)
		xt->tree = mem_alloc(npart * sizeof(struct rbtree_x_part));

	/* prior versions of Linux tirpc are subject to default prefer-reader
	 * behavior (so have potential for writer starvation) */
	rwlockattr_init(&rwlock_attr);
#ifdef GLIBC
	pthread_rwlockattr_setkind_np(
		&rwlock_attr,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

	xt->npart = npart;

	for (ix = 0; ix < npart; ++ix) {
		t = &(xt->tree[ix]);
		mutex_init(&t->mtx, NULL);
		rwlock_init(&t->lock, &rwlock_attr);
		pthread_spin_init(&t->sp, PTHREAD_PROCESS_PRIVATE);
		opr_rbtree_init(&t->t, cmpf /* may be NULL */);
	}

	return (code);
}

void rbtx_cleanup(struct rbtree_x *xt)
{
    int ix;
    struct rbtree_x_part *t;
    uint32_t npart = xt->npart;
    uint32_t flags = xt->flags;

    if (xt->tree) {
        for (ix = 0; ix < xt->npart; ++ix) {
            t = &(xt->tree[ix]);
            mutex_destroy(&t->mtx);
            pthread_rwlock_destroy(&t->lock);
            pthread_spin_destroy(&t->sp);
        }
        if (flags & RBT_X_FLAG_ALLOC)
            mem_free(xt->tree, npart * sizeof(struct rbtree_x_part));
    }
}
