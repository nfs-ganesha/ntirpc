/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2013-2015 CohortFS, LLC.
 * Copyright (c) 2017 Red Hat, Inc.
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

#ifndef TIRPC_SVC_XPRT_H
#define TIRPC_SVC_XPRT_H

#include <rpc/svc.h>
#include <misc/portable.h>
#include <misc/rbtree_x.h>

/**
 * @file svc_xprt.h
 * @contributeur William Allen Simpson <bill@cohortfs.com>
 * @brief Service transports package
 *
 * @section DESCRIPTION
 *
 * Maintains a tree of all extant transports by fd.
 *
 *  svc_xprt_init -- init module; usually called by svc_init()
 *  svc_xprt_lookup -- find or create shared fd state
 *  svc_xprt_clear -- remove a transport
 *  svc_xprt_foreach -- scan registered transports
 *  svc_xprt_dump_xprts -- dump registered transports
 *  svc_xprt_shutdown -- clear the tree, destroy transports
 */

int svc_xprt_init(void);

typedef void (*svc_xprt_setup_t) (SVCXPRT **);

/*
 * returns with lock taken
 */
SVCXPRT *svc_xprt_lookup(int, svc_xprt_setup_t);
void svc_xprt_clear(SVCXPRT *);

typedef bool(*svc_xprt_each_func_t) (SVCXPRT *, void *);
int svc_xprt_foreach(svc_xprt_each_func_t, void *);

void svc_xprt_dump_xprts(const char *);
void svc_xprt_shutdown(void);

#endif				/* TIRPC_SVC_XPRT_H */
