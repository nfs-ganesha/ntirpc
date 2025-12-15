// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2025, IBM . All rights reserved.
 * Author: Deeraj Patil <deeraj.patil@ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.  see <http://www.gnu.org/licenses/
 *
 * ---------------------------------------
 */

/**
 * @file svc_auth_tls.c
 * @brief Routines used handling the AUTH_TLS request and internally calls the
 * TLS functions for handshake.
 * Reference : https://datatracker.ietf.org/doc/rfc9289/
 *
 */

#include <rpc/svc_auth.h>
#include <rpc/gss_internal.h>
#include "tls.h"
#include "tls_transport.h"

#define STARTTLS_TOKEN "STARTTLS"
#define STARTTLS_TOKEN_LEN 8

extern SVCAUTH svc_auth_none;
/**
 * Send AUTH_TLS response with STARTTLS token using existing RPC infrastructure
 */
bool svcauth_tls_send_response(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	char starttls_token[STARTTLS_TOKEN_LEN];
	bool res = true;
	int rc = 0;

	/* Prepare STARTTLS token */
	memcpy(starttls_token, STARTTLS_TOKEN, STARTTLS_TOKEN_LEN);

	/* Set up the reply verifier with STARTTLS token */
	req->rq_msg.RPCM_ack.ar_verf.oa_flavor = AUTH_NONE;
	req->rq_msg.RPCM_ack.ar_verf.oa_length = STARTTLS_TOKEN_LEN;
	memcpy(req->rq_msg.RPCM_ack.ar_verf.oa_body, starttls_token,
	       STARTTLS_TOKEN_LEN);

	/* Set up successful reply */
	req->rq_msg.RPCM_ack.ar_stat = SUCCESS;
	req->rq_msg.RPCM_ack.ar_results.where = NULL;
	req->rq_msg.RPCM_ack.ar_results.proc = (xdrproc_t)xdr_void;
	req->rq_msg.rm_direction = REPLY;
	req->rq_msg.rm_reply.rp_stat = MSG_ACCEPTED;
	/* Send the reply using existing RPC infrastructure */
	rc = svc_sendreply(req);
	if (rc >= XPRT_DIED) {
		LogDebugTLS(TLS_HANDSHAKE,
			    "svc_sendreply failed for AUTH_TLS response on fd %"
			    PRId32, xprt->xp_fd);
		res = false;
	} else {
		LogDebugTLS(TLS_HANDSHAKE,
			    "AUTH_TLS STARTTLS response sent on fd %" PRId32,
			    xprt->xp_fd);
	}
	res = svc_tls_init_xprt(xprt);
	return res;
}

/**
 * AUTH_TLS authentication handler
 */
enum auth_stat svcauth_tls_checks(struct svc_req *req)
{
	enum auth_stat rc = AUTH_OK;

	/* Unserialize client credentials. */
	if (req->rq_msg.cb_cred.oa_length != 0) {
		/* AUTH_TLS credentials MUST be zero length */
		LogDebugTLS(TLS_HANDSHAKE,
			"AUTH_TLS credential length must be zero, got %" PRIu32,
			req->rq_msg.cb_cred.oa_length);
		return AUTH_BADCRED;
	}

	/* Validate AUTH_TLS probe format */
	if (req->rq_msg.cb_cred.oa_flavor != AUTH_TLS) {
		LogDebugTLS(TLS_HANDSHAKE,
			    "Expected AUTH_TLS flavor, got %" PRId32,
			    req->rq_msg.cb_cred.oa_flavor);
		rc = AUTH_BADCRED;
	}

	/* Check verifier - MUST be AUTH_NONE with zero length */
	if (req->rq_msg.cb_verf.oa_flavor != AUTH_NONE ||
	    req->rq_msg.cb_verf.oa_length != 0) {
		LogDebugTLS(TLS_HANDSHAKE,
			"AUTH_TLS verifier must be AUTH_NONE with zero length");
		rc = AUTH_BADCRED;
	}

	/* Check if this is within an existing TLS session */
	if (req->rq_xprt->xp_tls.tls_enabled ||
	    req->rq_xprt->xp_tls.tls_established) {
		LogDebugTLS(TLS_HANDSHAKE,
			"AUTH_TLS probe within existing TLS session on fd %"
			PRId32,	req->rq_xprt->xp_fd);
		rc = AUTH_BADCRED;
		return rc;
	}

	/* Check if this is NULL procedure - AUTH_TLS only allowed on NULL */
	if (req->rq_msg.cb_proc != NULLPROC) {
		LogDebugTLS(TLS_HANDSHAKE,
			"AUTH_TLS only allowed on NULL procedure, proc %"
			PRIu32,	req->rq_msg.cb_proc);
		rc = AUTH_BADCRED;
	}

	/* Mark this connection as TLS-pending */
	req->rq_xprt->xp_tls.tls_pending = true;

	return rc;
}

enum auth_stat _svcauth_tls(struct svc_req *req)
{
	enum auth_stat rslt;

	rslt = svcauth_tls_checks(req);
	if (rslt == AUTH_OK) {
		/* Set rq_auth to avoid NULL pointer dereference in SVCAUTH_CHECKSUM */
		req->rq_auth = &svc_auth_none;
		LogDebugTLS(TLS_HANDSHAKE,
			    "TLS Valid AUTH_TLS probe detected on fd %" PRId32,
			    req->rq_xprt->xp_fd);
		if (svcauth_tls_send_response(req) == false)
			rslt = AUTH_TOOWEAK;
	}
	return rslt;
}
