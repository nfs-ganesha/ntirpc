/*
  svc_auth_gss.c

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
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>
#include <rpc/gss_internal.h>
#include <misc/portable.h>

static struct svc_auth_ops svc_auth_gss_ops;

#define SVCAUTH_PRIVATE(auth) \
	((struct svc_rpc_gss_data *)(auth)->svc_ah_private)

/* Global server credentials. */
gss_cred_id_t svcauth_gss_creds;
static gss_name_t svcauth_gss_name;
static int64_t svcauth_gss_creds_expires;
static gss_cred_id_t svcauth_prev_gss_creds;

/* RW-Locks to synchronise reads and writes to global auth variables */
static rwlock_t svcauth_gss_creds_lock = RWLOCK_INITIALIZER;
static rwlock_t svcauth_gss_name_lock = RWLOCK_INITIALIZER;

/* Global flag to indicate if svcauth_gss is enabled (default: enabled) */
bool svcauth_gss_enabled = true;
static mutex_t svcauth_gss_status_lock = MUTEX_INITIALIZER;

/*
 * This function sets the global svcauth_gss authentication status.
 * The status can be ON or OFF, depending on the input.
 *
 * If the new status is OFF, we reset the global auth state variables and
 * clear gss-context cache.
 *
 * We protect the entire function through a mutex to prevent conflicts between
 * threads trying to set different statuses.
 */
void
svcauth_gss_set_status(bool status_enabled)
{
	OM_uint32 maj_stat, min_stat;

	/* Acquire mutex to overwrite global `svcauth_gss_enabled` */
	mutex_lock(&svcauth_gss_status_lock);

	if (svcauth_gss_enabled == status_enabled) {
		mutex_unlock(&svcauth_gss_status_lock);
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: svcauth_gss is already set to %d", __func__, status_enabled);
		return;
	}
	svcauth_gss_enabled = status_enabled;

	if (status_enabled) {
		/* No further action required */
		mutex_unlock(&svcauth_gss_status_lock);
		return;
	}

	/* Reset all state related to svcauth_gss credentials */
	rwlock_wrlock(&svcauth_gss_creds_lock);

	if (svcauth_gss_creds != NULL) {
		maj_stat = gss_release_cred(&min_stat, &svcauth_gss_creds);

		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: failed to release gss_creds major=%u minor=%u",
				__func__, maj_stat, min_stat);
		}
		svcauth_gss_creds = NULL;
	}
	if (svcauth_prev_gss_creds != NULL) {
		maj_stat = gss_release_cred(&min_stat, &svcauth_prev_gss_creds);

		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: failed to release prev_gss_creds major=%u minor=%u",
				__func__, maj_stat, min_stat);
		}
		svcauth_prev_gss_creds = NULL;
	}
	/* Set expiry-time in the past to not use existing creds */
	svcauth_gss_creds_expires = 1;

	rwlock_unlock(&svcauth_gss_creds_lock);

	/* Reset svcauth_gss service name */
	rwlock_wrlock(&svcauth_gss_name_lock);

	if (svcauth_gss_name != NULL) {
		maj_stat = gss_release_name(&min_stat, &svcauth_gss_name);
		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: failed to release gss_name major=%u minor=%u",
				__func__, maj_stat, min_stat);
		}
		svcauth_gss_name = NULL;
	}
	rwlock_unlock(&svcauth_gss_name_lock);

	/* Clear authgss-context cache */
	authgss_ctx_hash_clear();

        mutex_unlock(&svcauth_gss_status_lock);
}

bool
svcauth_gss_set_svc_name(gss_name_t name)
{
	OM_uint32 maj_stat, min_stat;

	/* Acquire write lock before updating svcauth_gss_name */
	rwlock_wrlock(&svcauth_gss_name_lock);

	if (svcauth_gss_name != NULL) {
		maj_stat = gss_release_name(&min_stat, &svcauth_gss_name);
		if (maj_stat != GSS_S_COMPLETE) {
			rwlock_unlock(&svcauth_gss_name_lock);
			return (false);
		}
		svcauth_gss_name = NULL;
	}

	/* XXX Ganesha */
	if (svcauth_gss_name == GSS_C_NO_NAME) {
		rwlock_unlock(&svcauth_gss_name_lock);
		return (true);
	}
	maj_stat = gss_duplicate_name(&min_stat, name, &svcauth_gss_name);
	rwlock_unlock(&svcauth_gss_name_lock);

	if (maj_stat != GSS_S_COMPLETE)
		return (false);

	return (true);
}

bool
svcauth_gss_import_name(char *service)
{
	gss_name_t name;
	gss_buffer_desc namebuf;
	OM_uint32 maj_stat, min_stat;

	namebuf.value = service;
	namebuf.length = strlen(service);

	maj_stat =
	    gss_import_name(&min_stat, &namebuf,
			    (gss_OID) GSS_C_NT_HOSTBASED_SERVICE, &name);
	if (maj_stat != GSS_S_COMPLETE)
		return (false);

	if (svcauth_gss_set_svc_name(name) != true) {
		gss_release_name(&min_stat, &name);
		return (false);
	}

	/* discard duplicate name */
	gss_release_name(&min_stat, &name);

	return (true);
}

#ifdef __APPLE__
/* there's also mach_absolute_time() - don't know if it's faster */
#define get_time_fast()	time(0)
#else
static inline int64_t
get_time_fast(void)
{
	struct timespec ts[1];
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, ts);
	return ts->tv_sec;
}
#endif

bool
svcauth_gss_acquire_cred(void)
{
	OM_uint32 maj_stat, min_stat;
	int64_t now;
	OM_uint32 timerec;
	gss_cred_id_t old_creds, ancient_creds;

	now = get_time_fast();

	rwlock_rdlock(&svcauth_gss_creds_lock);
	if (svcauth_gss_creds && (!svcauth_gss_creds_expires || svcauth_gss_creds_expires > now)) {
		rwlock_unlock(&svcauth_gss_creds_lock);
		return (true);
	}
	rwlock_unlock(&svcauth_gss_creds_lock);

	/* Now acquire write-lock for writing svcauth_gss_creds */
	rwlock_wrlock(&svcauth_gss_creds_lock);

	if (svcauth_gss_creds && (!svcauth_gss_creds_expires || svcauth_gss_creds_expires > now)) {
		maj_stat = GSS_S_COMPLETE;
	} else {
		ancient_creds = svcauth_prev_gss_creds;
		old_creds = svcauth_gss_creds;
		timerec = 0;
		now = get_time_fast();

		/* Acquire read-lock before reading svcauth_gss_name */
		rwlock_rdlock(&svcauth_gss_name_lock);
		maj_stat =
		    gss_acquire_cred(&min_stat, svcauth_gss_name, 0, GSS_C_NULL_OID_SET,
				     GSS_C_ACCEPT, &svcauth_gss_creds, NULL, &timerec);
		rwlock_unlock(&svcauth_gss_name_lock);

		if (maj_stat == GSS_S_COMPLETE) {
			if (timerec == GSS_C_INDEFINITE)
				svcauth_gss_creds_expires = 0;
			else
				svcauth_gss_creds_expires = now + timerec;
			if (old_creds) {
				svcauth_prev_gss_creds = old_creds;
			}
			if (ancient_creds) {
				(void) gss_release_cred(&min_stat, &ancient_creds);
			}
		}
	}
	/* Release the svcauth_gss_creds write-lock */
	rwlock_unlock(&svcauth_gss_creds_lock);

	if (maj_stat != GSS_S_COMPLETE) {
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: gss_acquire_cred failed major=%u minor=%u", __func__,
			maj_stat, min_stat);
		return (false);
	}

	return (true);
}

bool
svcauth_gss_release_cred(void)
{
	rwlock_wrlock(&svcauth_gss_creds_lock);
#if 0
	OM_uint32 maj_stat, min_stat;

	maj_stat = gss_release_cred(&min_stat, &svcauth_gss_creds);
	if (maj_stat != GSS_S_COMPLETE) {
		rwlock_unlock(&svcauth_gss_creds_lock);
		return (false);
	}
	svcauth_gss_creds = NULL;
#else
	svcauth_gss_creds_expires = 1;
	/* make any future callers to svcauth_gss_acquire_cred() should get a new cred. */
#endif

	rwlock_unlock(&svcauth_gss_creds_lock);
	return (true);
}

static bool
svcauth_gss_accept_sec_context(struct svc_req *req,
			       struct svc_rpc_gss_data *gd,
			       struct rpc_gss_init_res *gr)
{
	struct rpc_gss_cred *gc;
	gss_buffer_desc recv_tok, seqbuf, checksum;
	gss_OID mech;
	OM_uint32 maj_stat = 0, min_stat = 0, ret_flags, seq;
#define INDEF_EXPIRE 60*60*24	/* from mit k5 src/lib/rpc/svc_auth_gssapi.c */
	OM_uint32 time_rec;

	gc = (struct rpc_gss_cred *)req->rq_msg.rq_cred_body;
	memset(gr, 0, sizeof(*gr));

	/* Deserialize arguments. */
	memset(&recv_tok, 0, sizeof(recv_tok));

	req->rq_msg.rm_xdr.where = &recv_tok;
	req->rq_msg.rm_xdr.proc = (xdrproc_t)xdr_rpc_gss_init_args;
	if (!SVCAUTH_UNWRAP(req)) {
		xdr_free((xdrproc_t)xdr_rpc_gss_init_args, (void *)&recv_tok);
		return (false);
	}

	rwlock_rdlock(&svcauth_gss_creds_lock);

	if (svcauth_gss_creds == NULL) {
		rwlock_unlock(&svcauth_gss_creds_lock);
		xdr_free((xdrproc_t)xdr_rpc_gss_init_args, (void *)&recv_tok);
		return false;
	}

	gr->gr_major =
	    gss_accept_sec_context(&gr->gr_minor, &gd->ctx, svcauth_gss_creds,
				   &recv_tok, GSS_C_NO_CHANNEL_BINDINGS,
				   &gd->client_name, &mech, &gr->gr_token,
				   &ret_flags, &time_rec, NULL);
	rwlock_unlock(&svcauth_gss_creds_lock);

	xdr_free((xdrproc_t)xdr_rpc_gss_init_args, (void *)&recv_tok);

	if ((gr->gr_major != GSS_S_COMPLETE)
	    && (gr->gr_major != GSS_S_CONTINUE_NEEDED)) {
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: auth failed major=%u minor=%u", __func__,
			gr->gr_major, gr->gr_minor);
		gd->ctx = GSS_C_NO_CONTEXT;
		gss_release_buffer(&min_stat, &gr->gr_token);
		return (false);
	}
	/* ANDROS: krb5 mechglue returns ctx of size 8 - two pointers,
	 * one to the mechanism oid, one to the internal_ctx_id */
	gr->gr_ctx.value = mem_alloc(sizeof(gss_union_ctx_id_desc));
	memcpy(gr->gr_ctx.value, gd->ctx, sizeof(gss_union_ctx_id_desc));
	gr->gr_ctx.length = sizeof(gss_union_ctx_id_desc);

	/* ANDROS: change for debugging linux kernel version...
	   gr->gr_win = 0x00000005;
	 */
	gr->gr_win = sizeof(gd->seqmask) * 8;

	/* Save client info. */
	gd->sec.mech = mech;
	gd->sec.qop = GSS_C_QOP_DEFAULT;
	gd->sec.svc = gc->gc_svc;
	gd->win = gr->gr_win;

	if (time_rec == GSS_C_INDEFINITE) time_rec = INDEF_EXPIRE;
	if (time_rec > 10) time_rec -= 5;
	gd->endtime = time_rec + get_time_fast();

	if (gr->gr_major == GSS_S_COMPLETE) {
		maj_stat =
		    gss_display_name(&min_stat, gd->client_name, &gd->cname,
				     &gd->sec.mech);
		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: display_name major=%u minor=%u", __func__,
				maj_stat, min_stat);
			gss_release_buffer(&min_stat, &gr->gr_token);
			return (false);
		}
#ifdef DEBUG
#ifdef HAVE_KRB5
		{
			gss_buffer_desc mechname;
			gss_oid_to_str(&min_stat, mech, &mechname);
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: accepted context for %.*s with "
				"<mech %.*s, qop %d, svc %d>", __func__,
				gd->cname.length, (char *)gd->cname.value,
				mechname.length, (char *)mechname.value,
				gd->sec.qop, gd->sec.svc);
			gss_release_buffer(&min_stat, &mechname);
		}
#elif HAVE_HEIMDAL
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: accepted context for %.*s with "
			"<mech {}, qop %d, svc %d>", __func__, gd->cname.length,
			(char *)gd->cname.value, gd->sec.qop, gd->sec.svc);
#endif
#endif				/* DEBUG */
		seq = htonl(gr->gr_win);
		seqbuf.value = &seq;
		seqbuf.length = sizeof(seq);

		gss_release_buffer(&min_stat, &gd->checksum);

		maj_stat =
		    gss_get_mic(&min_stat, gd->ctx, GSS_C_QOP_DEFAULT, &seqbuf,
				&checksum);

		if (maj_stat != GSS_S_COMPLETE) {
			gss_release_buffer(&min_stat, &gr->gr_token);
			return (false);
		}

		/* XXX ref? (assert gd->locked?) */
		if (checksum.length > MAX_AUTH_BYTES){
			gss_release_buffer(&min_stat, &gr->gr_token);
			gss_release_buffer(&min_stat, &checksum);
			return (false);
		}
		req->rq_msg.RPCM_ack.ar_verf.oa_flavor = RPCSEC_GSS;
		req->rq_msg.RPCM_ack.ar_verf.oa_length = checksum.length;
		memcpy(req->rq_msg.RPCM_ack.ar_verf.oa_body, checksum.value,
		       checksum.length);
		gss_release_buffer(&min_stat, &checksum);
	}
	return (true);
}

#define RPCHDR_LEN ((10 * BYTES_PER_XDR_UNIT) + MAX_AUTH_BYTES)

static int
svcauth_gss_validate(struct svc_req *req,
		     struct svc_rpc_gss_data *gd)
{
	struct opaque_auth *oa;
	int32_t *buf;
	gss_buffer_desc rpcbuf, checksum;
	OM_uint32 maj_stat, min_stat, qop_state;
	u_char rpchdr[RPCHDR_LEN];

	memset(rpchdr, 0, RPCHDR_LEN);

	/* XXX - Reconstruct RPC header for signing (from xdr_callmsg). */
	oa = &req->rq_msg.cb_cred;
	if (oa->oa_length > MAX_AUTH_BYTES)
		return GSS_S_CALL_BAD_STRUCTURE;
	/* XXX since MAX_AUTH_BYTES is 400, the following code trivially
	 * overruns (up to 431 per Coverity, but compare RPCHDR_LEN with
	 * what is marshalled below). */

	buf = (int32_t *) rpchdr;
	IXDR_PUT_LONG(buf, req->rq_msg.rm_xid);
	IXDR_PUT_ENUM(buf, req->rq_msg.rm_direction);
	IXDR_PUT_LONG(buf, req->rq_msg.rm_call.cb_rpcvers);
	IXDR_PUT_LONG(buf, req->rq_msg.cb_prog);
	IXDR_PUT_LONG(buf, req->rq_msg.cb_vers);
	IXDR_PUT_LONG(buf, req->rq_msg.cb_proc);
	IXDR_PUT_ENUM(buf, oa->oa_flavor);
	IXDR_PUT_LONG(buf, oa->oa_length);
	if (oa->oa_length) {
		memcpy(buf, oa->oa_body, oa->oa_length);
		buf += RNDUP(oa->oa_length) / sizeof(int32_t);
	}
	rpcbuf.value = rpchdr;
	rpcbuf.length = (u_char *) buf - rpchdr;

	checksum.value = req->rq_msg.cb_verf.oa_body;
	checksum.length = req->rq_msg.cb_verf.oa_length;

	maj_stat =
	    gss_verify_mic(&min_stat, gd->ctx, &rpcbuf, &checksum, &qop_state);

	if (maj_stat != GSS_S_COMPLETE) {
		__warnx(TIRPC_DEBUG_FLAG_AUTH, "%s: %d %d", __func__, maj_stat,
			min_stat);
	}
	return (maj_stat);
}

bool
svcauth_gss_nextverf(struct svc_req *req, struct svc_rpc_gss_data *gd,
		     u_int num)
{
	gss_buffer_desc signbuf, checksum;
	OM_uint32 maj_stat, min_stat;

	signbuf.value = &num;
	signbuf.length = sizeof(num);

	maj_stat =
	    gss_get_mic(&min_stat, gd->ctx, gd->sec.qop, &signbuf, &checksum);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_get_mic", maj_stat, min_stat);
		return (false);
	}
	if (checksum.length > MAX_AUTH_BYTES) {
		gss_log_status("checksum.length", maj_stat, min_stat);
		gss_release_buffer(&min_stat, &checksum);
		return (false);
	}
	req->rq_msg.RPCM_ack.ar_verf.oa_flavor = RPCSEC_GSS;
	req->rq_msg.RPCM_ack.ar_verf.oa_length = checksum.length;
	memcpy(req->rq_msg.RPCM_ack.ar_verf.oa_body, checksum.value,
	       checksum.length);
	gss_release_buffer(&min_stat, &checksum);

	return (true);
}

enum auth_stat
_svcauth_gss(struct svc_req *req, bool *no_dispatch)
{
	XDR xdrs[1];
	SVCAUTH *auth;
	struct svc_rpc_gss_data *gd = NULL;
	struct rpc_gss_cred *gc = NULL;
	struct rpc_gss_init_res gr;
	int call_stat, offset;
	OM_uint32 min_stat;
	enum auth_stat rc = AUTH_OK;

	/* Unserialize client credentials. */
	if (req->rq_msg.cb_cred.oa_length <= 0) {
		return AUTH_BADCRED;
	}

	gc = (struct rpc_gss_cred *)req->rq_msg.rq_cred_body;
	memset(gc, 0, sizeof(struct rpc_gss_cred));

	xdrmem_create(xdrs, req->rq_msg.cb_cred.oa_body,
		      req->rq_msg.cb_cred.oa_length, XDR_DECODE);

	if (!xdr_rpc_gss_cred(xdrs, gc)) {
		XDR_DESTROY(xdrs);
		rc = AUTH_BADCRED;
		goto cred_free;
	}
	XDR_DESTROY(xdrs);

	/* Check version. */
	if (gc->gc_v != RPCSEC_GSS_VERSION) {
		rc = AUTH_BADCRED;
		goto cred_free;
	}

	if (gc->gc_seq > RPCSEC_GSS_MAXSEQ) {
		rc = RPCSEC_GSS_CTXPROBLEM;
		goto cred_free;
	}

	if (gc->gc_proc > RPCSEC_GSS_MAXPROC) {
		rc = AUTH_BADCRED;
		goto cred_free;
	}

	/* Check RPCSEC_GSS service. */
	if (gc->gc_svc != RPCSEC_GSS_SVC_NONE
	    && gc->gc_svc != RPCSEC_GSS_SVC_INTEGRITY
	    && gc->gc_svc != RPCSEC_GSS_SVC_PRIVACY) {
		rc = AUTH_BADCRED;
		goto cred_free;
	}

	/* Context lookup. */
	if ((gc->gc_proc == RPCSEC_GSS_DATA)
	    || (gc->gc_proc == RPCSEC_GSS_DESTROY)) {

		/* Per RFC 2203 5.3.3.3, if a valid security context
		 * cannot be found to authorize a request, the
		 * implementation returns RPCSEC_GSS_CREDPROBLEM.
		 * N.B., we are explicitly allowed to discard contexts
		 * for any reason (e.g., to save space). */
		gd = authgss_ctx_hash_get(gc);
		if (!gd) {
			rc = RPCSEC_GSS_CREDPROBLEM;
			goto cred_free;
		}
		if (gc->gc_svc != gd->sec.svc)
			gd->sec.svc = gc->gc_svc;
	}

	if (!gd) {
		/* Allocate and set up server auth handle. */
		auth = mem_alloc(sizeof(SVCAUTH));
		gd = alloc_svc_rpc_gss_data();
		auth->svc_ah_ops = &svc_auth_gss_ops;
		auth->svc_ah_private = gd;
		gd->auth = auth;
	}

	/* Serialize context. */
	mutex_lock(&gd->lock);

	/* thread auth */
	req->rq_auth = gd->auth;

	/* Check sequence number. */
	if (gd->established) {
		if (get_time_fast() >= gd->endtime) {
			*no_dispatch = true;
			 rc = RPCSEC_GSS_CREDPROBLEM;
			 goto gd_free;
		}

		/* XXX implied serialization?  or just fudging?  advance if
		 * greater? */
		offset = gd->seqlast - gc->gc_seq;
		if (offset < 0) {
			gd->seqlast = gc->gc_seq;
			offset = 0 - offset;
			gd->seqmask <<= offset;
			offset = 0;
		} else if (offset >= gd->win || (gd->seqmask & (1 << offset))) {
			*no_dispatch = true;
			goto gd_free;
		}
		gd->seqmask |= (1 << offset);	/* XXX harmless */

		req->rq_ap1 = (void *)(uintptr_t) gc->gc_seq; /* GCC casts */
		req->rq_clntname = (char *) gd->client_name;
		req->rq_svcname = (char *) gd->ctx;
	}

	/* gd->established */
	/* Handle RPCSEC_GSS control procedure. */
	switch (gc->gc_proc) {

	case RPCSEC_GSS_INIT:
	case RPCSEC_GSS_CONTINUE_INIT:

		if (req->rq_msg.cb_proc != NULLPROC) {
			rc = AUTH_FAILED; /* XXX ? */
			goto gd_free;
		}

		/* Fail new INIT / CONTINUE_INIT requests if svcauth_gss is disabled */
		if (!svcauth_gss_enabled) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH, "%s: auth-gss disabled. Failing", __func__);
			rc = AUTH_REJECTEDCRED;
			goto gd_free;
		}

		/* XXX why unconditionally acquire creds? */
		if (!svcauth_gss_acquire_cred()) {
			rc = AUTH_FAILED;
			goto gd_free;
		}

		if (!svcauth_gss_accept_sec_context(req, gd, &gr)) {
			rc = AUTH_REJECTEDCRED;
			goto gd_free;
		}

		if (!svcauth_gss_nextverf(req, gd, htonl(gr.gr_win))) {
			/* XXX check */
			gss_release_buffer(&min_stat, &gr.gr_token);
			mem_free(gr.gr_ctx.value, 0);
			rc = AUTH_FAILED;
			goto gd_free;
		}

		if (gr.gr_major == GSS_S_COMPLETE) {
			(void)authgss_ctx_hash_set(gd);
		}

		*no_dispatch = true;

		req->rq_msg.RPCM_ack.ar_results.where = &gr;
		req->rq_msg.RPCM_ack.ar_results.proc =
					(xdrproc_t) xdr_rpc_gss_init_res;
		call_stat = svc_sendreply(req);

		/* XXX */
		gss_release_buffer(&min_stat, &gr.gr_token);
		gss_release_buffer(&min_stat, &gd->checksum);
		mem_free(gr.gr_ctx.value, 0);

		if (call_stat >= XPRT_DIED) {
			rc = AUTH_FAILED;
			(void)authgss_ctx_hash_del(gd);
			goto gd_free;
		}

		if (gr.gr_major == GSS_S_COMPLETE) {
			gd->established = true;
			/* krb5 pac -- try all that apply */
			gss_buffer_desc attr, display_buffer;

			/* completely generic */
			int auth = 1, comp = 0, more = -1;

			memset(&gd->pac.ms_pac, 0,
			       sizeof(gss_buffer_desc));
			memset(&display_buffer, 0,
			       sizeof(gss_buffer_desc));

			/* MS AD */
			attr.value = "urn:mspac:";
			attr.length = 10;

			gr.gr_major =
				gss_get_name_attribute(&gr.gr_minor,
						       gd->client_name,
						       &attr, &auth, &comp,
						       &gd->pac.ms_pac,
						       &display_buffer,
						       &more);

			if (gr.gr_major == GSS_S_COMPLETE) {
				/* dont need it */
				gss_release_buffer(&gr.gr_minor,
						   &display_buffer);
				gd->flags |= SVC_RPC_GSS_FLAG_MSPAC;
			}
		}
		break;

		/* XXX next 2 cases:  is it correct to leave gd in cache
		 * after a validate or verf failure ? */

	case RPCSEC_GSS_DATA:
		call_stat = svcauth_gss_validate(req, gd);
		switch (call_stat) {
		default:
			rc = RPCSEC_GSS_CREDPROBLEM;
			goto gd_free;
		case 0:
			break;
		}

		if (!svcauth_gss_nextverf(req, gd, htonl(gc->gc_seq))) {
			rc = AUTH_FAILED;
			goto gd_free;
		}
		break;

	case RPCSEC_GSS_DESTROY:
		if (req->rq_msg.cb_proc != NULLPROC) {
			rc = AUTH_FAILED;	/* XXX ? */
			goto gd_free;
		}

		if (svcauth_gss_validate(req, gd)) {
			rc = RPCSEC_GSS_CREDPROBLEM;
			goto gd_free;
		}

		if (!svcauth_gss_nextverf(req, gd, htonl(gc->gc_seq))) {
			rc = AUTH_FAILED;
			goto gd_free;
		}

		*no_dispatch = true;

		/* avoid lock order reversal gd->lock, xprt->xp_lock */
		mutex_unlock(&gd->lock);

		/* This takes gd->lock, so call it after we unlock */
		(void)authgss_ctx_hash_del(gd);

		req->rq_msg.RPCM_ack.ar_results.where = NULL;
		req->rq_msg.RPCM_ack.ar_results.proc = (xdrproc_t) xdr_void;
		if (svc_sendreply(req) >= XPRT_DIED) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() svc_sendreply failed",
				__func__);
		}

		/* We acquired a reference on gd with authgss_ctx_hash_get
		 * call.  Time to release the reference as we don't need
		 * gd anymore.
		 */
		unref_svc_rpc_gss_data(gd);
		req->rq_auth = &svc_auth_none;
		goto cred_free;

		break;

	default:
		rc = AUTH_REJECTEDCRED;
		break;
	}
gd_free:
	mutex_unlock(&gd->lock);

	if (rc != AUTH_OK) {
		/* On success, the ref gets returned to the caller */
		unref_svc_rpc_gss_data(gd);
		req->rq_auth = NULL;
	}

cred_free:
	xdr_free((xdrproc_t) xdr_rpc_gss_cred, gc);	

	return rc;
}

static bool
svcauth_gss_release(struct svc_req *req)
{
	struct svc_rpc_gss_data *gd;

	gd = SVCAUTH_PRIVATE(req->rq_auth);
	if (gd)
		unref_svc_rpc_gss_data(gd);
	req->rq_auth = NULL;
	return (true);
}

bool
svcauth_gss_destroy(SVCAUTH *auth)
{
	struct svc_rpc_gss_data *gd;
	OM_uint32 min_stat;

	gd = SVCAUTH_PRIVATE(auth);

	gss_delete_sec_context(&min_stat, &gd->ctx, GSS_C_NO_BUFFER);
	gss_release_buffer(&min_stat, &gd->cname);

	if (gd->client_name)
		gss_release_name(&min_stat, &gd->client_name);

	if (gd->flags & SVC_RPC_GSS_FLAG_MSPAC)
		gss_release_buffer(&min_stat, &gd->pac.ms_pac);

	gss_release_buffer(&min_stat, &gd->checksum);

	mutex_unlock(&gd->lock);
	mutex_destroy(&gd->lock);

	mem_free(gd, sizeof(*gd));
	mem_free(auth, sizeof(*auth));

	return (true);
}

static bool
svcauth_gss_wrap(struct svc_req *req, XDR *xdrs)
{
	struct svc_rpc_gss_data *gd = SVCAUTH_PRIVATE(req->rq_auth);
	u_int gc_seq = (u_int) (uintptr_t) req->rq_ap1;
	struct rpc_gss_cred *gc = (struct rpc_gss_cred *)
					req->rq_msg.rq_cred_body;
	bool result;


	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() %d %s", __func__,
		!gd->established ? 0 : gc->gc_svc,
		!gd->established ? "not established"
		: gc->gc_svc == RPCSEC_GSS_SVC_NONE ? "krb5"
		: gc->gc_svc == RPCSEC_GSS_SVC_INTEGRITY ? "krb5i"
		: gc->gc_svc == RPCSEC_GSS_SVC_PRIVACY ? "krb5p"
		: "unknown");

	if (!gd->established || gc->gc_svc == RPCSEC_GSS_SVC_NONE)
		return (svc_auth_none.svc_ah_ops->svc_ah_wrap(req, xdrs));

	mutex_lock(&gd->lock);
	result = xdr_rpc_gss_wrap(xdrs, req->rq_msg.RPCM_ack.ar_results.proc,
				  req->rq_msg.RPCM_ack.ar_results.where,
				  gd->ctx, gd->sec.qop, gc->gc_svc, gc_seq);
	mutex_unlock(&gd->lock);
	return (result);
}

static bool
svcauth_gss_unwrap(struct svc_req *req)
{
	struct svc_rpc_gss_data *gd = SVCAUTH_PRIVATE(req->rq_auth);
	u_int gc_seq = (u_int) (uintptr_t) req->rq_ap1;
	bool result;

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE)
		return (svc_auth_none.svc_ah_ops->svc_ah_unwrap(req));

	mutex_lock(&gd->lock);
	result = xdr_rpc_gss_unwrap(req->rq_xdrs, req->rq_msg.rm_xdr.proc,
				    req->rq_msg.rm_xdr.where, gd->ctx,
				    gd->sec.qop, gd->sec.svc, gc_seq,
				    NULL, NULL);
	mutex_unlock(&gd->lock);
	return (result);
}

void svcauth_gss_svc_checksum(void *priv, void *databuf, size_t length)
{
	struct svc_req *req = priv;	

	SVC_CHECKSUM(req, databuf, length);
}

static bool
svcauth_gss_checksum(struct svc_req *req)
{
	struct svc_rpc_gss_data *gd = SVCAUTH_PRIVATE(req->rq_auth);
	u_int gc_seq = (u_int) (uintptr_t) req->rq_ap1;
	bool result;

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE) {
		return (svc_auth_none.svc_ah_ops->svc_ah_checksum(req));
	}

	mutex_lock(&gd->lock);
	result = xdr_rpc_gss_unwrap(req->rq_xdrs, req->rq_msg.rm_xdr.proc,
				    req->rq_msg.rm_xdr.where, gd->ctx,
				    gd->sec.qop, gd->sec.svc, gc_seq,
				    svcauth_gss_svc_checksum, req);
	mutex_unlock(&gd->lock);
	return (result);
}

static struct svc_auth_ops svc_auth_gss_ops = {
	svcauth_gss_wrap,
	svcauth_gss_unwrap,
	svcauth_gss_checksum,
	svcauth_gss_release,
	svcauth_gss_destroy
};

char *
svcauth_gss_get_principal(SVCAUTH *auth)
{
	struct svc_rpc_gss_data *gd;
	char *pname;

	gd = SVCAUTH_PRIVATE(auth);

	if (gd->cname.length == 0)
		return (NULL);

	pname = mem_alloc(gd->cname.length + 1);
	memcpy(pname, gd->cname.value, gd->cname.length);
	pname[gd->cname.length] = '\0';

	return (pname);
}
