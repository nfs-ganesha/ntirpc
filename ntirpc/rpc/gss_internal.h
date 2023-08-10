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

#ifndef GSS_INTERNAL_H
#define GSS_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <rpc/rpc.h>
#include <misc/rbtree_x.h>
#include <misc/queue.h>
#include <misc/abstract_atomic.h>
#include <intrinsic.h>

#ifdef HAVE_HEIMDAL
#include <gssapi.h>
#define gss_nt_service_name GSS_C_NT_HOSTBASED_SERVICE
#else
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_generic.h>
#include <gssapi/gssapi_ext.h>
#endif
#include <rpc/auth_gss.h>

extern SVCAUTH svc_auth_none;

#define SVCAUTH_PRIVATE(auth) \
	((struct svc_rpc_gss_data *)(auth)->svc_ah_private)

/*
 * from mit-krb5-1.2.1 mechglue/mglueP.h:
 * Array of context IDs typed by mechanism OID
 */
struct gss_union_ctx_id {
	gss_OID mech_type;
	gss_ctx_id_t internal_ctx_id;
};

typedef struct gss_union_ctx_id gss_union_ctx_id_desc;
typedef gss_union_ctx_id_desc *gss_union_ctx_id_t;

#define SVC_RPC_GSS_FLAG_NONE    0x0000
#define SVC_RPC_GSS_FLAG_MSPAC   0x0001

/* This defines the sequence window for SVC-GSS implementation.
 * Due to the way we store the sequence window mask, this number MUST always
 * be a multiple of CHAR_BIT.
 */
#define SVC_GSS_SEQ_WIN 256

/* We additionally maintain an internal sequence window, that is double the
 * size of the sequence window that is published to the client. This is done
 * because, due to the asynchronous (= unordered) nature of request processing
 * at the server, it is possible that in a window of N, assuming sequence
 * numbers from 1 to N, the server processes and responds to the request with
 * (N+1) sequence number before the request with sequence number `1` (that is,
 * the server responds to `N`th request, then the client sends `N+1`th request,
 * which also gets processed before the older request with sequence number `1`)
 * In such a scenario, the request with sequence number `1` will fall outside
 * the sequence window, and will be dropped. In order to reduce the chances of
 * such a situation, we maintain a larger sequence window on the server
 */
#define SVC_GSS_SEQ_WIN_INTERNAL (SVC_GSS_SEQ_WIN * 2)

struct svc_rpc_gss_data {
	struct opr_rbtree_node node_k;
	TAILQ_ENTRY(svc_rpc_gss_data) lru_q;
	mutex_t lock;
	uint32_t flags;
	uint32_t refcnt;
	uint32_t gen;
	struct {
		uint32_t k;
	} hk;
	bool established;
	gss_ctx_id_t ctx;	/* context id */
	struct rpc_gss_sec sec;	/* security triple */
	gss_buffer_desc cname;	/* GSS client name */
	u_int seqlast;
	uint8_t seqmask[SVC_GSS_SEQ_WIN_INTERNAL / CHAR_BIT];
	gss_name_t client_name;
	gss_buffer_desc checksum;
	struct {
		/* extended krb5 ticket ("pac") data */
		gss_buffer_desc ms_pac;
	} pac;
	SVCAUTH *auth;
	uint32_t endtime;
};

bool svcauth_gss_destroy(SVCAUTH *auth);

static inline struct
svc_rpc_gss_data *alloc_svc_rpc_gss_data(void)
{
	struct svc_rpc_gss_data *gd =
		(struct svc_rpc_gss_data *)
		mem_zalloc(sizeof(struct svc_rpc_gss_data));

	mutex_init(&gd->lock, NULL);
	TAILQ_INIT_ENTRY(gd, lru_q);
	gd->refcnt = 1;
	return (gd);
}

static inline void
unref_svc_rpc_gss_data(struct svc_rpc_gss_data *gd)
{
	mutex_lock(&gd->lock);

	/* if refcnt is 0, gd is not reachable */
	if (unlikely(atomic_dec_uint32_t(&gd->refcnt) == 0)) {
		svcauth_gss_destroy(gd->auth);
		/* gd was unlocked and freed. */
		return;
	}

	mutex_unlock(&gd->lock);
}

struct svc_rpc_gss_data *authgss_ctx_hash_get(struct rpc_gss_cred *gc);
bool authgss_ctx_hash_set(struct svc_rpc_gss_data *gd);
bool authgss_ctx_hash_del(struct svc_rpc_gss_data *gd);
void authgss_ctx_hash_clear(void);

bool svcauth_gss_acquire_cred(void);
bool svcauth_gss_release_cred(void);
bool svcauth_gss_import_name(char *service);
bool svcauth_gss_set_svc_name(gss_name_t name);
void svcauth_gss_set_status(bool status_enabled);

#endif				/* GSS_INTERNAL_H */
