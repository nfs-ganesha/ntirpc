
/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
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
/*
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

/*
 * svc_auth.c, Server-side rpc authenticator interface.
 *
 */
#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>
#include <stdlib.h>
#ifdef USE_MONITORING
#include "metrics_libntirpc.h"
#endif /* USE_MONITORING */

/*
 * svcauthsw is the bdevsw of server side authentication.
 *
 * Server side authenticators are called from authenticate by
 * using the client auth struct flavor field to index into svcauthsw.
 * The server auth flavors must implement a routine that looks
 * like:
 *
 *	enum auth_stat
 *	flavorx_auth(rqst, msg)
 *		struct svc_req *rqst;
 *		struct rpc_msg *msg;
 *
 */

/* declarations to allow servers to specify new authentication flavors */
struct authsvc {
	int flavor;
	enum auth_stat (*handler) (struct svc_req *);
	struct authsvc *next;
};
static struct authsvc *Auths;

/*
 * The call rpc message, msg has been obtained from the wire.  The msg contains
 * the raw form of credentials and verifiers.  authenticate returns AUTH_OK
 * if the msg is successfully authenticated.  If AUTH_OK then the routine also
 * does the following things:
 * set req->rq_msg.RPCM_ack.ar_verf to the appropriate response verifier;
 * set req->rq_msg.rq_cred_body to the "cooked" form of the credentials.
 *
 * NB: ar_verf must be pre-allocated, its length is set appropriately.
 *
 * The caller still owns and is responsible for msg->cb_cred and
 * msg->cb_verf.  The authentication system retains ownership of
 * rq_cred_body, the cooked credentials.
 *
 * There is an assumption that any flavour less than AUTH_NULL is invalid.
 */
enum auth_stat
svc_auth_authenticate(struct svc_req *req, bool *no_dispatch)
{
	struct authsvc *asp;
	enum auth_stat rslt;
	int cred_flavor;
	extern mutex_t authsvc_lock;
#ifdef USE_MONITORING
	struct timespec start, end, latency;

	metrics_libntirpc_clock_gettime(&start);
#endif /* USE_MONITORING */

	/* VARIABLES PROTECTED BY authsvc_lock: asp, Auths */
	req->rq_msg.RPCM_ack.ar_verf = _null_auth;
	cred_flavor = req->rq_msg.cb_cred.oa_flavor;
	req->rq_msg.RPCM_ack.ar_verf.oa_flavor = cred_flavor;
	switch (cred_flavor) {
#ifdef USE_TLS
	case AUTH_TLS:
		/* Validate AUTH_TLS probe */
		rslt = _svcauth_tls(req);
		goto out;
#endif /* USE_TLS */
#ifdef _HAVE_GSSAPI
	case RPCSEC_GSS:
		rslt = _svcauth_gss(req, no_dispatch);
		goto out;
#endif /* _HAVE_GSSAPI */
	case AUTH_NONE:
		rslt = _svcauth_none(req);
		goto out;
	case AUTH_SYS:
		rslt = _svcauth_unix(req);
		goto out;
	case AUTH_SHORT:
		rslt = _svcauth_short(req);
		return (rslt);
#ifdef DES_BUILTIN
	case AUTH_DES:
		rslt = _svcauth_des(req);
		return (rslt);
#endif
	default:
		break;
	}

	/* flavor doesn't match any of the builtin types, so try new ones */
	mutex_lock(&authsvc_lock);
	for (asp = Auths; asp; asp = asp->next) {
		if (asp->flavor == cred_flavor) {
			enum auth_stat as;
			as = (*asp->handler) (req);
			mutex_unlock(&authsvc_lock);
			return (as);
		}
	}
	mutex_unlock(&authsvc_lock);

	return (AUTH_REJECTEDCRED);

out:
#ifdef USE_MONITORING
	metrics_libntirpc_clock_gettime(&end);
	timespecsub(&end, &start, &latency);
	metrics_libntirpc_observe_svc_auth_request_latency(cred_flavor,
		rslt, &latency);
#endif /* USE_MONITORING */

	return (rslt);
}

/*
 *  Allow the rpc service to register new authentication types that it is
 *  prepared to handle.  When an authentication flavor is registered,
 *  the flavor is checked against already registered values.  If not
 *  registered, then a new Auths entry is added on the list.
 *
 *  There is no provision to delete a registration once registered.
 *
 *  This routine returns:
 *	 0 if registration successful
 *	 1 if flavor already registered
 *	-1 if can't register (errno set)
 */

int svc_auth_reg(int cred_flavor,
		 enum auth_stat (*handler) (struct svc_req *))
{
	struct authsvc *asp;
	extern mutex_t authsvc_lock;

	switch (cred_flavor) {
	case AUTH_NULL:
	case AUTH_SYS:
#ifdef USE_TLS
	case AUTH_TLS:
#endif
	case AUTH_SHORT:
	case RPCSEC_GSS:
#ifdef DES_BUILTIN
	case AUTH_DES:
#endif
		/* already registered */
		return (1);

	default:
		mutex_lock(&authsvc_lock);
		for (asp = Auths; asp; asp = asp->next) {
			if (asp->flavor == cred_flavor) {
				/* already registered */
				mutex_unlock(&authsvc_lock);
				return (1);
			}
		}

		/* this is a new one, so go ahead and register it */
		asp = mem_alloc(sizeof(*asp));
		asp->flavor = cred_flavor;
		asp->handler = handler;
		asp->next = Auths;
		Auths = asp;
		mutex_unlock(&authsvc_lock);
		break;
	}
	return (0);
}
