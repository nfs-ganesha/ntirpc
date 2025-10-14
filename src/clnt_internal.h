/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
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

#ifndef _CLNT_INTERNAL_H
#define _CLNT_INTERNAL_H

#include <rpc/clnt.h>
#include "rpc_dplx_internal.h"

#define MCALL_MSG_SIZE 24

struct cx_data {
	struct rpc_client cx_c;		/**< Transport Independent handle */
	struct rpc_dplx_rec *cx_rec;	/* unified sync */

	char cx_mcallc[MCALL_MSG_SIZE];	/* marshalled callmsg */
	u_int cx_mpos;		/* pos after marshal */
};
#define CX_DATA(p) (opr_containerof((p), struct cx_data, cx_c))

/* compartmentalize a bit */
static inline void
clnt_data_init(struct cx_data *cx)
{
	mutex_init(&cx->cx_c.cl_lock, NULL);
	cx->cx_c.cl_refcnt = 1;
}

static inline void
clnt_data_destroy(struct cx_data *cx)
{
	mutex_destroy(&cx->cx_c.cl_lock);

	/* note seemingly pointers to constant ""? */
	if (cx->cx_c.cl_netid && cx->cx_c.cl_netid[0])
		mem_free(cx->cx_c.cl_netid, strlen(cx->cx_c.cl_netid) + 1);
	if (cx->cx_c.cl_tp && cx->cx_c.cl_tp[0])
		mem_free(cx->cx_c.cl_tp, strlen(cx->cx_c.cl_tp) + 1);
}

/* in svc_rqst.c */
void svc_rqst_expire_insert(struct clnt_req *);
void svc_rqst_expire_remove(struct clnt_req *);

#endif				/* _CLNT_INTERNAL_H */
