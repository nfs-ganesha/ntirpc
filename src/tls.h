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
 * @file tls.h
 */

#include <fcntl.h>
#include <errno.h>
#include "strl.h"
#include "rpc/svc.h"
#include "rpc/types.h"
#include "rpc/tls.h"
#include "svc_internal.h"
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/types.h>
#endif
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#ifdef USE_GNUTLS
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#endif

#define TLS_SESSION_UNKNOWN_ERROR -1
#define TLS_SESSION_CLOSED_ADRUPTLY -2
#define TLS_HANDSHAKE_FAILED -3

#define TLS_INIT "[Init Path]:"
#define TLS_DISPATCH "[Dispatch]:"
#define TLS_HANDSHAKE "[Handshake Path]:"
#define TLS_SHUTDOWN "[Handshake Path]:"
#define TLS_UNKNOWN " "

#define LogCritTLS(component, format, ...)                                 \
	__warnx(TIRPC_DEBUG_FLAG_ERROR, "[TLS]:%s:%s:%" PRId32 " " format, \
		component, __func__, __LINE__, ##__VA_ARGS__)

#define LogWarnTLS(component, format, ...)                                \
	__warnx(TIRPC_DEBUG_FLAG_WARN, "[TLS]:%s:%s:%" PRId32 " " format, \
		component, __func__, __LINE__, ##__VA_ARGS__)

#define LogEventTLS(component, format, ...)                                \
	__warnx(TIRPC_DEBUG_FLAG_EVENT, "[TLS]:%s:%s:%" PRId32 " " format, \
		component, __func__, __LINE__, ##__VA_ARGS__)

#define LogDebugTLS(component, format, ...)                                \
	if (tls_config.debug)                                              \
		__warnx(TIRPC_DEBUG_FLAG_EVENT, "[TLS]:%s:%s:%" PRId32 " " \
			format, component, __func__, __LINE__, ##__VA_ARGS__)


/* TLS context structure */
typedef struct tls_ctx {
#ifdef USE_OPENSSL
	struct ssl_st *ssl; /* openSSL session */
	struct ssl_ctx_st *ctx;
#endif

#ifdef USE_GNUTLS
	/*
	struct gnutls_session_int *session;
	struct gnutls_certificate_credentials_st *cred;
	*/
	gnutls_session_t session; /* GnuTLS session */
	gnutls_certificate_credentials_t creds; /* GnuTLS credentials */
#endif

	pthread_mutex_t ctx_lock;
	bool handshake_complete;
	int fd;
} tls_ctx_t;

/* libs cache the data in buffers, signal stucture used for DPR */
struct tls_signal_dpr {
	uint32_t signal;
	int fd;
};

#ifdef USE_OPENSSL
typedef SSL_CTX tls_cred_t;
#endif

#ifdef USE_GNUTLS
typedef struct gnutls_certificate_credentials_st tls_cred_t;
#endif

/* tls_config is declared in rpc/tls.h (included via rpc/tls.h above) */
