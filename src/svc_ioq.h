/*
 * Copyright (c) 2013 Linux Box Corporation.
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

#ifndef SVC_IOQ_H
#define SVC_IOQ_H

#include <rpc/svc.h>
#include <rpc/xdr_ioq.h>

/* Named list type for ZC harvest — avoids anonymous-struct warnings. */
TAILQ_HEAD(zc_harvest_list, poolq_entry);
typedef struct zc_harvest_list zc_harvest_list_t;

void svc_ioq_write(SVCXPRT *);
void svc_ioq_write_now(SVCXPRT *, struct xdr_ioq *);
void svc_ioq_write_submit(SVCXPRT *, struct xdr_ioq *);
/* Non-blocking MSG_ZEROCOPY errqueue drain; safe from epoll/recv/send paths */
void svc_ioq_zc_drain(SVCXPRT *);
/* Split drain: collect completions (before postclear), release after. */
int  svc_ioq_zc_drain_collect(SVCXPRT *, zc_harvest_list_t *);
void svc_ioq_zc_drain_release(SVCXPRT *, zc_harvest_list_t *, int);
/* Drop any xioqs still waiting for ZC completion (xprt teardown) */
void svc_ioq_zc_release_all(SVCXPRT *);

#endif				/* SVC_IOQ_H */
