/*
 * Copyright (c) 2013-2015 CohortFS, LLC.
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

/**
 * @file work_pool.h
 * @author William Allen Simpson <bill@cohortfs.com>
 * @brief Pthreads-based work queue package
 *
 * @section DESCRIPTION
 *
 * This provides simple work queues using pthreads and TAILQ primitives.
 *
 * @note    Loosely based upon previous thrdpool by
 *          Matt Benjamin <matt@cohortfs.com>
 */

#ifndef WORK_POOL_H
#define WORK_POOL_H

#include <rpc/pool_queue.h>

struct work_pool_params {
	int32_t thrd_max;
	int32_t thrd_min;
	uint32_t thr_stack_size;
};

struct work_pool_thread;

struct work_pool {
	struct poolq_head pqh;
	TAILQ_HEAD(work_pool_s, work_pool_thread) wptqh;
	char *name;
	pthread_attr_t attr;
	struct work_pool_params params;
	long timeout_ms;
	uint32_t n_threads;
	uint32_t worker_index;
};

struct work_pool_entry;

struct work_pool_thread {
	struct poolq_entry pqe;		/*** 1st ***/
	TAILQ_ENTRY(work_pool_thread) wptq;
	pthread_cond_t pqcond;

	struct work_pool *pool;
	struct work_pool_entry *work;
	char worker_name[16];
	pthread_t pt;
	uint32_t worker_index;
	bool wakeup;
};

typedef void (*work_pool_fun_t) (struct work_pool_entry *);

struct work_pool_entry {
	struct poolq_entry pqe;		/*** 1st ***/
	struct work_pool_thread *wpt;
	work_pool_fun_t fun;
	void *arg;
};

int work_pool_init(struct work_pool *, const char *, struct work_pool_params *);
int work_pool_submit(struct work_pool *, struct work_pool_entry *);
int work_pool_shutdown(struct work_pool *);

#endif				/* WORK_POOL_H */
