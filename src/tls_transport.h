/* Backend TLS library operations (implemented in tls_openssl.c / tls_gnutls.c) */
extern tls_ctx_t *tls_ctx_init(int fd, tls_cred_t *cred, bool is_server);
extern tls_cred_t *tls_cred_init(const char *cert_file, const char *key_file,
		const char *ca_file, const char *ciphers,
		const char *min_version, bool ktls, bool debug);
extern bool tls_handshake(tls_ctx_t *ctx);
extern int tls_recv(tls_ctx_t *ctx, void *buf, size_t len, int flags);
extern int tls_send(tls_ctx_t *ctx, const struct msghdr *msg, int flags);
extern int tls_datapending(tls_ctx_t *ctx);
extern bool tls_close(tls_ctx_t *ctx);
extern bool tls_verify_peer(tls_ctx_t *ctx, char *peer_identity,
		size_t id_size);
extern bool tls_key_update(tls_ctx_t *ctx);
extern bool tls_get_type(tls_ctx_t *ctx);

/* Transport-layer TLS wiring (implemented in tls_transport.c) */
extern bool xprt_tls_init(const char *cert_file, const char *key_file,
		const char *ca_file, const char *ciphers,
		const char *min_version, bool ktls, bool debug);
extern bool svc_tls_init_xprt(SVCXPRT *xprt);
