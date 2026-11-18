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
 * rpc.h, Just includes the billions of rpc header files necessary to
 * do remote procedure calling.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */
#ifndef _TIRPC_RPC_H
#define _TIRPC_RPC_H

#include <rpc/types.h>		/* some typedefs */
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

/* external data representation interfaces */
#include <rpc/xdr.h>		/* generic (de)serializer */

/* Client side only authentication */
#include <rpc/auth.h>		/* generic authenticator (client side) */

/* Client side (mostly) remote procedure call */
#include <rpc/clnt.h>		/* generic rpc stuff */

/* semi-private protocol headers */
#include <rpc/rpc_msg.h>	/* protocol for rpc messages */
#include <rpc/auth_unix.h>	/* protocol for unix style cred */

#ifdef HAVE_RPCSEC_GSS
#include <rpc/auth_gss.h>	/* RPCSEC_GSS */
#endif

/* Server side only remote procedure callee */
#include <rpc/svc.h>		/* service manager and multiplexer */

#ifndef _KERNEL
#include <rpc/rpcb_clnt.h>	/* rpcbind interface functions */
#endif
#include <rpc/rpcent.h>

#ifndef UDPMSGSIZE
#define UDPMSGSIZE 8800
#endif

__BEGIN_DECLS
extern int bindresvport(int, struct sockaddr_in *);

char *taddr2uaddr(const struct netconfig *, const struct sockaddr_storage *);
struct sockaddr_storage *uaddr2taddr(const struct netconfig *, const char *);

struct sockaddr;
extern int bindresvport_sa(int, struct sockaddr *);

extern bool tirpc_control(const u_int, void *);
extern void *__mem_alloc(size_t);
extern void __mem_free(void *, size_t);

__END_DECLS
/*
 * The following are not exported interfaces, they are for internal library
 * and rpcbind use only. Do not use, they may change without notice.
 */
__BEGIN_DECLS
int __rpc_nconf2fd_flags_opt(const struct netconfig *, int, int);

static inline int
__rpc_nconf2fd_flags(const struct netconfig *nconf, int flags)
{
#ifdef SOL_IPV6
	int opts = IPV6_V6ONLY;
#else
	int opts = 0;
#endif

	return __rpc_nconf2fd_flags_opt(nconf, flags, opts);
}

static inline int
__rpc_nconf2fd(const struct netconfig *nconf)
{
	return __rpc_nconf2fd_flags(nconf, 0);
}

int __rpc_nconf2sockinfo(const struct netconfig *, struct __rpc_sockinfo *);
int __rpc_fd2sockinfo(int, struct __rpc_sockinfo *);
u_int __rpc_get_t_size(int, int, int);
__END_DECLS
#endif				/* !_RPC_RPC_H */
