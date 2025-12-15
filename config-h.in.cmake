/* config.h file expanded by Cmake for build */

#include "ntirpc/version.h"

#ifndef CONFIG_H
#define CONFIG_H

/* Build controls */

#cmakedefine _MSPAC_SUPPORT 1
#cmakedefine HAVE_STDBOOL_H 1
#cmakedefine HAVE_KRB5 1
#cmakedefine LINUX 1
#cmakedefine BSDBASED 1
#cmakedefine _HAVE_GSSAPI 1
#cmakedefine HAVE_STRING_H 1
#cmakedefine HAVE_STRINGS_H 1
#cmakedefine LITTLEEND 1
#cmakedefine BIGEND 1
#cmakedefine TIRPC_EPOLL 1
#cmakedefine USE_RPC_RDMA 1
#cmakedefine USE_LTTNG_NTIRPC 1
#cmakedefine USE_MONITORING 1
#cmakedefine USE_TLS 1
#cmakedefine USE_OPENSSL 1
#cmakedefine USE_GNUTLS 1

/* Package stuff */
#define PACKAGE "libntirpc"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "libntirpc"
#define PACKAGE_STRING "libntirpc ${NTIRPC_VERSION_MAJOR}${NTIRPC_VERSION_MINOR}"
#define PACKAGE_TARNAME "libntirpc"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "${NTIRPC_VERSION_MAJOR}${NTIRPC_VERSION_MINOR}"

#endif /* CONFIG_H */
