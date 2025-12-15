/*	$NetBSD: auth.h,v 1.15 2000/06/02 22:57:55 fvdl Exp $	*/

/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2012-2017 Red Hat, Inc. and/or its affiliates.
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
 *
 * from: @(#)auth.h 1.17 88/02/08 SMI
 * from: @(#)auth.h 2.3 88/08/07 4.0 RPCSRC
 * from: @(#)auth.h 1.43  98/02/02 SMI
 * $FreeBSD: src/include/rpc/auth.h,v 1.20 2003/01/01 18:48:42 schweikh Exp $
 */

/*
 * auth.h, Authentication interface.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * The data structures are completely opaque to the client.  The client
 * is required to pass an AUTH * to routines that create rpc
 * "sessions".
 */

#ifndef _TIRPC_AUTH_H
#define  _TIRPC_AUTH_H

#include <rpc/xdr.h>
#include <rpc/rpc_err.h>
#include <misc/abstract_atomic.h>

#include <sys/cdefs.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif
#include <sys/types.h>

#define MAX_AUTH_BYTES 400	/* maximum length of opaque auth */
#define MAXNETNAMELEN 255	/* maximum length of network user's name */

/*
 *  Client side authentication/security data
 */

typedef struct sec_data {
	u_int secmod;		/* security mode number e.g. in nfssec.conf */
	u_int rpcflavor;	/* rpc flavors:AUTH_UNIX,AUTH_DES,RPCSEC_GSS */
	int flags;		/* AUTH_F_xxx flags */
	void *data;		/* opaque data per flavor */
} sec_data_t;

#ifdef _SYSCALL32_IMPL
struct sec_data32 {
	uint32_t secmod;	/* security mode number e.g. in nfssec.conf */
	uint32_t rpcflavor;	/* rpc flavors:AUTH_UNIX,AUTH_DES,RPCSEC_GSS */
	int32_t flags;		/* AUTH_F_xxx flags */
	void *data;		/* opaque data per flavor */
};
#endif				/* _SYSCALL32_IMPL */

/*
 * authentication/security specific flags
 */
#define AUTH_F_RPCTIMESYNC 0x001	/* use RPC to do time sync */
#define AUTH_F_TRYNONE  0x002	/* allow fall back to AUTH_NONE */

typedef u_int32_t u_int32;	/* 32-bit unsigned integers */

union des_block {
	struct {
		u_int32_t high;
		u_int32_t low;
	} key;
	char c[8];
};
typedef union des_block des_block;
__BEGIN_DECLS
extern bool xdr_des_block(XDR *, des_block *);
__END_DECLS
/*
 * Authentication info.  Opaque to client.
 */
struct opaque_auth {
	enum_t oa_flavor;	/* flavor of auth */
	u_int oa_length;	/* not to exceed MAX_AUTH_BYTES */
	char oa_body[MAX_AUTH_BYTES];
};

/*
 * Auth handle, interface to client side authenticators.
 */
typedef struct __auth {
	struct auth_ops {
		/* nextverf */
		void (*ah_nextverf) (struct __auth *);

		/* serialize */
		 bool(*ah_marshal) (struct __auth *, XDR *);

		/* validate verifier */
		 bool(*ah_validate) (struct __auth *, struct opaque_auth *);

		/* refresh credentials */
		 bool(*ah_refresh) (struct __auth *, void *);

		/* destroy this structure */
		void (*ah_destroy) (struct __auth *);

		/* encode data for wire */
		 bool(*ah_wrap) (struct __auth *, XDR *, xdrproc_t, void *);

		/* decode data for wire */
		 bool(*ah_unwrap) (struct __auth *, XDR *, xdrproc_t, void *);
	} *ah_ops;

	union des_block ah_key;
	struct rpc_err ah_error;
	struct opaque_auth ah_cred;
	struct opaque_auth ah_verf;

	void *ah_private;
	int ah_refcnt;
} AUTH;

static inline int auth_get(AUTH *auth)
{
	return atomic_add_int32_t(&auth->ah_refcnt, 1);
}

static inline int auth_put(AUTH *auth)
{
	return atomic_sub_int32_t(&auth->ah_refcnt, 1);
}

/*
 * Authentication ops.
 * The ops and the auth handle provide the interface to the authenticators.
 *
 * AUTH *auth;
 * XDR *xdrs;
 * struct opaque_auth verf;
 */
#define AUTH_FAILURE(auth) ((auth)->ah_error.re_status != RPC_SUCCESS)
#define AUTH_SUCCESS(auth) ((auth)->ah_error.re_status == RPC_SUCCESS)

#define AUTH_NEXTVERF(auth)                     \
	((*((auth)->ah_ops->ah_nextverf))(auth))
#define auth_nextverf(auth)                     \
	((*((auth)->ah_ops->ah_nextverf))(auth))

#define AUTH_MARSHALL(auth, xdrs)			\
	((*((auth)->ah_ops->ah_marshal))(auth, xdrs))
#define auth_marshall(auth, xdrs)			\
	((*((auth)->ah_ops->ah_marshal))(auth, xdrs))

#define AUTH_VALIDATE(auth, verfp)                      \
	((*((auth)->ah_ops->ah_validate))((auth), verfp))
#define auth_validate(auth, verfp)                      \
	((*((auth)->ah_ops->ah_validate))((auth), verfp))

#define AUTH_REFRESH(auth, msg)				\
	((*((auth)->ah_ops->ah_refresh))(auth, msg))
#define auth_refresh(auth, msg)				\
	((*((auth)->ah_ops->ah_refresh))(auth, msg))

#define AUTH_DESTROY(auth)                      \
	do {					\
		int refs = auth_put((auth));				\
		if (refs == 0)						\
			((*((auth)->ah_ops->ah_destroy))(auth));	\
		__warnx(TIRPC_DEBUG_FLAG_AUTH,				\
			"%s: auth_put(), refs %d\n",			\
			__func__, refs);				\
	} while (0)

#define auth_destroy(auth)						\
	do {								\
		int refs = auth_put((auth));				\
		if (refs == 0)						\
			((*((auth)->ah_ops->ah_destroy))(auth));	\
		__warnx(TIRPC_DEBUG_FLAG_AUTH,				\
			"%s: auth_put(), refs %d\n",			\
			__func__, refs);				\
	} while (0)

#define AUTH_WRAP(auth, xdrs, xfunc, xwhere)    \
	((*((auth)->ah_ops->ah_wrap))(auth, xdrs,	\
				      xfunc, xwhere))
#define auth_wrap(auth, xdrs, xfunc, xwhere)    \
	((*((auth)->ah_ops->ah_wrap))(auth, xdrs,	\
				      xfunc, xwhere))

#define AUTH_UNWRAP(auth, xdrs, xfunc, xwhere)  \
	((*((auth)->ah_ops->ah_unwrap))(auth, xdrs,	\
					xfunc, xwhere))
#define auth_unwrap(auth, xdrs, xfunc, xwhere)  \
	((*((auth)->ah_ops->ah_unwrap))(auth, xdrs,	\
					xfunc, xwhere))

__BEGIN_DECLS
extern struct opaque_auth _null_auth;
__END_DECLS
/*
 * Any style authentication.  These routines can be used by any
 * authentication style that does not use the wrap/unwrap functions.
 */
int authany_wrap(void), authany_unwrap(void);

/*
 * These are the various implementations of client side authenticators.
 *
 * Always returns AUTH. Must check ah_error.re_status,
 * followed by AUTH_DESTROY() as necessary.
 */

/*
 * System style authentication
 * AUTH *authunix_create(machname, uid, gid, len, aup_gids)
 * char *machname;
 * int uid;
 * int gid;
 * int len;
 * int *aup_gids;
 */
__BEGIN_DECLS
extern AUTH *authunix_ncreate(char *, uid_t, uid_t, int, uid_t *);
extern AUTH *authunix_ncreate_default(void);	/* takes no parameters */
extern AUTH *authnone_ncreate(void);	/* takes no parameters */
extern AUTH *authnone_ncreate_dummy(void);	/* takes no parameters */
__END_DECLS
/*
 * Netname manipulation routines.
 */
__BEGIN_DECLS
extern int getnetname(char *);
extern int host2netname(char *, const char *, const char *);
extern int user2netname(char *, const uid_t, const char *);
extern int netname2user(char *, uid_t *, gid_t *, int *, gid_t *);
extern int netname2host(char *, char *, const int);
extern void passwd2des(char *, char *);
__END_DECLS
/*
 *
 * These routines interface to the keyserv daemon
 *
 */
__BEGIN_DECLS
extern int key_decryptsession(const char *, des_block *);
extern int key_encryptsession(const char *, des_block *);
extern int key_gendes(des_block *);
extern int key_setsecret(const char *);
extern int key_secretkey_is_set(void);
extern int key_encryptsession_pk(char *, netobj *, des_block *);
__END_DECLS
/*
 * Publickey routines.
 */
__BEGIN_DECLS
extern int getpublickey(const char *, char *);
extern int getpublicandprivatekey(char *, char *);
extern int getsecretkey(char *, char *, char *);
__END_DECLS

__BEGIN_DECLS
struct svc_req;
enum auth_stat _svcauth_none(struct svc_req *);
enum auth_stat _svcauth_short(struct svc_req *);
enum auth_stat _svcauth_unix(struct svc_req *);
enum auth_stat _svcauth_gss(struct svc_req *, bool *);
enum auth_stat _svcauth_tls(struct svc_req *req);
__END_DECLS

#define AUTH_NONE 0		/* no authentication */
#define AUTH_NULL 0		/* backward compatibility */
#define AUTH_SYS 1		/* unix style (uid, gids) */
#define AUTH_UNIX AUTH_SYS
#define AUTH_SHORT 2		/* short hand unix style */
#define AUTH_DH  3		/* for Diffie-Hellman mechanism */
#define AUTH_DES AUTH_DH	/* for backward compatibility */
#define AUTH_KERB 4		/* kerberos style */
#define RPCSEC_GSS 6		/* RPCSEC_GSS */

/* AUTH_TLS authentication flavor - value from RFC */
#define AUTH_TLS    7  /* As defined in Section 7.1 of the RFC */

#endif				/* !_TIRPC_AUTH_H */
