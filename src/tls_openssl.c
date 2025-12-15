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
 * @file tls_openssl.c
 * @brief Routines used for managing the TLS Session using openSSL lib.
 * Implementation patterns have been derived from openSSL library
 * especially from server.c patterns
 *
 * Routines used for supporting TLS in ntirpc.
 *
 */

#include "tls.h"

/* Global SSL context */
static int ssl_ctx_index = -1;
static void tls_info_callback(const SSL *ssl, int where, int ret);

/* Helper function to get OpenSSL error string */
static char *get_ssl_error(void)
{
	static char ssl_error[256];
	unsigned long e = ERR_get_error();

	if (e == 0)
		return "No SSL error";

	ERR_error_string_n(e, ssl_error, sizeof(ssl_error));
	return ssl_error;
}
unsigned char sid_ctx[] = "ntirpc-tls-session";

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
	SSL_CTX *global_ctx = NULL;

	LogDebugTLS(TLS_INIT, "%s:%" PRId32 , __func__, __LINE__);
	/* Initialize OpenSSL */
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	/* Supports both client/server */
	const SSL_METHOD *method = TLS_method();

	/* Create SSL context */
	global_ctx = SSL_CTX_new(method);
	if (!global_ctx) {
		LogCritTLS(TLS_INIT, "Failed to create SSL context: %s",
			   get_ssl_error());
		return NULL;
	}

	if (debug)
		SSL_CTX_set_info_callback(global_ctx, tls_info_callback);

	/* Enable KTLS enable */
	/* KTLS is only supported by OpenSSL 3.0+ */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	if (ktls)
		SSL_CTX_set_options(global_ctx, SSL_OP_ENABLE_KTLS);
#else
	if (ktls)
		LogWarnTLS(TLS_INIT,
			   "KTLS requested but not supported by this OpenSSL version (< 3.0); ignoring");
#endif

	SSL_CTX_set_session_id_context(global_ctx, sid_ctx, sizeof(sid_ctx));

	/* Set minimum TLS version */
	if (min_version) {
		int version = 0;

		if (strcmp(min_version, "TLSv1.2") == 0)
			version = TLS1_2_VERSION;
		else if (strcmp(min_version, "TLSv1.3") == 0)
			version = TLS1_3_VERSION;

		if (version > 0) {
			SSL_CTX_set_min_proto_version(global_ctx, version);
		}
	}
	SSL_CTX_set_max_early_data(global_ctx, 0);

	/* Set cipher list if provided */
	if (ciphers && SSL_CTX_set_cipher_list(global_ctx, ciphers) != 1) {
		LogCritTLS(TLS_INIT, "Failed to set cipher list: %s",
			   get_ssl_error());
		SSL_CTX_free(global_ctx);
		global_ctx = NULL;
		return NULL;
	}

	/* Load server certificate */
	if (SSL_CTX_use_certificate_file(global_ctx, cert_file,
				SSL_FILETYPE_PEM) != 1) {
		LogCritTLS(TLS_INIT, "Failed to load certificate file %s: %s",
				cert_file, get_ssl_error());
		SSL_CTX_free(global_ctx);
		global_ctx = NULL;
		return NULL;
	}

	/*
	 * Load certificate chain from cert_file.
	 * SSL_CTX_use_certificate_chain_file() handles both cases:
	 *   - Single certificate PEM: loads just the leaf cert.
	 *   - Certificate chain PEM:  loads the leaf cert and all
	 *     intermediate CA certificates in the file, so the full
	 *     chain is sent to the peer during the TLS handshake.
	 */
	if (SSL_CTX_use_certificate_chain_file(global_ctx, cert_file) != 1) {
		LogCritTLS(TLS_INIT,
			   "Failed to load certificate/chain file '%s': %s",
			   cert_file, get_ssl_error());
		SSL_CTX_free(global_ctx);
		global_ctx = NULL;
		return NULL;
	}

	/* Load private key */
	if (SSL_CTX_use_PrivateKey_file(global_ctx, key_file,
					SSL_FILETYPE_PEM) != 1) {
		LogCritTLS(TLS_INIT, "Failed to load private key file %s: %s",
			   key_file, get_ssl_error());
		SSL_CTX_free(global_ctx);
		global_ctx = NULL;
		return NULL;
	}

	/* Check key and certificate */
	if (SSL_CTX_check_private_key(global_ctx) != 1) {
		LogCritTLS(TLS_INIT,
			   "Private key does not match certificate: %s",
			   get_ssl_error());
		SSL_CTX_free(global_ctx);
		global_ctx = NULL;
		return NULL;
	}

	/*  Load CA certificates for client verification */
	if (ca_file) {
		if (SSL_CTX_load_verify_locations(global_ctx, ca_file,
						  NULL) != 1) {
			LogCritTLS(TLS_INIT, "Failed to load CA file %s: %s",
				   ca_file, get_ssl_error());
			SSL_CTX_free(global_ctx);
			global_ctx = NULL;
			return NULL;
		}

		/*
		 * Request client cert; accept connection regardless of
		 * whether the client provides one.  tls_get_type() decides
		 * post-handshake.
		 */
		SSL_CTX_set_verify(global_ctx, SSL_VERIFY_PEER, NULL);
	} else {
		/* try to use default system CA certificates */
		if (SSL_CTX_set_default_verify_paths(global_ctx) != 1) {
			LogWarnTLS(TLS_INIT,
				   "Failed to set default verify paths: %s",
				   get_ssl_error());
			SSL_CTX_free(global_ctx);
			global_ctx = NULL;
			return NULL;
		}
	}

	/* Set up an ex_data index for associating
	 * tls_ctx_t with SSL objects */
	ssl_ctx_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	if (ssl_ctx_index == -1) {
		LogCritTLS(TLS_INIT, "Failed to get SSL ex_data index");
		exit(1);
	}
	if (!SSL_CTX_set_num_tickets(global_ctx, 0)) {
		LogCritTLS(TLS_INIT, "Failed to set num tickets on SSL");
		exit(1);
	}

	LogDebugTLS(TLS_INIT, "TLS initialized successfully with OpenSSL");
	return global_ctx;
}

/**
 * Create a new TLS context for a socket
 *
 * @param fd           Socket file descriptor
 * @param cred         creds which got returned from tls_cred_init()
 * @param is_server    Connection type i.e fd is acting as server or a client.
 * @return             Pointer to the new TLS context, or NULL on failure
 */
tls_ctx_t *tls_ctx_init(int fd, tls_cred_t *cred, bool is_server)
{
	tls_ctx_t *ctx = calloc(1, sizeof(tls_ctx_t));

	if (!ctx)
		return NULL;

	pthread_mutex_init(&(ctx->ctx_lock), NULL);
	ctx->fd = fd;
	ctx->ctx = cred;

	ctx->ssl = SSL_new(ctx->ctx);
	if (!ctx->ssl) {
		LogCritTLS(TLS_INIT, "Failed to create SSL: %s",
			   get_ssl_error());
		free(ctx);
		return NULL;
	}

	if (!SSL_set_fd(ctx->ssl, fd)) {
		LogCritTLS(TLS_INIT, "Failed to set SSL fd: %s",
			   get_ssl_error());
		SSL_free(ctx->ssl);
		free(ctx);
		return NULL;
	}
	if (!SSL_set_ex_data(ctx->ssl, ssl_ctx_index, ctx)) {
		LogCritTLS(TLS_INIT, "Failed to set ex_data on SSL");
		SSL_free(ctx->ssl);
		free(ctx);
		return NULL;
	}

	/* Optionally set server/client specific behavior before handshake */
	if (is_server) {
		LogDebugTLS(TLS_INIT, "FD:%" PRId32 " SERVER_METHOD", fd);
		SSL_set_accept_state(ctx->ssl);
	} else {
		LogDebugTLS(TLS_INIT, "FD:%" PRId32 " CLIENT_METHOD", fd);
		SSL_set_connect_state(ctx->ssl);
	}
	LogDebugTLS(TLS_SHUTDOWN, "ctx_init:%x", ctx);

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
	int ssl_err;
	int counter = 0;

	LogDebugTLS(TLS_HANDSHAKE, "%s:%" PRId32 , __func__, __LINE__);
	if (!ctx || !ctx->ssl) {
		LogCritTLS(TLS_INIT, "Invalid TLS context");
		return false;
	}
	pthread_mutex_lock(&(ctx->ctx_lock));

	if (ctx->handshake_complete) {
		pthread_mutex_unlock(&(ctx->ctx_lock));
		return true;
	}
	/* Clear any previous errors */
	ERR_clear_error();

retry:
	ret = SSL_accept(ctx->ssl);
	if (ret == 1) {
		ctx->handshake_complete = true;
		LogDebugTLS(TLS_HANDSHAKE,
			    "TLS handshake completed successfully");
		const char *servername =
			SSL_get_servername(ctx->ssl, TLSEXT_NAMETYPE_host_name);
		if (servername) {
			LogDebugTLS(TLS_HANDSHAKE, "SNI hostname: %s",
					servername);
		} else {
			LogDebugTLS(TLS_HANDSHAKE, "No SNI hostname received");
		}
		pthread_mutex_unlock(&(ctx->ctx_lock));
		return true;
	}
	++counter;
	ssl_err = SSL_get_error(ctx->ssl, ret);
	if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
		/* Handshake needs more data, not an error */
		LogDebugTLS(TLS_HANDSHAKE, "Need more data:%s : %" PRId32 ,
			    get_ssl_error(), ssl_err);
	}
	if (counter < 1) {
		LogDebugTLS(TLS_HANDSHAKE, "retry %s : %" PRId32 ,
			    get_ssl_error(), ssl_err);
		goto retry;
	}

	LogCritTLS(TLS_HANDSHAKE, "TLS handshake failed: %s", get_ssl_error());
	pthread_mutex_unlock(&(ctx->ctx_lock));
	return false;
}

/**
 * Check secure connection is TLS or MTLS
 *
 * @param ctx          TLS context
 * @return             true on MTLS, false on TLS
 */
bool tls_get_type(tls_ctx_t *ctx)
{
	bool ret = false;

	if (!ctx->ssl)
		return ret;
	X509 *cert = SSL_get_peer_certificate(ctx->ssl);

	if (cert) {
		long verify_result = SSL_get_verify_result(ctx->ssl);

		X509_free(cert);
		if (verify_result == X509_V_OK) {
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
bool tls_verify_peer(tls_ctx_t *ctx, char *peer_identity,
			 size_t id_size)
{
	X509 *cert;
	X509_NAME *subject;
	char buf[256];

	LogDebugTLS(TLS_HANDSHAKE, "%s:%" PRId32 , __func__, __LINE__);

	if (!ctx || !ctx->ssl) {
		LogCritTLS(TLS_HANDSHAKE, "Invalid TLS context");
		return false;
	}

	/* Get client certificate */
	cert = SSL_get_peer_certificate(ctx->ssl);
	if (!cert) {
		LogDebugTLS(TLS_HANDSHAKE,
			    "Client did not provide a certificate");
		if (peer_identity && id_size > 0)
			strlcpy(peer_identity, "anonymous", id_size);
		return true; /* Allow connections without client cert */
	}

	/* Verify certificate */
	long verify_result = SSL_get_verify_result(ctx->ssl);

	if (verify_result != X509_V_OK) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Client certificate verification failed: %s",
			   X509_verify_cert_error_string(verify_result));
		X509_free(cert);
		return false;
	}

	/* Extract Common Name from certificate */
	subject = X509_get_subject_name(cert);
	if (!subject) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Could not get subject from client certificate");
		X509_free(cert);
		return false;
	}

	/* Get the common name */
	int cn_len = X509_NAME_get_text_by_NID(subject, NID_commonName, buf,
					       sizeof(buf));
	if (cn_len < 0) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Could not get CN from client certificate");
		X509_free(cert);
		return false;
	}

	/* Copy the common name to the output buffer if provided */
	if (peer_identity && id_size > 0) {
		strlcpy(peer_identity, buf, id_size);
	}

	LogDebugTLS(TLS_HANDSHAKE,
		    "Client authenticated with certificate CN: %s", buf);
	X509_free(cert);
	return true;
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
	int ret=0;
	int error_code = 0;
	int orig_flags = 0;
	int fd = 0;
	//bool nonblock = flags & MSG_DONTWAIT;
	bool nonblock = 0;
	ssize_t offset = 0;

	if (!ctx || !ctx->ssl)
		return EINVAL;
	fd = ctx->fd;
	orig_flags = fcntl(fd, F_GETFL, 0);

	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK);

	LogDebugTLS(TLS_DISPATCH, "Recv requested len > %" PRId32 , len);

	while (offset < len) {
retry:
		ret = SSL_read(ctx->ssl, buf + offset, len - offset);
		if (ret <= 0) {
			int ssl_err = SSL_get_error(ctx->ssl, ret);

			LogDebugTLS(TLS_DISPATCH, "Read ret:%" PRId32
				   " error:%" PRId32 " %" PRId32 ,
				    ret, ssl_err, errno);
			switch (ssl_err) {
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				goto retry;
			case SSL_ERROR_NONE:
			case SSL_ERROR_SYSCALL:
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
	/* Restore original flags */
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
	return SSL_pending(ctx->ssl);
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
	int fd ;
	int orig_flags;
	//bool nonblock = flags & MSG_DONTWAIT;
	bool nonblock = 0;
	ssize_t total_sent = 0;
	int error_code = 0;

	if (!ctx || !ctx->ssl)
		return EINVAL;

	fd = ctx->fd;
	orig_flags = fcntl(fd, F_GETFL, 0);

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
			int ret =
				SSL_write(ctx->ssl, buf + offset, len - offset);
			if (ret <= 0) {
				int ssl_err = SSL_get_error(ctx->ssl, ret);

				LogDebugTLS(TLS_DISPATCH,
					    "Write ret:%" PRId32 " error:%"
					    PRId32 " %" PRId32 , ret,
					    ssl_err, errno);
				switch (ssl_err) {
				case SSL_ERROR_WANT_READ:
				case SSL_ERROR_WANT_WRITE:
					goto retry;
				case SSL_ERROR_NONE:
				case SSL_ERROR_SYSCALL:
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

	if (nonblock)
		fcntl(fd, F_SETFL, orig_flags); // Restore original flags

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
	LogDebugTLS(TLS_SHUTDOWN, "ctx:%x", ctx);
	int ret = 0;

	if (!ctx) {
		return true;
	}

	if (ctx->ssl) {
		/* write direction of the connection */
		ret = SSL_shutdown(ctx->ssl);
		LogDebugTLS(TLS_SHUTDOWN, "shutdown %" PRId32 , ret);
		if (ret == 0) {
			/* If return is 0, we need to call it again to complete
			 * bidirectional shutdown
			 * i.e read direction of the connection */
			ret = SSL_shutdown(ctx->ssl);
			LogDebugTLS(TLS_SHUTDOWN, "shutdown1 %" PRId32 , ret);
		}

		if (ret < 0) {
			/* Optionally log SSL error */
			int err = SSL_get_error(ctx->ssl, ret);

			LogDebugTLS(TLS_SHUTDOWN, "shutdown failed %" PRId32
				    " :%" PRId32 , ret, err);
		}
		SSL_free(ctx->ssl);
	}

	LogDebugTLS(TLS_SHUTDOWN, "freeing ctx ret:%" PRId32 , ret);
	free(ctx);
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
	if (!ctx || !ctx->ssl) {
		LogWarnTLS(TLS_HANDSHAKE, "Invalid ctx");
		return false;
	}

	if (SSL_version(ctx->ssl) != TLS1_3_VERSION) {
		LogWarnTLS(TLS_HANDSHAKE,
			   "Key update is only supported in TLS 1.3");
		return false;
	}
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	int is_ktls_send = BIO_get_ktls_send(SSL_get_wbio(ctx->ssl));
	int is_ktls_recv = BIO_get_ktls_recv(SSL_get_rbio(ctx->ssl));

	LogWarnTLS(TLS_HANDSHAKE, "is_KTLS_rec:%" PRId32 " isKTLS_send:%"
		   PRId32 , is_ktls_send, is_ktls_recv);

	/* return false directly , which will shutdown the connection
	 * or can try the openssl */
	if (is_ktls_send || is_ktls_recv) {
		return false;
	}
#else
	/* KTLS status check not available on OpenSSL < 3.0 */
	LogDebugTLS(TLS_HANDSHAKE,
		   "KTLS status check not supported by this OpenSSL version (< 3.0)");
#endif
	/* Request peer to update keys for both directions */
	if (!SSL_key_update(ctx->ssl, SSL_KEY_UPDATE_REQUESTED)) {
		LogWarnTLS(TLS_HANDSHAKE, "SSL_key_update failed");
		return false;
	}

	/* SSL_do_handshake must be called to complete the key update process */
	if (SSL_do_handshake(ctx->ssl) <= 0) {
		int err = SSL_get_error(ctx->ssl, -1);

		LogWarnTLS(TLS_HANDSHAKE, "SSL_do_handshake failed: %" PRId32
			  " (%s)\n",
			  err, ERR_reason_error_string(ERR_get_error()));
		return false;
	}

	LogWarnTLS(TLS_INIT,
		   " Key update requested and handshake completed.");
	return true;
}

/* Debug callback registered with lib to get detailed o/p of whats happening */
static void tls_info_callback(const SSL *ssl, int where, int ret)
{
	const char *str = SSL_state_string_long(ssl);
	const char *prefix = "";
	const char *direction = "";
	static int handshake_started;

	/* Handshake state tracking */
	if (where & SSL_CB_HANDSHAKE_START) {
		prefix = "[HANDSHAKE START]";
		handshake_started = 1;
		LogWarnTLS(TLS_INIT, "%s : %s", prefix, str);

		/* Log additional handshake details */
		const char *version = SSL_get_version(ssl);

		LogDebugTLS(TLS_UNKNOWN,
			    "[HANDSHAKE START] Protocol version: %s",
			    version ? version : "unknown");

		/* Log cipher information if available */
		const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);

		if (cipher) {
			LogDebugTLS(TLS_UNKNOWN, "[HANDSHAKE START] Cipher: %s",
				    SSL_CIPHER_get_name(cipher));
		}
		return;
	}

	if (where & SSL_CB_HANDSHAKE_DONE) {
		if (SSL_session_reused(ssl)) {
			prefix = "[HANDSHAKE RESUMED DONE]";
		} else {
			prefix = "[HANDSHAKE DONE]";
		}
		handshake_started = 0;

		LogWarnTLS(TLS_INIT, "%s : %s", prefix, str);

		/* Log detailed handshake completion information */
		const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);

		if (cipher) {
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] Final cipher: %s",
				    SSL_CIPHER_get_name(cipher));
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] Cipher bits: %" PRId32 ,
				    SSL_CIPHER_get_bits(cipher, NULL));
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] Cipher version: %s",
				    SSL_CIPHER_get_version(cipher));
		}

		/* Log protocol version */
		LogDebugTLS(TLS_UNKNOWN, "[HANDSHAKE DONE] Protocol: %s",
			    SSL_get_version(ssl));

		/* Log session information */
		SSL_SESSION *session = SSL_get_session(ssl);

		if (session) {
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] Session timeout: %" PRId64
				    , SSL_SESSION_get_timeout(session));
			if (SSL_session_reused(ssl)) {
				LogDebugTLS(TLS_UNKNOWN,
					"[HANDSHAKE DONE] Session was reused");
			} else {
				LogDebugTLS(TLS_UNKNOWN,
					"[HANDSHAKE DONE] New session created");
			}
		}

		/* Check for SNI */
		const char *servername =
			SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
		if (servername) {
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] SNI hostname: %s",
				    servername);
		}

		/* Check for ALPN */
		const unsigned char *alpn_selected;
		unsigned int alpn_len;

		SSL_get0_alpn_selected(ssl, &alpn_selected, &alpn_len);
		if (alpn_selected && alpn_len > 0) {
			char alpn_str[256];

			snprintf(alpn_str, sizeof(alpn_str), "%.*s", alpn_len,
				 alpn_selected);
			LogDebugTLS(TLS_UNKNOWN,
				    "[HANDSHAKE DONE] ALPN protocol: %s",
				    alpn_str);
		}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
		/* Check KTLS status */
		BIO *wbio = SSL_get_wbio(ssl);
		BIO *rbio = SSL_get_rbio(ssl);

		if (wbio && rbio) {
			int ktls_send = BIO_get_ktls_send(wbio);
			int ktls_recv = BIO_get_ktls_recv(rbio);

			LogDebugTLS(TLS_UNKNOWN,
				"[HANDSHAKE DONE] KTLS status - send: %s, recv: %s",
				ktls_send ? "enabled" : "disabled",
				ktls_recv ? "enabled" : "disabled");
		}
#endif

		return;
	}

	/* Alert handling with detailed information */
	if (where & SSL_CB_ALERT) {
		direction = (where & SSL_CB_READ) ? "read" : "write";
		int level = ret >> 8;
		int desc = ret & 0xff;
		const char *level_str = (level == 1) ? "warning" : "fatal";
		const char *alert_desc = SSL_alert_desc_string_long(desc);
		const char *alert_type = SSL_alert_type_string_long(ret);

		LogWarnTLS(TLS_UNKNOWN,
			"[ALERT] %s: level=%s, desc=%" PRId32
			" (%s) type=(%s) state=(%s)",
			direction, level_str, desc,
			alert_desc ? alert_desc : "unknown",
			alert_type ? alert_type : "unknown", str);

		/* Log additional context for specific alerts */
		switch (desc) {
		case SSL_AD_CLOSE_NOTIFY:
			LogDebugTLS(TLS_UNKNOWN,
				"[ALERT] Connection being closed gracefully");
			break;
		case SSL_AD_HANDSHAKE_FAILURE:
			LogDebugTLS(TLS_UNKNOWN,
				"[ALERT] Handshake failure - check cipher compatibility");
			break;
		case SSL_AD_BAD_CERTIFICATE:
			LogDebugTLS(TLS_UNKNOWN,
				"[ALERT] Bad certificate - certificate validation failed");
			break;
		case SSL_AD_CERTIFICATE_EXPIRED:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Certificate has expired");
			break;
		case SSL_AD_CERTIFICATE_UNKNOWN:
			LogDebugTLS(TLS_UNKNOWN,
				"[ALERT] Certificate unknown or not trusted");
			break;
		case SSL_AD_ILLEGAL_PARAMETER:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Illegal parameter in handshake");
			break;
		case SSL_AD_DECODE_ERROR:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Message decode error");
			break;
		case SSL_AD_DECRYPT_ERROR:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Message decryption error");
			break;
		case SSL_AD_PROTOCOL_VERSION:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Protocol version not supported");
			break;
		case SSL_AD_INSUFFICIENT_SECURITY:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Insufficient security level");
			break;
		case SSL_AD_INTERNAL_ERROR:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Internal error occurred");
			break;
		case SSL_AD_USER_CANCELLED:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] User cancelled the operation");
			break;
		case SSL_AD_NO_RENEGOTIATION:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Renegotiation not allowed");
			break;
		case SSL_AD_UNSUPPORTED_EXTENSION:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Unsupported extension");
			break;
		case SSL_AD_CERTIFICATE_UNOBTAINABLE:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Certificate unobtainable");
			break;
		case SSL_AD_UNRECOGNIZED_NAME:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Unrecognized name (SNI mismatch)");
			break;
		case SSL_AD_BAD_CERTIFICATE_STATUS_RESPONSE:
			LogDebugTLS(TLS_UNKNOWN,
				"[ALERT] Bad certificate status response (OCSP)");
			break;
		case SSL_AD_BAD_CERTIFICATE_HASH_VALUE:
			LogDebugTLS(TLS_UNKNOWN,
				    "[ALERT] Bad certificate hash value");
			break;
		default:
			LogDebugTLS(TLS_UNKNOWN, "[ALERT] Alert code %" PRId32
				    , desc);
			break;
		}
		return;
	}

	/* State-based categorization */
	if (where & SSL_CB_LOOP) {
		if (handshake_started) {
			prefix = "[HANDSHAKE LOOP]";
		} else {
			prefix = "[LOOP]";
		}

		/* Detailed state analysis */
		if (strstr(str, "read") || strstr(str, "Read")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Reading data/handshake messages]",
				    prefix, str);
		} else if (strstr(str, "write") || strstr(str, "Write")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Writing data/handshake messages]",
				    prefix, str);
		} else if (strstr(str, "certificate") ||
			   strstr(str, "Certificate")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Certificate processing]", prefix,
				    str);
		} else if (strstr(str, "key") || strstr(str, "Key")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Key exchange/processing]", prefix,
				    str);
		} else if (strstr(str, "cipher") || strstr(str, "Cipher")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Cipher negotiation/setup]",
				    prefix, str);
		} else if (strstr(str, "finished") || strstr(str, "Finished")) {
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Handshake finishing]", prefix,
				    str);
		} else {
			LogWarnTLS(TLS_UNKNOWN, "%s : %s", prefix, str);
		}
		return;
	}

	if (where & SSL_CB_EXIT) {
		if (ret == 0) {
			prefix = "[EXIT] failed";
			LogWarnTLS(TLS_UNKNOWN, "%s : %s [Operation failed]",
				   prefix, str);

			/* Log additional error information */
			unsigned long err = ERR_peek_last_error();

			if (err != 0) {
				char err_buf[256];

				ERR_error_string_n(err, err_buf,
						   sizeof(err_buf));
				LogDebugTLS(TLS_UNKNOWN,
					    "[EXIT] Last error: %s", err_buf);
			}
		} else if (ret < 0) {
			prefix = "[EXIT] error";
			LogWarnTLS(TLS_UNKNOWN,
				   "%s : %s [Error condition, ret=%"
				   PRId32 "]", prefix, str, ret);

			/* Log SSL error details */
			int ssl_err = SSL_get_error(ssl, ret);
			const char *ssl_err_str = "";

			switch (ssl_err) {
			case SSL_ERROR_NONE:
				ssl_err_str = "SSL_ERROR_NONE";
				break;
			case SSL_ERROR_SSL:
				ssl_err_str = "SSL_ERROR_SSL";
				break;
			case SSL_ERROR_WANT_READ:
				ssl_err_str = "SSL_ERROR_WANT_READ";
				break;
			case SSL_ERROR_WANT_WRITE:
				ssl_err_str = "SSL_ERROR_WANT_WRITE";
				break;
			case SSL_ERROR_WANT_X509_LOOKUP:
				ssl_err_str = "SSL_ERROR_WANT_X509_LOOKUP";
				break;
			case SSL_ERROR_SYSCALL:
				ssl_err_str = "SSL_ERROR_SYSCALL";
				break;
			case SSL_ERROR_ZERO_RETURN:
				ssl_err_str = "SSL_ERROR_ZERO_RETURN";
				break;
			case SSL_ERROR_WANT_CONNECT:
				ssl_err_str = "SSL_ERROR_WANT_CONNECT";
				break;
			case SSL_ERROR_WANT_ACCEPT:
				ssl_err_str = "SSL_ERROR_WANT_ACCEPT";
				break;
			default:
				ssl_err_str = "SSL_ERROR_UNKNOWN";
				break;
			}
			LogDebugTLS(TLS_UNKNOWN, "[EXIT] SSL error: %s (%"
				    PRId32 ")", ssl_err_str, ssl_err);
		} else {
			prefix = "[EXIT] ok";
			LogDebugTLS(TLS_UNKNOWN,
				    "%s : %s [Operation successful]", prefix,
				    str);
		}
		return;
	}

	/* Read/Write operations */
	if (where & SSL_CB_READ) {
		prefix = "[READ]";
		LogDebugTLS(TLS_UNKNOWN, "%s : %s [Reading TLS data]", prefix,
			    str);
		return;
	}

	if (where & SSL_CB_WRITE) {
		prefix = "[WRITE]";
		LogDebugTLS(TLS_UNKNOWN, "%s : %s [Writing TLS data]", prefix,
			    str);
		return;
	}

	/* Accept loop (server-specific) */
	if (where & SSL_CB_ACCEPT_LOOP) {
		prefix = "[ACCEPT LOOP]";
		LogDebugTLS(TLS_UNKNOWN,
			    "%s : %s [Server accepting connection]", prefix,
			    str);
		return;
	}

	/* Connect loop (client-specific) */
	if (where & SSL_CB_CONNECT_LOOP) {
		prefix = "[CONNECT LOOP]";
		LogDebugTLS(TLS_UNKNOWN, "%s : %s [Client connecting]", prefix,
			    str);
		return;
	}

	/* Default case - analyze the state string for more context */
	if (strstr(str, "error") || strstr(str, "Error") ||
	    strstr(str, "failed")) {
		prefix = "[ERROR]";
	} else if (strstr(str, "certificate") || strstr(str, "Certificate")) {
		prefix = "[CERTIFICATE]";
	} else if (strstr(str, "key") || strstr(str, "Key")) {
		prefix = "[KEY EXCHANGE]";
	} else if (strstr(str, "cipher") || strstr(str, "Cipher")) {
		prefix = "[CIPHER]";
	} else if (strstr(str, "session") || strstr(str, "Session")) {
		prefix = "[SESSION]";
	} else if (strstr(str, "version") || strstr(str, "Version")) {
		prefix = "[VERSION]";
	} else if (strstr(str, "extension") || strstr(str, "Extension")) {
		prefix = "[EXTENSION]";
	} else {
		prefix = "[INFO]";
	}

	LogWarnTLS(TLS_UNKNOWN, "%s : %s", prefix, str);
}
