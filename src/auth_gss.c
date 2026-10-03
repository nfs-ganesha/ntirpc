/*
  auth_gss.c

  RPCSEC_GSS client routines.

  Copyright (c) 2000 The Regents of the University of Michigan.
  All rights reserved.

  Copyright (c) 2000 Dug Song <dugsong@UMICH.EDU>.
  All rights reserved, all wrongs reversed.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  3. Neither the name of the University nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <rpc/types.h>
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/auth_gss.h>
#include <rpc/clnt.h>
#include <netinet/in.h>
#include <gssapi/gssapi.h>

static void authgss_nextverf(AUTH *auth);
static bool authgss_marshal(AUTH *auth, XDR *xdrs);
static bool authgss_refresh(AUTH *auth, void *arg);
static bool authgss_validate(AUTH *auth, struct opaque_auth *verf);
static void authgss_destroy(AUTH *auth);
static void authgss_destroy_context(AUTH *auth);
static bool authgss_wrap(AUTH *auth, XDR *xdrs, xdrproc_t xdr_func,
			 void *xdr_ptr);
static bool authgss_unwrap(AUTH *auth, XDR *xdrs, xdrproc_t xdr_func,
			   void *xdr_ptr);

/*
 * from mit-krb5-1.2.1 mechglue/mglueP.h:
 * Array of context IDs typed by mechanism OID
 */
typedef struct gss_union_ctx_id_t {
	gss_OID mech_type;
	gss_ctx_id_t internal_ctx_id;
} gss_union_ctx_id_desc, *gss_union_ctx_id_t;

static struct auth_ops authgss_ops = {
	authgss_nextverf,
	authgss_marshal,
	authgss_validate,
	authgss_refresh,
	authgss_destroy,
	authgss_wrap,
	authgss_unwrap
};

#ifdef DEBUG

/* useful as i add more mechanisms */
void
print_rpc_gss_sec(struct rpc_gss_sec *ptr)
{
	int i;
	char *p;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "rpc_gss_sec:");
	if (ptr->mech == NULL)
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "NULL gss_OID mech");
	else {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "     mechanism_OID: {");
		p = (char *)ptr->mech->elements;
		for (i = 0; i < ptr->mech->length; i++)
			/* First byte of OIDs encoded to save a byte */
			if (i == 0) {
				int first, second;
				if (*p < 40) {
					first = 0;
					second = *p;
				} else if (40 <= *p && *p < 80) {
					first = 1;
					second = *p - 40;
				} else if (80 <= *p && *p < 127) {
					first = 2;
					second = *p - 80;
				} else {
					/* Invalid value! */
					first = -1;
					second = -1;
				}
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, " %u %u",
					first, second);
				p++;
			} else {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, " %u",
					(unsigned char)*p++);
			}
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, " }");
	}
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "     qop: %d", ptr->qop);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "     service: %d", ptr->svc);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "     cred: %p", ptr->cred);
}
#endif /*DEBUG*/

struct rpc_gss_data {
	AUTH gd_auth;
	mutex_t lock;
	gss_buffer_desc gc_wire_verf;	/* save GSS_S_COMPLETE NULL RPC verfier
					 * to process at end of context
					 * negotiation*/
	CLIENT *clnt;		/* client handle */
	gss_name_t name;	/* service name */
	struct rpc_gss_sec sec;	/* security tuple */
	gss_ctx_id_t ctx;	/* context id */
	struct rpc_gss_cred gc;	/* client credentials */
	u_int win;		/* sequence window */
	bool established;	/* context established */
};
#define AUTH_PRIVATE(p) (opr_containerof((p), struct rpc_gss_data, gd_auth))

/* retry timeout default to the moon and back */
static const struct timespec to = { 3, 0 };

AUTH *
authgss_ncreate(CLIENT *clnt, gss_name_t name, struct rpc_gss_sec *sec)
{
	struct rpc_gss_data *gd = mem_zalloc(sizeof(*gd));
	AUTH *auth = &gd->gd_auth;
	OM_uint32 maj_stat;
	OM_uint32 min_stat = 0;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	auth->ah_ops = &authgss_ops;
	auth->ah_private = NULL;
	auth->ah_error.re_status = RPC_SUCCESS;

	/* XXX move to ctor */
	mutex_init(&gd->lock, NULL);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() name is %p", __func__, name);

	if (name != GSS_C_NO_NAME) {
		maj_stat = gss_duplicate_name(&min_stat, name, &gd->name);
		if (maj_stat != GSS_S_COMPLETE) {
			auth->ah_error.re_status = RPC_SYSTEMERROR;
			auth->ah_error.re_errno = EINVAL;
			return (auth);
		}
	} else
		gd->name = name;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() gd->name is %p",
		__func__, gd->name);

	gd->clnt = clnt;
	gd->ctx = GSS_C_NO_CONTEXT;
	gd->sec = *sec;

	gd->gc.gc_v = RPCSEC_GSS_VERSION;
	gd->gc.gc_proc = RPCSEC_GSS_INIT;
	gd->gc.gc_svc = gd->sec.svc;

	if (!authgss_refresh(auth, NULL))
		auth = NULL;
	else
		auth_get(auth);	/* Reference for caller */

	return (auth);
}

AUTH *
authgss_ncreate_default(CLIENT *clnt, char *service,
			      struct rpc_gss_sec *sec)
{
	AUTH *auth;
	OM_uint32 maj_stat = 0, min_stat = 0;
	gss_buffer_desc sname;
	gss_name_t name = GSS_C_NO_NAME;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	sname.value = service;
	sname.length = strlen(service);

	maj_stat =
	    gss_import_name(&min_stat, &sname,
			    (gss_OID) GSS_C_NT_HOSTBASED_SERVICE, &name);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_import_name", maj_stat, min_stat);
		auth = authnone_ncreate_dummy();
		auth->ah_error.re_status = RPC_AUTHERROR;
		return (auth);
	}

	auth = authgss_ncreate(clnt, name, sec);

	if (name != GSS_C_NO_NAME) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() freeing name %p", __func__, name);
		gss_release_name(&min_stat, &name);
	}

	return (auth);
}

bool
authgss_get_private_data(AUTH *auth, struct authgss_private_data *pd)
{
	struct rpc_gss_data *gd;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (!auth || !pd)
		return (false);

	gd = AUTH_PRIVATE(auth);

	if (!gd || !gd->established)
		return (false);

	pd->pd_ctx = gd->ctx;
	pd->pd_ctx_hndl = gd->gc.gc_ctx;
	pd->pd_seq_win = gd->win;

	return (true);
}

static void
authgss_nextverf(AUTH *auth)
{
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);
	/* no action necessary */
}

static bool
authgss_marshal(AUTH *auth, XDR *xdrs)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);
	XDR tmpxdrs;
	char tmp[MAX_AUTH_BYTES];
	gss_buffer_desc rpcbuf, checksum;
	OM_uint32 maj_stat, min_stat;
	bool xdr_stat;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (gd->established) {
		/* XXX */
		mutex_lock(&gd->lock);
		gd->gc.gc_seq++;
		mutex_unlock(&gd->lock);
	}

	xdrmem_create(&tmpxdrs, tmp, sizeof(tmp), XDR_ENCODE);

	if (!xdr_rpc_gss_cred(&tmpxdrs, &gd->gc)) {
		XDR_DESTROY(&tmpxdrs);
		return (false);
	}
	auth->ah_cred.oa_flavor = RPCSEC_GSS;
	auth->ah_cred.oa_length = XDR_GETPOS(&tmpxdrs);
	if (auth->ah_cred.oa_length > MAX_AUTH_BYTES) {
		XDR_DESTROY(&tmpxdrs);
		return (false);
	}
	memcpy(auth->ah_cred.oa_body, tmp, auth->ah_cred.oa_length);

	XDR_DESTROY(&tmpxdrs);

	if (!xdr_opaque_auth_encode(xdrs, &auth->ah_cred))
		return (false);

	if (gd->gc.gc_proc == RPCSEC_GSS_INIT
	    || gd->gc.gc_proc == RPCSEC_GSS_CONTINUE_INIT) {
		return (xdr_opaque_auth_encode(xdrs, &_null_auth));
	}
	/* Checksum serialized RPC header, up to and including credential. */
	rpcbuf.length = XDR_GETPOS(xdrs);
	XDR_SETPOS(xdrs, 0);
	rpcbuf.value = xdr_inline_encode(xdrs, rpcbuf.length);

	maj_stat =
	    gss_get_mic(&min_stat, gd->ctx, gd->sec.qop, &rpcbuf, &checksum);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_get_mic", maj_stat, min_stat);
		if (maj_stat == GSS_S_CONTEXT_EXPIRED) {
			gd->established = false;
			authgss_destroy_context(auth);
		}
		return (false);
	}
	if (checksum.length > MAX_AUTH_BYTES) {
		gss_log_status("checksum.length", maj_stat, min_stat);
		gss_release_buffer(&min_stat, &checksum);
		return (false);
	}
	auth->ah_verf.oa_flavor = RPCSEC_GSS;
	auth->ah_verf.oa_length = checksum.length;
	memcpy(auth->ah_verf.oa_body, checksum.value, checksum.length);

	xdr_stat = xdr_opaque_auth_encode(xdrs, &auth->ah_verf);
	gss_release_buffer(&min_stat, &checksum);

	return (xdr_stat);
}

static bool
authgss_validate(AUTH *auth, struct opaque_auth *verf)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);
	u_int num, qop_state;
	gss_buffer_desc signbuf, checksum;
	OM_uint32 maj_stat, min_stat;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (gd->established == false) {
		/* would like to do this only on NULL rpc --
		 * gc->established is good enough.
		 * save the on the wire verifier to validate last
		 * INIT phase packet after decode if the major
		 * status is GSS_S_COMPLETE
		 */
		gd->gc_wire_verf.value = mem_alloc(verf->oa_length);
		memcpy(gd->gc_wire_verf.value, verf->oa_body, verf->oa_length);
		gd->gc_wire_verf.length = verf->oa_length;
		return (true);
	}

	if (gd->gc.gc_proc == RPCSEC_GSS_INIT
	    || gd->gc.gc_proc == RPCSEC_GSS_CONTINUE_INIT) {
		num = htonl(gd->win);
	} else
		num = htonl(gd->gc.gc_seq);

	signbuf.value = &num;
	signbuf.length = sizeof(num);

	checksum.value = verf->oa_body;
	checksum.length = verf->oa_length;

	maj_stat =
	    gss_verify_mic(&min_stat, gd->ctx, &signbuf, &checksum, &qop_state);
	if (maj_stat != GSS_S_COMPLETE || qop_state != gd->sec.qop) {
		gss_log_status("gss_verify_mic", maj_stat, min_stat);
		if (maj_stat == GSS_S_CONTEXT_EXPIRED) {
			gd->established = false;
			authgss_destroy_context(auth);
		}
		return (false);
	}
	return (true);
}

#define authgss_refresh_return(code) \
	do { \
		if (gd_locked) \
			mutex_unlock(&gd->lock); \
		return (code); \
	} while (0)

static bool
authgss_refresh(AUTH *auth, void *arg)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);
	struct rpc_gss_init_res gr;
	gss_buffer_desc *recv_tokenp, send_token;
	OM_uint32 maj_stat, min_stat, call_stat, ret_flags;
	bool gd_locked = false;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	/* Serialize context. */
	mutex_lock(&gd->lock);
	gd_locked = true;

	if (gd->established)
		authgss_refresh_return(true);

	/* GSS context establishment loop. */
	memset(&gr, 0, sizeof(gr));
	recv_tokenp = GSS_C_NO_BUFFER;

#ifdef DEBUG
	print_rpc_gss_sec(&gd->sec);
#endif	 /*DEBUG*/
	    for (;;) {
#ifdef DEBUG
		/* print the token we just received */
		if (recv_tokenp != GSS_C_NO_BUFFER) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"The token we just received (length %d):",
				recv_tokenp->length);
			gss_log_hexdump(recv_tokenp->value, recv_tokenp->length,
					0);
		}
#endif
		maj_stat = gss_init_sec_context(&min_stat,
						gd->sec.cred,
						&gd->ctx,
						gd->name,
						gd->sec.mech,
						gd->sec.req_flags,
						0,	/* time req */
						NULL,	/* channel */
						recv_tokenp,
						NULL,	/* used mech */
						&send_token,
						&ret_flags,
						NULL);	/* time rec */

		if (recv_tokenp != GSS_C_NO_BUFFER) {
			gss_release_buffer(&min_stat, &gr.gr_token);
			recv_tokenp = GSS_C_NO_BUFFER;
		}
		if (maj_stat != GSS_S_COMPLETE
		    && maj_stat != GSS_S_CONTINUE_NEEDED) {
			gss_log_status("gss_init_sec_context",
					maj_stat, min_stat);
			break;
		}
		if (send_token.length != 0) {
			struct clnt_req *cc = mem_alloc(sizeof(*cc));

			memset(&gr, 0, sizeof(gr));

#ifdef DEBUG
			/* print the token we are about to send */
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"The token being sent (length %d):",
				send_token.length);
			gss_log_hexdump(send_token.value, send_token.length, 0);
#endif

			clnt_req_fill(cc, gd->clnt, auth, NULLPROC,
				      (xdrproc_t) xdr_rpc_gss_init_args,
				      &send_token,
				      (xdrproc_t) xdr_rpc_gss_init_res, &gr);
			call_stat = clnt_req_setup(cc, to);
			if (call_stat == RPC_SUCCESS) {
				call_stat = CLNT_CALL_WAIT(cc);
			}
			clnt_req_release(cc);

			gss_release_buffer(&min_stat, &send_token);

			if (call_stat != RPC_SUCCESS
			    || (gr.gr_major != GSS_S_COMPLETE
				&& gr.gr_major != GSS_S_CONTINUE_NEEDED))
				authgss_refresh_return(false);

			if (gr.gr_ctx.length != 0) {
				if (gd->gc.gc_ctx.value)
					gss_release_buffer(&min_stat,
							   &gd->gc.gc_ctx);
				gd->gc.gc_ctx = gr.gr_ctx;
			}
			if (gr.gr_token.length != 0) {
				if (maj_stat != GSS_S_CONTINUE_NEEDED)
					break;
				recv_tokenp = &gr.gr_token;
			}
			gd->gc.gc_proc = RPCSEC_GSS_CONTINUE_INIT;
		}

		/* GSS_S_COMPLETE => check gss header verifier,
		 * usually checked in gss_validate
		 */
		if (maj_stat == GSS_S_COMPLETE) {
			gss_buffer_desc bufin;
			gss_buffer_desc bufout;
			u_int seq, qop_state = 0;

			seq = htonl(gr.gr_win);
			bufin.value = (unsigned char *)&seq;
			bufin.length = sizeof(seq);
			bufout.value = (unsigned char *)gd->gc_wire_verf.value;
			bufout.length = gd->gc_wire_verf.length;

			maj_stat =
			    gss_verify_mic(&min_stat, gd->ctx, &bufin, &bufout,
					   &qop_state);

			if (maj_stat != GSS_S_COMPLETE
			    || qop_state != gd->sec.qop) {
				gss_log_status("gss_verify_mic",
						maj_stat, min_stat);
				if (maj_stat == GSS_S_CONTEXT_EXPIRED) {
					gd->established = false;
					authgss_destroy_context(auth);
				}
				authgss_refresh_return (false);
			}
			gd->established = true;
			gd->gc.gc_proc = RPCSEC_GSS_DATA;
			gd->gc.gc_seq = 0;
			gd->win = gr.gr_win;
			break;
		}
	}
	/* End context negotiation loop. */
	if (gd->gc.gc_proc != RPCSEC_GSS_DATA) {
		if (gr.gr_token.length != 0)
			gss_release_buffer(&min_stat, &gr.gr_token);

		auth->ah_error.re_status = RPC_AUTHERROR;
		authgss_refresh_return(false);
	}
	authgss_refresh_return(true);
}

bool
authgss_service(AUTH *auth, int svc)
{
	struct rpc_gss_data *gd;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (!auth)
		return (false);
	gd = AUTH_PRIVATE(auth);
	if (!gd || !gd->established)
		return (false);
	gd->sec.svc = svc;
	gd->gc.gc_svc = svc;
	return (true);
}

static void
authgss_destroy_context(AUTH *auth)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);
	OM_uint32 min_stat;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (gd->gc.gc_ctx.length != 0) {
		if (gd->established) {
			struct clnt_req *cc = mem_alloc(sizeof(*cc));

			gd->gc.gc_proc = RPCSEC_GSS_DESTROY;

			clnt_req_fill(cc, gd->clnt, auth, NULLPROC,
				      (xdrproc_t) xdr_void, NULL,
				      (xdrproc_t) xdr_void, NULL);
			if (clnt_req_setup(cc, to) == RPC_SUCCESS) {
				(void)CLNT_CALL_WAIT(cc);
			}
			clnt_req_release(cc);
		}
		gss_release_buffer(&min_stat, &gd->gc.gc_ctx);
		/* XXX ANDROS check size of context  - should be 8 */
		memset(&gd->gc.gc_ctx, 0, sizeof(gd->gc.gc_ctx));
	}
	if (gd->ctx != GSS_C_NO_CONTEXT) {
		gss_delete_sec_context(&min_stat, &gd->ctx, NULL);
		gd->ctx = GSS_C_NO_CONTEXT;
	}

	/* free saved wire verifier (if any) */
	mem_free(gd->gc_wire_verf.value, gd->gc_wire_verf.length);
	gd->gc_wire_verf.value = NULL;
	gd->gc_wire_verf.length = 0;

	gd->established = false;
}

static void
authgss_destroy(AUTH *auth)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);
	OM_uint32 min_stat;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	authgss_destroy_context(auth);

#ifdef DEBUG
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() freeing name %p",
		__func__, gd->name);
#endif
	if (gd->name != GSS_C_NO_NAME)
		gss_release_name(&min_stat, &gd->name);

	mem_free(gd, sizeof(*gd));
}

bool
authgss_wrap(AUTH *auth, XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() %d %s", __func__,
		!gd->established ? 0 : gd->sec.svc,
		!gd->established ? "not established"
		: gd->sec.svc == RPCSEC_GSS_SVC_NONE ? "krb5"
		: gd->sec.svc == RPCSEC_GSS_SVC_INTEGRITY ? "krb5i"
		: gd->sec.svc == RPCSEC_GSS_SVC_PRIVACY ? "krb5p"
		: "unknown");

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE)
		return ((*xdr_func) (xdrs, xdr_ptr));

	return (xdr_rpc_gss_wrap
		(xdrs, xdr_func, xdr_ptr, gd->ctx, gd->sec.qop, gd->sec.svc,
		 gd->gc.gc_seq));
}

bool
authgss_unwrap(AUTH *auth, XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr)
{
	struct rpc_gss_data *gd = AUTH_PRIVATE(auth);

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s()", __func__);

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE)
		return ((*xdr_func) (xdrs, xdr_ptr));

	return (xdr_rpc_gss_unwrap
		(xdrs, xdr_func, xdr_ptr, gd->ctx, gd->sec.qop, gd->sec.svc,
		 gd->gc.gc_seq, NULL, NULL));
}
