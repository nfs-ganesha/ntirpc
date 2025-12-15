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
 * @file tls_transport.c
 * @brief Plugging module for entertaining different backend TLS libs.
 * Implementation is done in such a way that, if any user wants to add support
 * for more TLS libs, it should be seamless.
 *
 * Routines used for entertaining TLS in ntirpc.
 *
 */

#include "tls.h"
#include "tls_transport.h"
#include <arpa/inet.h>
#include <stdatomic.h>

tls_cred_t *g_xprt_cred;
bool g_tls_enabled;

bool xprt_tls_init(const char *cert_file, const char *key_file,
		   const char *ca_file, const char *ciphers,
		   const char *min_version, bool ktls, bool debug)
{
	g_xprt_cred = tls_cred_init(cert_file, key_file, ca_file, ciphers,
				   min_version, ktls, debug);
	if (g_xprt_cred != NULL) {
		g_tls_enabled = true;
		return true;
	} else {
		return false;
	}
}

/* Initialize TLS for a transport */
bool xp_tls_init_impl(SVCXPRT *xprt)
{
	int ret;
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	char ip[INET6_ADDRSTRLEN] = {0};

	if (!xprt || !xprt->xp_tls.tls_enabled) {
		LogEventTLS(TLS_HANDSHAKE,
			    "TLS not enabled for this transport");
		ret = false;
		goto out;
	}

	if (getpeername(xprt->xp_fd, (struct sockaddr *)&addr, &len) == 0) {
		if (addr.ss_family == AF_INET) {
			struct sockaddr_in *s = (struct sockaddr_in *)&addr;
			inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
		} else if (addr.ss_family == AF_INET6) {
			struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
			inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
		}
	}

	LogEventTLS(TLS_HANDSHAKE, "trying TLS connection for fd %" PRId32
		    " client: %s", xprt->xp_fd, ip);

	pthread_mutex_lock(&(xprt->xp_tls.tls_lock));
	/* Create TLS context if not already created */
	if (!xprt->xp_tls.tls_ctx) {
		xprt->xp_tls.tls_ctx =
			tls_ctx_init(xprt->xp_fd, g_xprt_cred, true);
		if (!xprt->xp_tls.tls_ctx) {
			LogCritTLS(TLS_HANDSHAKE,
				   "Failed to initialize TLS context for fd %"
				   PRId32 , xprt->xp_fd);
			ret = false;
			goto out;
		}
	}

	if (xprt->xp_tls.tls_established) {
		LogDebugTLS(TLS_HANDSHAKE,
			    "TLS already established for this transport");
		ret = true;
		goto out;
	}

	/* Perform TLS handshake */
	if (!tls_handshake(xprt->xp_tls.tls_ctx)) {
		LogWarnTLS(TLS_HANDSHAKE, "TLS handshake failed for fd %"
			   PRId32 , xprt->xp_fd);
		ret = false;
		goto out;
	}

	/* Verify client certificate */
	char peer_identity[512];
	if (!tls_verify_peer(xprt->xp_tls.tls_ctx, peer_identity,
				 sizeof(peer_identity))) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Client certificate verification failed for fd %"
			   PRId32 , xprt->xp_fd);
		ret = false;
		goto out;
	}


	xprt->xp_tls.mtls = tls_get_type(xprt->xp_tls.tls_ctx);
	xprt->xp_tls.tls_established = true;
	LogEventTLS(TLS_HANDSHAKE, "TLS connection established for fd %" PRId32
		    " client: %s mtls:%d", xprt->xp_fd, ip, xprt->xp_tls.mtls);

	ret = true;
out:
	pthread_mutex_unlock(&(xprt->xp_tls.tls_lock));
	return ret;
}

/* Receive TLS decoded data */
int xp_tls_recv_impl(SVCXPRT *xprt, void *buf, size_t len, int flags)
{
	int ret;

	LogDebugTLS(TLS_DISPATCH, "xprt:%p fd:%" PRId32 , xprt, xprt->xp_fd);
	if (!xprt || !xprt->xp_tls.tls_ctx || !buf || len <= 0) {
		LogDebugTLS(TLS_DISPATCH, "Invalid TLS context for recv");
		return -1;
	}

	ret = tls_recv(xprt->xp_tls.tls_ctx, buf, len, flags);

	if (ret == TLS_SESSION_CLOSED_ADRUPTLY) {
		LogWarnTLS(TLS_DISPATCH, "Session Closed adruptly");
		return -1;
	}
	return ret;
}

/* Send data over TLS */
int xp_tls_send_impl(SVCXPRT *xprt, const struct msghdr *msg, int flags)
{
	int ret = 0;

	LogDebugTLS(TLS_DISPATCH, "xprt:%p fd:%" PRId32 , xprt, xprt->xp_fd);
	if (!xprt || !xprt->xp_tls.tls_ctx || !msg || !msg->msg_iov ||
	    msg->msg_iovlen <= 0) {
		LogDebugTLS(TLS_DISPATCH, "Invalid TLS context for send");
		return -1;
	}

	ret = tls_send(xprt->xp_tls.tls_ctx, msg, flags);

	if (ret == TLS_SESSION_CLOSED_ADRUPTLY) {
		LogWarnTLS(TLS_DISPATCH, "Session Closed adruptly");
		return -1;
	}

	return ret;
}

/* Receive TLS decoded data */
int xp_tls_datapending_impl(SVCXPRT *xprt)
{
	LogDebugTLS(TLS_DISPATCH, "xprt:%p fd:%" PRId32 , xprt, xprt->xp_fd);
	if (!xprt || !xprt->xp_tls.tls_ctx) {
		LogDebugTLS(TLS_DISPATCH, "Invalid TLS context for dpr lib");
		return -1;
	}

	return tls_datapending(xprt->xp_tls.tls_ctx);
}

/* Reset the TLS specific data in xprt */
void xp_tls_reset_xprt(SVCXPRT *xprt)
{
	xprt->xp_tls.tls_enabled = false;
	xprt->xp_tls.tls_ctx = NULL;
	xprt->xp_tls.tls_established = false;
	xprt->xp_tls.mtls = false;
	xprt->xp_tls.not_first_packet = false;
}

/* Close TLS connection */
void xp_tls_close_impl(SVCXPRT *xprt)
{
	if (!xprt || !xprt->xp_tls.tls_ctx) {
		return; /* Nothing to close */
	}
	LogEventTLS(TLS_DISPATCH, "connection close xprt:%p fd:%" PRId32 ,
		    xprt, xprt->xp_fd);
	pthread_mutex_lock(&(xprt->xp_tls.tls_lock));
	tls_close(xprt->xp_tls.tls_ctx);
	xp_tls_reset_xprt(xprt);
	pthread_mutex_unlock(&(xprt->xp_tls.tls_lock));
}

/* Initialize TLS operations for a transport */
bool svc_tls_init_xprt(SVCXPRT *xprt)
{
	bool ret = false;

	if (!xprt) {
		return ret;
	}

	/*
	 * Guard: if the TLS engine failed to initialize at startup, reject
	 * immediately rather than attempting a handshake with NULL credentials.
	 * Log only once regardless of how many clients attempt to connect,
	 * to prevent log flooding.
	 */
	if (!g_tls_enabled) {
		static atomic_flag logged = ATOMIC_FLAG_INIT;

		if (!atomic_flag_test_and_set(&logged))
			LogCritTLS(TLS_INIT,
				   "TLS engine is disabled, rejecting "
				   "connection on fd %" PRId32,
				   xprt->xp_fd);
		return false;
	}

	/* Initialize TLS structure */
	pthread_mutex_init(&(xprt->xp_tls.tls_lock), NULL);
	xprt->xp_tls.tls_enabled = true;
	xprt->xp_tls.tls_ctx = NULL;
	xprt->xp_tls.tls_established = false;

	ret = xp_tls_init_impl(xprt);
	if (ret == false) {
		xp_tls_reset_xprt(xprt);
		LogEventTLS(TLS_HANDSHAKE, "TLS disabled xprt:%p fd:%" PRId32 ,
			    xprt, xprt->xp_fd);

	} else {
		LogDebugTLS(TLS_HANDSHAKE, "TLS enabled xprt:%p fd:%" PRId32 ,
			    xprt, xprt->xp_fd);
	}
	return ret;
}

bool is_tls_clienthello(int fd)
{
	unsigned char peek_buf[5];
	ssize_t n = recv(fd, peek_buf, sizeof(peek_buf), MSG_PEEK);

	if (n < 5)
		return false;

	/* TLS record type = 0x16 (handshake), Version = 0x0303 or higher */
	if (peek_buf[0] == 0x16 && peek_buf[1] == 0x03 &&
	    (peek_buf[2] == 0x01 || peek_buf[2] == 0x03 ||
	     peek_buf[2] == 0x04)) {
		return true;
	}
	return false;
}

/*
 * Used in case of Stunnel connection from client
 */
bool is_handshake_msg(SVCXPRT *xprt)
{
	if (is_tls_clienthello(xprt->xp_fd))
		return svc_tls_init_xprt(xprt);
	return false;
}
