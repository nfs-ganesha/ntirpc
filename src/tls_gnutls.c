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
 * @file tls_gnutls.c
 * @brief Routines used for managing the TLS Session using GnuTLS lib.
 * Implementation patterns have been derived from GNUTLS library
 * especially from server.c patterns
 *
 * Routines used for support TLS in ntirpc.
 *
 */
#include "tls.h"

/* Global GnuTLS context */
static gnutls_priority_t global_priority;
static void tls_enhanced_debug_callback(int level, const char *str);
static int tls_verify_certificate(gnutls_session_t session);

/* Helper function to get GnuTLS error string */
static char *get_gnutls_error(int error_code)
{
	return (char *)gnutls_strerror(error_code);
}

#define MAX_PRIORITY_STR 512

/**
 * Initialize the TLS subsystem
 *
 * @param cert_file    Path to the server certificate file (PEM format)
 * @param key_file     Path to the server private key file (PEM format)
 * @param ca_file      Path to the CA certificate file (PEM format) for
 *			client verification
 * @param ciphers      Cipher suite string (GnuTLS priority string)
 * @param min_version  Minimum TLS version ("TLSv1.2" or "TLSv1.3")
 * @param ktls         To enable or disable ktls
 * @param debug        To enable or disable debugging including registering
 *			lib callbacks
 * @return             Creds which should be used for per session connection
 *			creation, if fails returns NULL.
 */
tls_cred_t *tls_cred_init(const char *cert_file, const char *key_file,
			     const char *ca_file, const char *ciphers,
			     const char *min_version, bool ktls, bool debug)
{
	int ret;
	char priority_str[MAX_PRIORITY_STR] = { 0 };
	gnutls_certificate_credentials_t global_creds = NULL;

	LogDebugTLS(TLS_DISPATCH, "%s:%" PRId32 , __func__, __LINE__);
	/* Initialize GnuTLS */
	ret = gnutls_global_init();
	if (ret < 0) {
		LogCritTLS(TLS_INIT, "Failed to initialize GnuTLS: %s",
			   get_gnutls_error(ret));
		return NULL;
	}

	/*  Register debug callback - THIS IS THE KEY PART for debugging */
	/*  As per lib set to 9 for maximum verbosity, adjust as needed */
	if (debug) {
		gnutls_global_set_log_function(tls_enhanced_debug_callback);
		gnutls_global_set_log_level(9);
	}

	/* Initialize certificate credentials */
	ret = gnutls_certificate_allocate_credentials(&global_creds);
	if (ret < 0) {
		LogCritTLS(TLS_INIT, "Failed to allocate credentials: %s",
			   get_gnutls_error(ret));
		goto cleanup_global;
	}

	/* Load CA certificates for client verification */
	if (ca_file) {
		ret = gnutls_certificate_set_x509_trust_file(
			global_creds, ca_file, GNUTLS_X509_FMT_PEM);
		if (ret < 0) {
			LogCritTLS(TLS_INIT, "Failed to load CA file %s: %s",
				   ca_file, get_gnutls_error(ret));
			goto cleanup_global;
		}
	} else {
		/* Use default system CA certificates */
		ret = gnutls_certificate_set_x509_system_trust(global_creds);
		if (ret < 0) {
			LogWarnTLS(TLS_INIT,
				   "Failed to set default system trust: %s",
				   get_gnutls_error(ret));
			goto cleanup_global;
		}
	}

	/* Load server certificate and key */
	ret = gnutls_certificate_set_x509_key_file(global_creds, cert_file,
						   key_file,
						   GNUTLS_X509_FMT_PEM);
	if (ret < 0) {
		LogCritTLS(TLS_INIT, "Failed to load certificate/key files: %s",
			   get_gnutls_error(ret));
		goto cleanup_creds;
	}

	if (min_version && strcmp(min_version, "TLSv1.2") == 0)
		snprintf(priority_str, MAX_PRIORITY_STR, "NORMAL:-VERS-ALL:+%s",
			 "VERS-TLS1.2:+VERS-TLS1.3");
	else
		/* Default to TLSv1.3 only when min_version is NULL or unrecognised */
		snprintf(priority_str, MAX_PRIORITY_STR, "NORMAL:-VERS-ALL:+%s",
			 "VERS-TLS1.3");

	/* If custom cipher list provided, append it, else do the default*/
	if (ciphers && strlen(ciphers) > 0) {
		strncat(priority_str, ciphers,
			MAX_PRIORITY_STR - strlen(priority_str) - 1);
	}

	LogDebugTLS(TLS_INIT, "Setting priority: %s", priority_str);
	ret = gnutls_priority_init(&global_priority, priority_str, NULL);
	if (ret < 0) {
		LogCritTLS(TLS_INIT, "Failed to set priority: %s",
			   get_gnutls_error(ret));
		goto cleanup_creds;
	}

	/* Set certificate verification function if CA is provided */
	if (ca_file) {
		gnutls_certificate_set_verify_function(
			global_creds, tls_verify_certificate);
	}

	LogDebugTLS(TLS_INIT, "TLS initialized successfully with GnuTLS");
	return global_creds;

cleanup_creds:
	gnutls_certificate_free_credentials(global_creds);
cleanup_global:
	gnutls_global_deinit();
	return NULL;
}

/**
 * Create a new TLS context for a socket
 *
 * @param fd           Socket file descriptor
 * @param cred         creds which got returned from tls_cred_init()
 * @param is_server    Connection type i.e fd is acting as server or a client.
 * @return             Pointer to the new TLS context, or NULL on failure
 */
tls_ctx_t *tls_ctx_init(int fd, tls_cred_t *creds, bool is_server)
{
	tls_ctx_t *ctx;
	int ret;

	LogDebugTLS(TLS_HANDSHAKE, "%s:%" PRId32 , __func__, __LINE__);

	if (!creds) {
		LogCritTLS(TLS_HANDSHAKE, "TLS not initialized");
		return NULL;
	}

	ctx = calloc(1, sizeof(tls_ctx_t));
	if (!ctx) {
		LogCritTLS(TLS_HANDSHAKE, "Failed to allocate TLS context");
		return NULL;
	}

	pthread_mutex_init(&(ctx->ctx_lock), NULL);
	ctx->fd = fd;

	/* Initialize session */
	if (is_server) {
		ret = gnutls_init(&ctx->session, GNUTLS_SERVER);
	} else {
		ret = gnutls_init(&ctx->session, GNUTLS_CLIENT);
	}
	if (ret < 0) {
		LogCritTLS(TLS_HANDSHAKE,
			   "Failed to initialize GnuTLS session: %s",
			   get_gnutls_error(ret));
		free(ctx);
		return NULL;
	}

	gnutls_transport_set_int(ctx->session, fd);

	/* Set credentials */
	ret = gnutls_credentials_set(ctx->session, GNUTLS_CRD_CERTIFICATE,
				     creds);
	if (ret < 0) {
		LogCritTLS(TLS_HANDSHAKE, "Failed to set credentials: %s",
			   get_gnutls_error(ret));
		gnutls_deinit(ctx->session);
		free(ctx);
		return NULL;
	}

	/* Set priority */
	ret = gnutls_priority_set(ctx->session, global_priority);
	if (ret < 0) {
		LogCritTLS(TLS_HANDSHAKE, "Failed to set priority: %s",
			   get_gnutls_error(ret));
		gnutls_deinit(ctx->session);
		free(ctx);
		return NULL;
	}
	if (is_server)
		gnutls_certificate_server_set_request(ctx->session,
						      GNUTLS_CERT_REQUEST);

	return ctx;
}

/**
 * Perform TLS handshake
 *
 * @param ctx          TLS context
 * @return             true on success, false on failure
 */
bool tls_handshake(tls_ctx_t *ctx)
{
	int ret;

	LogDebugTLS(TLS_HANDSHAKE, "%s:%" PRId32 , __func__, __LINE__);
	gnutls_datum_t out;
	int type;
	unsigned int status;
	int counter = 0;

	if (!ctx || !ctx->session) {
		LogCritTLS(TLS_HANDSHAKE, "Invalid TLS context");
		return false;
	}

	pthread_mutex_lock(&(ctx->ctx_lock));

	if (ctx->handshake_complete) {
		pthread_mutex_unlock(&(ctx->ctx_lock));
		return true;
	}

	/* Set timeout to 10 seconds, time in milliseconds */
	gnutls_handshake_set_timeout(ctx->session, 100000);

	/* Perform server handshake */
retry:
	do {
		LogDebugTLS(TLS_HANDSHAKE, "trying Handhake ");
		ret = gnutls_handshake(ctx->session);
		++counter;
	} while (ret < 0 && gnutls_error_is_fatal(ret) == 0);

	if (ret < 0) {
		LogEventTLS(TLS_HANDSHAKE, "*** Handshake failed: :%" PRId32
			    " %s\n", ret, gnutls_strerror(ret));
		switch (ret) {
		case GNUTLS_E_AGAIN:
		case GNUTLS_E_INTERRUPTED:
			/* Handshake needs more data, not an error */
			LogDebugTLS(TLS_HANDSHAKE, "Need more data: %s",
				    get_gnutls_error(ret));
			goto retry;

		case GNUTLS_E_CERTIFICATE_VERIFICATION_ERROR:
			/* check certificate verification status */
			type = gnutls_certificate_type_get(ctx->session);
			status = gnutls_session_get_verify_cert_status(
				ctx->session);
			gnutls_certificate_verification_status_print(status,
								     type, &out,
								     0);
			LogDebugTLS(TLS_HANDSHAKE, "cert verify output: %s\n",
				    out.data);
			gnutls_free(out.data);
			break;

		case GNUTLS_E_FATAL_ALERT_RECEIVED:
			LogDebugTLS(TLS_HANDSHAKE,
				    "TLS fatal alert received on fd ");
			break;

		default:
			LogDebugTLS(TLS_HANDSHAKE,
				    "TLS Error <NOT HANDLED> :%s",
				    gnutls_strerror(ret));
			break;
		}
		pthread_mutex_unlock(&(ctx->ctx_lock));
		return false;
	}

	ctx->handshake_complete = true;
	/* Check for SNI hostname */
	char name[256];
	size_t name_len = sizeof(name);

	ret = gnutls_server_name_get(ctx->session, name, &name_len, &type, 0);
	if (ret == 0 && type == GNUTLS_NAME_DNS) {
		LogDebugTLS(TLS_HANDSHAKE, "SNI hostname: %s", name);
	} else {
		LogDebugTLS(TLS_HANDSHAKE, "No SNI hostname received");
	}

	ret = true;
	pthread_mutex_unlock(&(ctx->ctx_lock));
	LogDebugTLS(TLS_HANDSHAKE, "TLS handshake done");
	return ret;
}

/**
 * Check secure connection is TLS or MTLS
 *
 * @param ctx          TLS context
 * @return             true on MTLS, false on TLS
 */
bool tls_get_type(tls_ctx_t *ctx)
{
	unsigned int cert_list_size = 0;
	const gnutls_datum_t *cert_list;
	bool ret = false;

	if (!ctx->session)
		return ret;

	cert_list = gnutls_certificate_get_peers(ctx->session, &cert_list_size);

	if (cert_list && cert_list_size > 0) {
		unsigned int status = 0;

		ret = gnutls_certificate_verify_peers3(ctx->session, NULL,
						       &status);

		if (ret == 0 && status == 0) {
			ret = true;
		} else {
			ret = false;
		}
	}
	LogDebugTLS(TLS_HANDSHAKE, "MTLS CERIFICATE %" PRId32 , ret);
	return ret;
}

/**
 * Verify peer certificate and extract identity
 *
 * @param ctx           TLS context
 * @param peer_identity Buffer to store peer identity (CN from certificate)
 * @param id_size       Size of the peer_identity buffer
 * @return              true if verification succeeded, false otherwise
 */
static int tls_verify_certificate(gnutls_session_t session)
{
	unsigned int status;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size;
	gnutls_x509_crt_t cert;
	int ret;

	/* Get peer certificate list */
	cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
	if (cert_list == NULL || cert_list_size == 0) {
		LogDebugTLS(TLS_HANDSHAKE, "No client certificate provided");
		 /*  Allow connections without client certs */
		return GNUTLS_E_SUCCESS;
	}

	/* Verify certificate chain */
	ret = gnutls_certificate_verify_peers3(session, NULL, &status);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE, "Certificate verification failed: %s",
			   gnutls_strerror(ret));
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	if (status != 0) {
		gnutls_datum_t out;

		gnutls_certificate_verification_status_print(status,
							     GNUTLS_CRT_X509,
							     &out, 0);
		LogWarnTLS(TLS_HANDSHAKE, "Certificate verification failed: %s",
			   out.data);
		gnutls_free(out.data);
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	/* Initialize certificate */
	ret = gnutls_x509_crt_init(&cert);
	if (ret < 0) {
		LogWarnTLS(TLS_INIT, "Failed to initialize certificate: %s",
			   gnutls_strerror(ret));
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	/* Import certificate */
	ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE, "Failed to import certificate: %s",
			   gnutls_strerror(ret));
		gnutls_x509_crt_deinit(cert);
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	/* Log certificate information */
	char dn[256];
	size_t dn_size = sizeof(dn);

	ret = gnutls_x509_crt_get_dn(cert, dn, &dn_size);
	if (ret >= 0) {
		LogDebugTLS(TLS_HANDSHAKE, "Client certificate DN: %s", dn);
	}

	gnutls_x509_crt_deinit(cert);
	LogDebugTLS(TLS_HANDSHAKE, "Client certificate verified successfully");

	return GNUTLS_E_SUCCESS;
}

/**
 * Receive data over TLS
 *
 * @param ctx          TLS context
 * @param buf          Buffer to store received data
 * @param len          Length of the buffer
 * @return             Number of bytes received, or converted error
 */
int tls_recv(tls_ctx_t *ctx, void *buf, size_t len, int flags)
{
	int ret;
	int error_code = 0;
	int fd = ctx->fd;
	int orig_flags = fcntl(fd, F_GETFL, 0);
	//bool nonblock = flags & MSG_DONTWAIT;
	bool nonblock = 0;
	ssize_t offset = 0;


	if (!ctx || !ctx->session)
		return EINVAL;

	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK);

	LogDebugTLS(TLS_DISPATCH, "Recv requested len > %" PRId32 , len);

	while (offset < len) {
retry:
		ret = gnutls_record_recv(ctx->session, buf + offset,
					 len - offset);
		if (ret <= 0) {
			LogDebugTLS(TLS_DISPATCH, "Read ret:%" PRId32
				   " error:%s %" PRId32 ,
				    ret, get_gnutls_error(ret), errno);
			switch (ret) {
			case GNUTLS_E_AGAIN:
			case GNUTLS_E_INTERRUPTED:
				goto retry;
			case GNUTLS_E_PREMATURE_TERMINATION:
			case GNUTLS_E_INVALID_SESSION:
				error_code = TLS_SESSION_CLOSED_ADRUPTLY;
				break;
			default:
				error_code = TLS_SESSION_UNKNOWN_ERROR;
				break;
			}
			return error_code;
		}
		offset += ret;
	}
	/* restore original flags */
	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags);

	LogDebugTLS(TLS_DISPATCH, "Recv Completed len: %" PRId64 , offset);
	return offset;
}

/**
 * Function provides details of the pending data in library internal bufferes
 * only for recv
 *
 * @param ctx          TLS context
 * @return             Number of bytes cached in internal buffers
 */
int tls_datapending(tls_ctx_t *ctx)
{
        return gnutls_record_check_pending(ctx->session);
}

/**
 * Send data over TLS
 *
 * @param ctx          TLS context
 * @param msg          Message to send
 * @param flags        Send flags
 * @return             Number of bytes sent, or converted error
 */
int tls_send(tls_ctx_t *ctx, const struct msghdr *msg, int flags)
{
	int fd = ctx->fd;
	int orig_flags = fcntl(fd, F_GETFL, 0);
	//bool nonblock = flags & MSG_DONTWAIT;
	bool nonblock = 0;
	ssize_t total_sent = 0;
	int error_code = 0;
	int ret;

	if (!ctx || !ctx->session)
		return EINVAL;

	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK);

	LogDebugTLS(TLS_DISPATCH, "Send requested len > %" PRId32 ,
		    msg->msg_iovlen);
	for (int i = 0; i < msg->msg_iovlen; ++i) {
		const char *buf = msg->msg_iov[i].iov_base;
		ssize_t len = msg->msg_iov[i].iov_len;
		ssize_t offset = 0;

retry:
		while (offset < len) {
			ret = gnutls_record_send(ctx->session, buf + offset,
						     len - offset);
			if (ret <= 0) {
				LogDebugTLS(TLS_DISPATCH,
					    "Write ret:%" PRId32 " error:%s %"
					    PRId32 , ret,
					    get_gnutls_error(ret), errno);
				switch (ret) {
				case GNUTLS_E_AGAIN:
				case GNUTLS_E_INTERRUPTED:
					goto retry;
				case GNUTLS_E_PREMATURE_TERMINATION:
					error_code =
						TLS_SESSION_CLOSED_ADRUPTLY;
					break;
				default:
					error_code = TLS_SESSION_UNKNOWN_ERROR;
					break;
				}
				return error_code;
			}
			offset += ret;
			total_sent += ret;
		}
	}

	/* Restore original Flags */
	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags);

	LogDebugTLS(TLS_DISPATCH, "Send Completed len: %" PRId64 , total_sent);
	return total_sent;
}

/**
 * Close TLS connection and free resources
 *
 * @param ctx          TLS context
 * @return             true on success, false on failure
 */
bool tls_close(tls_ctx_t *ctx)
{
	LogDebugTLS(TLS_SHUTDOWN, "ctx:%p", ctx);

	if (!ctx) {
		return true;
	}

	if (ctx->session) {
		/* Properly close the TLS session */
		gnutls_bye(ctx->session, GNUTLS_SHUT_RDWR);
		gnutls_deinit(ctx->session);
	}

	LogDebugTLS(TLS_SHUTDOWN, "freeing ctx");
	free(ctx);
	return true;
}

/**
 * Verify peer certificate and extract identity
 *
 * @param ctx           TLS context
 * @param peer_identity Buffer to store peer identity (CN from certificate)
 * @param id_size       Size of the peer_identity buffer
 * @return              true if verification succeeded, false otherwise
 */
bool tls_verify_peer(tls_ctx_t *ctx, char *peer_identity,
			 size_t id_size)
{
	unsigned int status;
	int ret;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size;
	/* To extract Common Name from certificate */
	gnutls_x509_crt_t cert;

	LogDebugTLS(TLS_HANDSHAKE, "%s:%" PRId32 , __func__, __LINE__);

	if (!ctx || !ctx->session) {
		LogCritTLS(TLS_HANDSHAKE, "Invalid TLS context");
		return false;
	}

	/* Check if client provided a certificate */
	cert_list = gnutls_certificate_get_peers(ctx->session, &cert_list_size);

	if (cert_list == NULL || cert_list_size == 0) {
		LogDebugTLS(TLS_HANDSHAKE,
			    "Client did not provide a certificate");
		if (peer_identity && id_size > 0)
			strlcpy(peer_identity, "anonymous", id_size);
		return true; /* Allow connections without client cert */
	}

	/* Verify certificate */
	ret = gnutls_certificate_verify_peers2(ctx->session, &status);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE, "Certificate verification failed: %s",
			   get_gnutls_error(ret));
		return false;
	}

	if (status != 0) {
		gnutls_datum_t out;

		ret = gnutls_certificate_verification_status_print(
			status, GNUTLS_CRT_X509, &out, 0);
		if (ret == 0) {
			LogWarnTLS(TLS_HANDSHAKE,
				   "Client certificate verification failed: %s",
				   out.data);
			gnutls_free(out.data);
		} else {
			LogWarnTLS(TLS_HANDSHAKE,
				"Client certificate verification failed with status: %" PRIu32 ,
				status);
		}
		return false;
	}

	ret = gnutls_x509_crt_init(&cert);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Failed to initialize certificate structure: %s",
			   get_gnutls_error(ret));
		return false;
	}

	ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE, "Failed to import certificate: %s",
			   get_gnutls_error(ret));
		gnutls_x509_crt_deinit(cert);
		return false;
	}

	/* Get the common name */
	char buf[256];
	size_t buf_size = sizeof(buf);

	ret = gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME,
					    0, 0, buf, &buf_size);
	if (ret < 0) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Could not get CN from client certificate: %s",
			   get_gnutls_error(ret));
		gnutls_x509_crt_deinit(cert);
		return false;
	}

	/* Copy the common name to the output buffer if provided */
	if (peer_identity && id_size > 0) {
		strlcpy(peer_identity, buf, id_size);
	}

	LogDebugTLS(TLS_HANDSHAKE,
		    "Client authenticated with certificate CN: %s", buf);
	gnutls_x509_crt_deinit(cert);
	return true;
}

/**
 * Request a TLS 1.3 key update from the client.
 * call this periodically (e.g. every 3 minutes)
 * But currently its not supported.
 * In future based on requirement will extend this.
 * Note : Shutdown the connection if keyupdate is not supported,
 * which will cause new key exchange
 *
 * @param ssl The active SSL connection.
 * @return 0 on success, -1 on failure.
 */
bool tls_key_update(tls_ctx_t *ctx)
{
	int ret;

	if (!ctx || !ctx->session) {
		LogWarnTLS(TLS_HANDSHAKE, "Invalid ctx\n");
		return false;
	}

	/* Check if we're using TLS 1.3, which supports key updates */
	gnutls_protocol_t version = gnutls_protocol_get_version(ctx->session);

	if (version != GNUTLS_TLS1_3) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Key update is only supported in TLS 1.3\n");
		return false;
	}

	/* Request key update */
	ret = gnutls_session_key_update(ctx->session, GNUTLS_KU_PEER);
	if (ret < 0) {
		LogWarnTLS(TLS_INIT, "gnutls_session_key_update failed: %s\n",
			   get_gnutls_error(ret));
		return false;
	}

	LogWarnTLS(TLS_HANDSHAKE, "Key update requested and completed.\n");
	return true;
}

/*Debug callback registered with lib to get detailed o/p of whats happening */
static void tls_enhanced_debug_callback(int level, const char *str)
{
	const char *prefix = "";
	static int handshake_in_progress;
	static int session_resumed;

	/* Remove trailing newlines and whitespace */
	char *clean_str = strdup(str);

	if (clean_str) {
		size_t len = strlen(clean_str);

		while (len > 0 && (clean_str[len - 1] == '\n' ||
				   clean_str[len - 1] == '\r' ||
				   clean_str[len - 1] == ' ' ||
				   clean_str[len - 1] == '\t')) {
			clean_str[--len] = '\0';
		}
	} else {
		return;
	}

	/* Skip empty messages */
	if (strlen(clean_str) == 0) {
		free(clean_str);
		return;
	}

	/* Detailed message parsing for different TLS states and operations */

	/* Handshake related messages */
	if (strstr(clean_str, "handshake") || strstr(clean_str, "Handshake")) {
		if (strstr(clean_str, "start") ||
		    strstr(clean_str, "starting") ||
		    strstr(clean_str, "begin") ||
		    strstr(clean_str, "initiated")) {
			prefix = "[HANDSHAKE START]";
			handshake_in_progress = 1;
			session_resumed = 0;
		} else if (strstr(clean_str, "finished") ||
			   strstr(clean_str, "completed") ||
			   strstr(clean_str, "successful") ||
			   strstr(clean_str, "done")) {
			if (session_resumed) {
				prefix = "[HANDSHAKE RESUMED DONE]";
			} else {
				prefix = "[HANDSHAKE DONE]";
			}
			handshake_in_progress = 0;
		} else if (strstr(clean_str, "resumed") ||
			   strstr(clean_str, "resuming")) {
			prefix = "[HANDSHAKE RESUMED]";
			session_resumed = 1;
		} else if (handshake_in_progress) {
			prefix = "[HANDSHAKE LOOP]";
		} else {
			prefix = "[HANDSHAKE]";
		}
	}
	/* Alert messages */
	else if (strstr(clean_str, "alert") || strstr(clean_str, "Alert")) {
		const char *dir = "unknown";
		const char *level_str = "unknown";
		const char *desc = "unknown";

		if (strstr(clean_str, "received") ||
		    strstr(clean_str, "recv")) {
			dir = "read";
		} else if (strstr(clean_str, "sent") ||
			   strstr(clean_str, "send")) {
			dir = "write";
		}

		if (strstr(clean_str, "warning")) {
			level_str = "warning";
		} else if (strstr(clean_str, "fatal")) {
			level_str = "fatal";
		}

		/* Extract alert description if possible */
		if (strstr(clean_str, "close_notify"))
			desc = "close_notify";
		else if (strstr(clean_str, "unexpected_message"))
			desc = "unexpected_message";
		else if (strstr(clean_str, "bad_record_mac"))
			desc = "bad_record_mac";
		else if (strstr(clean_str, "decryption_failed"))
			desc = "decryption_failed";
		else if (strstr(clean_str, "record_overflow"))
			desc = "record_overflow";
		else if (strstr(clean_str, "decompression_failure"))
			desc = "decompression_failure";
		else if (strstr(clean_str, "handshake_failure"))
			desc = "handshake_failure";
		else if (strstr(clean_str, "no_certificate"))
			desc = "no_certificate";
		else if (strstr(clean_str, "bad_certificate"))
			desc = "bad_certificate";
		else if (strstr(clean_str, "unsupported_certificate"))
			desc = "unsupported_certificate";
		else if (strstr(clean_str, "certificate_revoked"))
			desc = "certificate_revoked";
		else if (strstr(clean_str, "certificate_expired"))
			desc = "certificate_expired";
		else if (strstr(clean_str, "certificate_unknown"))
			desc = "certificate_unknown";
		else if (strstr(clean_str, "illegal_parameter"))
			desc = "illegal_parameter";
		else if (strstr(clean_str, "unknown_ca"))
			desc = "unknown_ca";
		else if (strstr(clean_str, "access_denied"))
			desc = "access_denied";
		else if (strstr(clean_str, "decode_error"))
			desc = "decode_error";
		else if (strstr(clean_str, "decrypt_error"))
			desc = "decrypt_error";
		else if (strstr(clean_str, "protocol_version"))
			desc = "protocol_version";
		else if (strstr(clean_str, "insufficient_security"))
			desc = "insufficient_security";
		else if (strstr(clean_str, "internal_error"))
			desc = "internal_error";
		else if (strstr(clean_str, "user_canceled"))
			desc = "user_canceled";
		else if (strstr(clean_str, "no_renegotiation"))
			desc = "no_renegotiation";

		LogWarnTLS(TLS_INIT, "[ALERT] %s: level=%s, desc=%s (%s)", dir,
			   level_str, desc, clean_str);
		free(clean_str);
		return;
	}
	/* Read operations */
	else if (strstr(clean_str, "read") || strstr(clean_str, "recv") ||
		 strstr(clean_str, "Read") || strstr(clean_str, "reading") ||
		 strstr(clean_str, "REC[") || strstr(clean_str, "received")) {
		prefix = "[READ]";
	}
	/* Write operations */
	else if (strstr(clean_str, "write") || strstr(clean_str, "send") ||
		 strstr(clean_str, "Write") || strstr(clean_str, "writing") ||
		 strstr(clean_str, "sending")) {
		prefix = "[WRITE]";
	}
	/* Certificate related */
	else if (strstr(clean_str, "certificate") ||
		 strstr(clean_str, "Certificate") ||
		 strstr(clean_str, "cert") || strstr(clean_str, "X.509")) {
		prefix = "[CERTIFICATE]";
	}
	/* Key exchange */
	else if (strstr(clean_str, "key") || strstr(clean_str, "Key") ||
		 strstr(clean_str, "ECDHE") || strstr(clean_str, "RSA") ||
		 strstr(clean_str, "DH") || strstr(clean_str, "exchange")) {
		prefix = "[KEY EXCHANGE]";
	}
	/* Cipher related */
	else if (strstr(clean_str, "cipher") || strstr(clean_str, "Cipher") ||
		 strstr(clean_str, "AES") || strstr(clean_str, "GCM") ||
		 strstr(clean_str, "encryption") ||
		 strstr(clean_str, "decrypt")) {
		prefix = "[CIPHER]";
	}
	/* Session related */
	else if (strstr(clean_str, "session") || strstr(clean_str, "Session") ||
		 strstr(clean_str, "ticket") || strstr(clean_str, "cache")) {
		prefix = "[SESSION]";
	}
	/* Version negotiation */
	else if (strstr(clean_str, "version") || strstr(clean_str, "Version") ||
		 strstr(clean_str, "TLS") || strstr(clean_str, "protocol")) {
		prefix = "[VERSION]";
	}
	/* Extensions */
	else if (strstr(clean_str, "extension") ||
		 strstr(clean_str, "Extension") || strstr(clean_str, "SNI") ||
		 strstr(clean_str, "ALPN") ||
		 strstr(clean_str, "server_name")) {
		prefix = "[EXTENSION]";
	}
	/* KTLS related */
	else if (strstr(clean_str, "ktls") || strstr(clean_str, "KTLS") ||
		 strstr(clean_str, "kernel") || strstr(clean_str, "offload")) {
		prefix = "[KTLS]";
	}
	/* Error conditions */
	else if (strstr(clean_str, "error") || strstr(clean_str, "Error") ||
		 strstr(clean_str, "fail") || strstr(clean_str, "Fail") ||
		 strstr(clean_str, "invalid") || strstr(clean_str, "Invalid")) {
		if (strstr(clean_str, "fatal") || strstr(clean_str, "Fatal")) {
			prefix = "[EXIT] failed";
		} else {
			prefix = "[EXIT] error";
		}
	}
	/* Success conditions */
	else if (strstr(clean_str, "success") || strstr(clean_str, "Success") ||
		 strstr(clean_str, "ok") || strstr(clean_str, "OK") ||
		 strstr(clean_str, "complete") ||
		 strstr(clean_str, "established")) {
		prefix = "[EXIT] ok";
	}
	/* Loop/state machine */
	else if (strstr(clean_str, "loop") || strstr(clean_str, "Loop") ||
		 strstr(clean_str, "state") || strstr(clean_str, "State") ||
		 strstr(clean_str, "step") || strstr(clean_str, "phase")) {
		prefix = "[LOOP]";
	}
	/* Record layer */
	else if (strstr(clean_str, "record") || strstr(clean_str, "Record") ||
		 strstr(clean_str, "packet") || strstr(clean_str, "frame")) {
		prefix = "[RECORD]";
	}
	/* Buffer operations */
	else if (strstr(clean_str, "buffer") || strstr(clean_str, "Buffer") ||
		 strstr(clean_str, "data") || strstr(clean_str, "bytes")) {
		prefix = "[BUFFER]";
	}
	/* Default based on log level */
	else {
		switch (level) {
		case 0:
			prefix = "[FATAL]";
			break;
		case 1:
			prefix = "[ERROR]";
			break;
		case 2:
			prefix = "[WARNING]";
			break;
		case 3:
			prefix = "[INFO]";
			break;
		case 4:
			prefix = "[DEBUG]";
			break;
		case 5:
			prefix = "[TRACE]";
			break;
		case 6:
			prefix = "[DETAILED]";
			break;
		case 7:
			prefix = "[VERBOSE]";
			break;
		case 8:
			prefix = "[CRYPTO]";
			break;
		case 9:
			prefix = "[ULTRA]";
			break;
		default:
			prefix = "[UNKNOWN]";
			break;
		}
	}

	/* Log the message with appropriate prefix */
	LogWarnTLS(TLS_UNKNOWN, "%s : %s", prefix, clean_str);

	free(clean_str);
}
