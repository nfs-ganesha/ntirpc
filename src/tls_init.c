#include "tls.h"
#include "tls_transport.h"

tls_config_t tls_config;

/* Initialize TLS from configuration */
bool tls_init(tls_config_t from_config)
{
	tls_config = from_config;
	if (!tls_config.enabled) {
		LogDebugTLS(TLS_INIT, "TLS is disabled in configuration");
		return true;
	}

	LogDebugTLS(TLS_INIT, "Initializing TLS with cert=%s, key=%s, ca=%s",
		    tls_config.cert_file, tls_config.key_file,
		    tls_config.ca_file ? tls_config.ca_file : "none");

	/* Initialize TLS library */
	if (!xprt_tls_init(tls_config.cert_file, tls_config.key_file,
			   tls_config.ca_file, tls_config.ciphers,
			   tls_config.min_version, tls_config.ktls,
			   tls_config.debug)) {
		LogCritTLS(TLS_INIT, "Failed to initialize TLS");
		return false;
	}

	LogDebugTLS(TLS_INIT, "TLS initialized successfully");
	return true;
}

/* Need to extend this to shutdown path for safe cleanup */
void tls_cleanup(void)
{
	if (!tls_config.enabled)
		return;

	/* Free configuration strings */
	if (tls_config.cert_file)
		free(tls_config.cert_file);

	if (tls_config.key_file)
		free(tls_config.key_file);

	if (tls_config.ca_file)
		free(tls_config.ca_file);

	if (tls_config.ciphers)
		free(tls_config.ciphers);

	if (tls_config.min_version)
		free(tls_config.min_version);

	tls_config.cert_file = NULL;
	tls_config.key_file = NULL;
	tls_config.ca_file = NULL;
	tls_config.ciphers = NULL;
	tls_config.min_version = NULL;
	LogDebugTLS(TLS_INIT, "TLS resources cleaned up");
}
