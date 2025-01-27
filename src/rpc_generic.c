/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code mut retain the above copyright notice,
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
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

#include "config.h"
#include <sys/cdefs.h>

/*
 * rpc_generic.c, Miscl routines for RPC.
 *
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <netdb.h>
#ifdef RPC_VSOCK
#include <linux/vm_sockets.h>
#endif /* VSOCK */
#include <netconfig.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/nettype.h>
#include <assert.h>

#include "rpc_com.h"
#include "strl.h"

void
thr_keyfree(void *k)
{
	mem_free(k, 0);	/* XXX */
}

static void
tirpc_thread_name(const char *p)
{
	/* do nothing */
}

void tirpc_mon_thread_name(uint32_t p, int32_t q)
{
	/* do nothing */
}
static void
tirpc_free(void *p, size_t unused)
{
	free(p);
}

static void *
tirpc_malloc(size_t size, const char *file, int line, const char *function)
{
	void *r = malloc(size);

	assert(r != NULL);
	return r;
}

static void *
tirpc_aligned(size_t alignment, size_t size, const char *file, int line,
	      const char *function)
{
	void *r;

#if defined(_ISOC11_SOURCE)
	r = aligned_alloc(alignment, size);
#else
	(void) posix_memalign(&r, alignment, size);
#endif
	assert(r != NULL);
	return r;
}

static void *
tirpc_calloc(size_t count, size_t size, const char *file, int line,
	     const char *function)
{
	void *r = calloc(count, size);

	assert(r != NULL);
	return r;
}

static void *
tirpc_realloc(void *p, size_t size, const char *file, int line,
	      const char *function)
{
	void *r = realloc(p, size);

	assert(r != NULL);
	return r;
}

tirpc_pkg_params __ntirpc_pkg_params = {
	TIRPC_FLAG_NONE,
	TIRPC_DEBUG_FLAG_NONE,
	tirpc_thread_name,
	warnx,
	tirpc_free,
	tirpc_malloc,
	tirpc_aligned,
	tirpc_calloc,
	tirpc_realloc,
	tirpc_mon_thread_name,
};

bool
tirpc_control(const u_int rq, void *in)
{
	switch (rq) {
	case TIRPC_GET_PARAMETERS:
		*(tirpc_pkg_params *)in = __ntirpc_pkg_params;
		break;
	case TIRPC_PUT_PARAMETERS:
		__ntirpc_pkg_params = *(tirpc_pkg_params *)in;
		break;
	case TIRPC_GET_DEBUG_FLAGS:
		*(u_int *) in = __ntirpc_pkg_params.debug_flags;
		break;
	case TIRPC_SET_DEBUG_FLAGS:
		__ntirpc_pkg_params.debug_flags = *(int *)in;
		break;
	case TIRPC_GET_OTHER_FLAGS:
		*(u_int *) in = __ntirpc_pkg_params.other_flags;
		break;
	case TIRPC_SET_OTHER_FLAGS:
		__ntirpc_pkg_params.other_flags = *(int *)in;
		break;
	default:
		return (false);
	}
	return (true);
}

struct handle {
	NCONF_HANDLE *nhandle;
	int nflag;		/* Whether NETPATH or NETCONFIG */
	int nettype;
};

struct _rpcnettype {
	const char *name;
	const int type;
};

static const struct _rpcnettype _rpctypelist[] = {
	{"netpath", _RPC_NETPATH},
	{"visible", _RPC_VISIBLE},
	{"circuit_v", _RPC_CIRCUIT_V},
	{"datagram_v", _RPC_DATAGRAM_V},
	{"circuit_n", _RPC_CIRCUIT_N},
	{"datagram_n", _RPC_DATAGRAM_N},
	{"tcp", _RPC_TCP},
	{"udp", _RPC_UDP},
	{"vsock", _RPC_VSOCK},
	{0, _RPC_NONE}
};

struct netid_af {
	const char *netid;
	int af;
	int protocol;
};

static const struct netid_af na_cvt[] = {
	{"udp", AF_INET, IPPROTO_UDP},
	{"tcp", AF_INET, IPPROTO_TCP},
#ifdef INET6
	{"udp6", AF_INET6, IPPROTO_UDP},
	{"tcp6", AF_INET6, IPPROTO_TCP},
#endif
#ifdef RPC_VSOCK
	{"vsock", AF_VSOCK, PF_VSOCK},
#endif /* VSOCK */
	{"local", AF_LOCAL, 0}
};

#if 0
static char *strlocase(char *);
#endif
static int getnettype(const char *);

/*
 * Cache the result of getrlimit(), so we don't have to do an
 * expensive call every time.
 */
int
__rpc_dtbsize(void)
{
	static int tbsize;
	struct rlimit rl;

	if (tbsize)
		return (tbsize);
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
		return (tbsize = (int)rl.rlim_max);
	/*
	 * Something wrong.  I'll try to save face by returning a
	 * pessimistic number.
	 */
	return (32);
}

/*
 * Find the appropriate buffer size
 */
u_int /*ARGSUSED*/
__rpc_get_t_size(int af, int proto, int size)
{
	int maxsize, defsize;

	maxsize = 256 * 1024;	/* XXX */
	switch (proto) {
	case IPPROTO_TCP:
		defsize = 64 * 1024;	/* XXX */
		break;
	case IPPROTO_UDP:
		defsize = UDPMSGSIZE;
		break;
#ifdef RPC_VSOCK
	case PF_VSOCK:
		defsize = 64 * 1024;	/* XXX */
		break;
#endif /* VSOCK */
	default:
		defsize = RPC_MAXDATASIZE; /* 9000 */
		break;
	}
	if (size == 0)
		return defsize;

	/* Check whether the value is within the upper max limit */
	return (size > maxsize ? (u_int) maxsize : (u_int) size);
}

/*
 * Find the appropriate address buffer size
 */
u_int
__rpc_get_a_size(int af)
{
	switch (af) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
#ifdef INET6
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
#endif
#ifdef RPC_VSOCK
		/* Linux */
	case AF_VSOCK:
		return sizeof(struct sockaddr_vm);
#endif /* VSOCK */
	case AF_LOCAL:
		return sizeof(struct sockaddr_un);
	default:
		break;
	}

	return ((u_int) RPC_MAXADDRSIZE);
}

#if 0
static char *
strlocase(char *p)
{
	char *t = p;

	for (; *p; p++)
		if (isupper(*p))
			*p = tolower(*p);
	return (t);
}
#endif

/*
 * Returns the type of the network as defined in <rpc/nettype.h>
 * If nettype is NULL, it defaults to NETPATH.
 */
static int
getnettype(const char *nettype)
{
	int i;

	if ((nettype == NULL) || (nettype[0] == 0))
		return (_RPC_NETPATH);	/* Default */
#if 0
	nettype = strlocase(nettype);
#endif
	for (i = 0; _rpctypelist[i].name; i++) {
		if (strcasecmp(nettype, _rpctypelist[i].name) == 0)
			return (_rpctypelist[i].type);
	}

	return (_rpctypelist[i].type);
}

/*
 * For the given nettype (tcp or udp only), return the first structure found.
 * This should be freed by calling freenetconfigent()
 */
struct netconfig *
__rpc_getconfip(const char *nettype)
{
	char *netid;
	char *netid_tcp = (char *)NULL;
	char *netid_udp = (char *)NULL;
	char *netid_vsock = (char *)NULL;
	struct netconfig *dummy;
	extern thread_key_t tcp_key, udp_key, vsock_key;
	extern mutex_t tsd_lock;

	if (tcp_key == -1) {
		mutex_lock(&tsd_lock);
		if (tcp_key == -1)
			thr_keycreate(&tcp_key, thr_keyfree);
		mutex_unlock(&tsd_lock);
	}
	netid_tcp = (char *)thr_getspecific(tcp_key);
	if (udp_key == -1) {
		mutex_lock(&tsd_lock);
		if (udp_key == -1)
			thr_keycreate(&udp_key, thr_keyfree);
		mutex_unlock(&tsd_lock);
	}
	netid_udp = (char *)thr_getspecific(udp_key);
	if (vsock_key == -1) {
		mutex_lock(&tsd_lock);
		if (vsock_key == -1)
			thr_keycreate(&vsock_key, free);
		mutex_unlock(&tsd_lock);
	}
	netid_vsock = (char *)thr_getspecific(vsock_key);
	if (!netid_udp && !netid_tcp) {
		struct netconfig *nconf;
		void *confighandle;

		confighandle = setnetconfig();
		if (!confighandle) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: setnetconfig failed to open %s",
				__func__, NETCONFIG);
			return (NULL);
		}
		while ((nconf = getnetconfig(confighandle)) != NULL) {
			if (strcmp(nconf->nc_protofmly, NC_INET) == 0
			    || strcmp(nconf->nc_protofmly, NC_INET6) == 0) {
				if (strcmp(nconf->nc_proto, NC_TCP) == 0
				    && netid_tcp == NULL) {
					netid_tcp = mem_strdup(nconf->nc_netid);
					thr_setspecific(tcp_key,
							(void *)netid_tcp);
				} else if (strcmp(nconf->nc_proto, NC_UDP) == 0
					   && netid_udp == NULL) {
					netid_udp = mem_strdup(nconf->nc_netid);
					thr_setspecific(udp_key,
							(void *)netid_udp);
				}
			} /* NC_INET || NC_INET6 */
			if (strcmp(nconf->nc_protofmly, NC_VSOCK) == 0) {
				if (netid_vsock == NULL) {
					netid_vsock =
						mem_strdup(nconf->nc_netid);
					thr_setspecific(vsock_key,
							(void *)netid_vsock);
				}
			} /* VSOCK */
		}
		endnetconfig(confighandle);
	}
	if (strcmp(nettype, "udp") == 0)
		netid = netid_udp;
	else if (strcmp(nettype, "tcp") == 0)
		netid = netid_tcp;
	else if (strcmp(nettype, "vsock") == 0)
		netid = netid_vsock;
	else
		return (NULL);
	if ((netid == NULL) || (netid[0] == 0))
		return (NULL);
	dummy = getnetconfigent(netid);

	return (dummy);
}

/*
 * Returns the type of the nettype, which should then be used with
 * __rpc_getconf().
 */
void *
__rpc_setconf(const char *nettype)
{
	struct handle *handle = (struct handle *)mem_zalloc(sizeof(*handle));

	switch (handle->nettype = getnettype(nettype)) {
	case _RPC_NETPATH:
	case _RPC_CIRCUIT_N:
	case _RPC_DATAGRAM_N:
		handle->nhandle = setnetpath();
		if (!handle->nhandle) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: setnetpath failed",
				__func__);
			mem_free(handle, sizeof(*handle));
			return (NULL);
		}
		handle->nflag = true;
		break;
	case _RPC_VISIBLE:
	case _RPC_CIRCUIT_V:
	case _RPC_DATAGRAM_V:
	case _RPC_TCP:
	case _RPC_UDP:
	case _RPC_VSOCK:
		handle->nhandle = setnetconfig();
		if (!handle->nhandle) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: setnetconfig failed to open %s",
				__func__, NETCONFIG);
			mem_free(handle, sizeof(*handle));
			return (NULL);
		}
		handle->nflag = false;
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: unknown net type %s",
			__func__, nettype);
		mem_free(handle, sizeof(*handle));
		return (NULL);
	}

	return (handle);
}

/*
 * Returns the next netconfig struct for the given "net" type.
 * __rpc_setconf() should have been called previously.
 */
struct netconfig *
__rpc_getconf(void *vhandle)
{
	struct netconfig *nconf;
	struct handle *handle = (struct handle *)vhandle;

	if (handle == NULL)
		return (NULL);
	for (;;) {
		if (handle->nflag)
			nconf = getnetpath(handle->nhandle);
		else
			nconf = getnetconfig(handle->nhandle);
		if (nconf == NULL)
			break;
		if ((nconf->nc_semantics != NC_TPI_CLTS)
		    && (nconf->nc_semantics != NC_TPI_COTS)
		    && (nconf->nc_semantics != NC_TPI_COTS_ORD))
			continue;
		switch (handle->nettype) {
		case _RPC_VISIBLE:
			if (!(nconf->nc_flag & NC_VISIBLE))
				continue;
			/* FALLTHROUGH */
		case _RPC_NETPATH:	/* Be happy */
			break;
		case _RPC_CIRCUIT_V:
			if (!(nconf->nc_flag & NC_VISIBLE))
				continue;
			/* FALLTHROUGH */
		case _RPC_CIRCUIT_N:
			if ((nconf->nc_semantics != NC_TPI_COTS)
			    && (nconf->nc_semantics != NC_TPI_COTS_ORD))
				continue;
			break;
		case _RPC_DATAGRAM_V:
			if (!(nconf->nc_flag & NC_VISIBLE))
				continue;
			/* FALLTHROUGH */
		case _RPC_DATAGRAM_N:
			if (nconf->nc_semantics != NC_TPI_CLTS)
				continue;
			break;
		case _RPC_TCP:
			if (((nconf->nc_semantics != NC_TPI_COTS)
			     && (nconf->nc_semantics != NC_TPI_COTS_ORD))
			    || (strcmp(nconf->nc_protofmly, NC_INET)
#ifdef INET6
				&& strcmp(nconf->nc_protofmly, NC_INET6))
#else
			    )
#endif
			    || strcmp(nconf->nc_proto, NC_TCP))
				continue;
			break;
		case _RPC_UDP:
			if ((nconf->nc_semantics != NC_TPI_CLTS)
			    || (strcmp(nconf->nc_protofmly, NC_INET)
#ifdef INET6
				&& strcmp(nconf->nc_protofmly, NC_INET6))
#else
			    )
#endif
			    || strcmp(nconf->nc_proto, NC_UDP))
				continue;
			break;
                case _RPC_VSOCK:
			/* accept stream sockets only, there is no valid
			 * proto (i.e., it must be "-") */
			if ((nconf->nc_semantics != NC_TPI_COTS_ORD) ||
				(strcmp(nconf->nc_protofmly, NC_VSOCK)) ||
				(strcmp(nconf->nc_proto, NC_NOPROTO)))
				continue;
			break;
		}
		break;
	}
	return (nconf);
}

void
__rpc_endconf(void *vhandle)
{
	struct handle *handle = (struct handle *)vhandle;

	if (handle == NULL)
		return;
	if (handle->nflag)
		endnetpath(handle->nhandle);
	else
		endnetconfig(handle->nhandle);
	mem_free(handle, sizeof(*handle));
}

static const struct timespec to = { 3, 0 };

/*
 * Used to ping the NULL procedure for clnt handle.
 * Returns NULL if fails, else a non-NULL pointer.
 */
void *
rpc_nullproc(CLIENT *clnt)
{
	struct clnt_req *cc = mem_alloc(sizeof(*cc));
	enum clnt_stat stat;

	clnt_req_fill(cc, clnt, authnone_ncreate(), NULLPROC,
		      (xdrproc_t) xdr_void, NULL,
		      (xdrproc_t) xdr_void, NULL);
	stat = clnt_req_setup(cc, to);
	if (stat == RPC_SUCCESS) {
		stat = CLNT_CALL_WAIT(cc);
	}
	if (stat != RPC_SUCCESS) {
		char *t = rpc_sperror(&cc->cc_error, __func__);

		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s", t);
		mem_free(t, RPC_SPERROR_BUFLEN);
		clnt_req_release(cc);
		return (NULL);
	}
	clnt_req_release(cc);

	return ((void *)clnt);
}

/*
 * Try all possible transports until
 * one succeeds in finding the netconf for the given fd.
 */
struct netconfig *
__rpcgettp(int fd)
{
	const char *netid;
	struct __rpc_sockinfo si;

	if (!__rpc_fd2sockinfo(fd, &si))
		return NULL;

	if (!__rpc_sockinfo2netid(&si, &netid))
		return NULL;

	/*LINTED const castaway */
	return getnetconfigent((char *)netid);
}

int
__rpc_fd2sockinfo(int fd, struct __rpc_sockinfo *sip)
{
	socklen_t len;
	int type, proto = 0;
	struct sockaddr_storage ss;

	len = sizeof(ss);
	if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0)
		return 0;
	sip->si_alen = len;

	len = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0)
		return 0;

	/* XXX */
	if (ss.ss_family != AF_LOCAL) {
		if (type == SOCK_STREAM) {
			switch (ss.ss_family) {
			case PF_INET:
			case PF_INET6:
				proto = IPPROTO_TCP;
				break;
#ifdef RPC_VSOCK
			case AF_VSOCK:
				proto = PF_VSOCK;
				break;
#endif /* VSOCK */
			}
		} else if (type == SOCK_DGRAM)
			proto = IPPROTO_UDP;
		else
			return 0;
	} else
		proto = 0;

	sip->si_af = ss.ss_family;
	sip->si_proto = proto;
	sip->si_socktype = type;

	return 1;
}

/*
 * Linear search, but the number of entries is small.
 */
int
__rpc_nconf2sockinfo(const struct netconfig *nconf,
		     struct __rpc_sockinfo *sip)
{
	int i;

	for (i = 0; i < (sizeof(na_cvt)) / (sizeof(struct netid_af)); i++)
		if (strcmp(na_cvt[i].netid, nconf->nc_netid) == 0
		    || (strcmp(nconf->nc_netid, "unix") == 0
			&& strcmp(na_cvt[i].netid, "local") == 0)) {
			sip->si_af = na_cvt[i].af;
			sip->si_proto = na_cvt[i].protocol;
			sip->si_socktype =
			    __rpc_seman2socktype((int)nconf->nc_semantics);
			if (sip->si_socktype == -1)
				return 0;
			sip->si_alen = __rpc_get_a_size(sip->si_af);
			return 1;
		}

	return 0;
}

int
__rpc_nconf2fd_flags(const struct netconfig *nconf, int flags)
{
	struct __rpc_sockinfo si;
	int fd;

	if (!__rpc_nconf2sockinfo(nconf, &si))
		return 0;

	fd = socket(si.si_af, si.si_socktype | flags, si.si_proto);
	if ((fd >= 0) &&
	    (si.si_af == AF_INET6)) {
#ifdef SOL_IPV6
		int val = 1;
		(void) setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, &val,
				  sizeof(val));
#endif
	}

	return fd;
}

int
__rpc_nconf2fd(const struct netconfig *nconf)
{
	return __rpc_nconf2fd_flags(nconf, 0);
}

int
__rpc_sockinfo2netid(struct __rpc_sockinfo *sip, const char **netid)
{
	int i;
	struct netconfig *nconf;

	nconf = getnetconfigent("local");

	for (i = 0; i < (sizeof(na_cvt)) / (sizeof(struct netid_af)); i++) {
		if (na_cvt[i].af == sip->si_af
		    && na_cvt[i].protocol == sip->si_proto) {
			if (strcmp(na_cvt[i].netid, "local") == 0
			    && nconf == NULL) {
				if (netid)
					*netid = "unix";
			} else {
				if (netid)
					*netid = na_cvt[i].netid;
			}
			if (nconf != NULL)
				freenetconfigent(nconf);
			return 1;
		}
	}
	if (nconf != NULL)
		freenetconfigent(nconf);

	return 0;
}

char *
taddr2uaddr(const struct netconfig *nconf, const struct netbuf *nbuf)
{
	struct __rpc_sockinfo si;

	if (!__rpc_nconf2sockinfo(nconf, &si))
		return NULL;
	return __rpc_taddr2uaddr_af(si.si_af, nbuf);
}

struct netbuf *
uaddr2taddr(const struct netconfig *nconf, const char *uaddr)
{
	struct __rpc_sockinfo si;

	if (!__rpc_nconf2sockinfo(nconf, &si))
		return NULL;
	return __rpc_uaddr2taddr_af(si.si_af, uaddr);
}

char *
__rpc_taddr2uaddr_af(int af, const struct netbuf *nbuf)
{
	char *ret = NULL;
	struct sockaddr_in *sin;
	struct sockaddr_un *sun;
	char namebuf[INET_ADDRSTRLEN];
#ifdef INET6
	struct sockaddr_in6 *sin6;
	char namebuf6[INET6_ADDRSTRLEN];
#endif
	u_int16_t port;

	if (nbuf->len <= 0)
		goto out;

	/* XXX check (fix) this */
#define RETURN_SIZE (INET6_ADDRSTRLEN + (2 * 6) + 1)
	ret = mem_zalloc(RETURN_SIZE);

	switch (af) {
	case AF_INET:
		if (nbuf->len < sizeof(*sin)) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		sin = nbuf->buf;
		if (inet_ntop(af, &sin->sin_addr, namebuf, sizeof(namebuf))
		    == NULL) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		port = ntohs(sin->sin_port);
		if (sprintf
		    (ret, "%s.%u.%u", namebuf, ((u_int32_t) port) >> 8,
		     port & 0xff) < 0) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		break;
#ifdef INET6
	case AF_INET6:
		if (nbuf->len < sizeof(*sin6)) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		sin6 = nbuf->buf;
		if (inet_ntop(af, &sin6->sin6_addr, namebuf6, sizeof(namebuf6))
		    == NULL) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		port = ntohs(sin6->sin6_port);
		if (sprintf(ret, "%s.%u.%u",
				namebuf6,
				((u_int32_t) port) >> 8,
				port & 0xff) < 0) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		break;
#endif
#ifdef RPC_VSOCK
	case AF_VSOCK:
	{
		struct sockaddr_vm *svm = nbuf->buf;
		port = svm->svm_port;
		if (sprintf
			(ret, "%d.%u.%u", svm->svm_cid, ((u_int32_t)port) >> 8,
				port & 0xff) < 0) {
			mem_free(ret, 0);
			return NULL;
		}
	}
	break;
#endif /* VSOCK */
	case AF_LOCAL:
		sun = nbuf->buf;
		/* if (asprintf(&ret, "%.*s", (int)(sun->sun_len -
		   offsetof(struct sockaddr_un, sun_path)),
		   sun->sun_path) < 0) */
		if (sprintf(ret, "%.*s",
			    (int)(sizeof(*sun) -
				  offsetof(struct sockaddr_un, sun_path)),
			    sun->sun_path) < 0) {
			mem_free(ret, RETURN_SIZE);
			return NULL;
		}
		break;
	default:
		mem_free(ret, RETURN_SIZE);
		return NULL;
	}

 out:
	return ret;
}

struct netbuf *
__rpc_uaddr2taddr_af(int af, const char *uaddr)
{
	struct netbuf *ret = NULL;
	char *addrstr, *p;
	unsigned port, portlo, porthi;
	struct sockaddr_in *sin;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	struct sockaddr_un *sun;

	port = 0;
	sin = NULL;
	if (uaddr == NULL)
		return NULL;
	addrstr = mem_strdup(uaddr);

	/*
	 * AF_LOCAL addresses are expected to be absolute
	 * pathnames.
	 */
	if (*addrstr != '/') {
		p = strrchr(addrstr, '.');
		if (p == NULL)
			goto out;
		portlo = (unsigned)atoi(p + 1);
		*p = '\0';

		p = strrchr(addrstr, '.');
		if (p == NULL)
			goto out;
		porthi = (unsigned)atoi(p + 1);
		*p = '\0';
		port = (porthi << 8) | portlo;
	}

	ret = (struct netbuf *)mem_zalloc(sizeof(*ret));

	switch (af) {
	case AF_INET:
		sin = (struct sockaddr_in *)mem_zalloc(sizeof(*sin));

		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		if (inet_pton(AF_INET, addrstr, &sin->sin_addr) <= 0) {
			mem_free(sin, sizeof(*sin));
			mem_free(ret, sizeof(*ret));
			ret = NULL;
			goto out;
		}
		ret->maxlen = ret->len = sizeof(*sin);
		ret->buf = sin;
		break;
#ifdef INET6
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)mem_zalloc(sizeof(*sin6));

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(port);
		if (inet_pton(AF_INET6, addrstr, &sin6->sin6_addr) <= 0) {
			mem_free(sin6, sizeof(*sin6));
			mem_free(ret, sizeof(*ret));
			ret = NULL;
			goto out;
		}
		ret->maxlen = ret->len = sizeof(*sin6);
		ret->buf = sin6;
		break;
#endif
#ifdef RPC_VSOCK
	case AF_VSOCK:
	{
		struct sockaddr_in sin;
		struct sockaddr_vm *svm;
		svm = (struct sockaddr_vm *) mem_zalloc(
			sizeof(struct sockaddr_vm));
		if (svm == NULL)
			goto out;
		svm->svm_family = AF_VSOCK;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);
		if (inet_pton(AF_INET, addrstr, &sin.sin_addr) <= 0) {
			mem_free(svm, 0);
			mem_free(ret, 0);
			ret = NULL;
			goto out;
		}
		svm->svm_cid = sin.sin_addr.s_addr;
		svm->svm_port = port;
		ret->maxlen = ret->len = sizeof(struct sockaddr_vm);
		ret->buf = svm;
	}
	break;
#endif /* VSOCK */
	case AF_LOCAL:
		sun = (struct sockaddr_un *)mem_zalloc(sizeof(*sun));

		sun->sun_family = AF_LOCAL;
		strlcpy(sun->sun_path, addrstr, sizeof(sun->sun_path));
		ret->len = SUN_LEN(sun);
		ret->maxlen = sizeof(struct sockaddr_un);
		ret->buf = sun;
		break;
	default:
		mem_free(ret, sizeof(*ret));
		ret = NULL;
		break;
	}
 out:
	mem_free(addrstr, 0);
	return ret;
}

int
__rpc_seman2socktype(int semantics)
{
	switch (semantics) {
	case NC_TPI_CLTS:
		return SOCK_DGRAM;
	case NC_TPI_COTS_ORD:
		return SOCK_STREAM;
	case NC_TPI_RAW:
		return SOCK_RAW;
	default:
		break;
	}

	return -1;
}

int
__rpc_socktype2seman(int socktype)
{
	switch (socktype) {
	case SOCK_DGRAM:
		return NC_TPI_CLTS;
	case SOCK_STREAM:
		return NC_TPI_COTS_ORD;
	case SOCK_RAW:
		return NC_TPI_RAW;
	default:
		break;
	}

	return -1;
}

/*
 * XXXX - IPv6 scope IDs can't be handled in universal addresses.
 * Here, we compare the original server address to that of the RPC
 * service we just received back from a call to rpcbind on the remote
 * machine. If they are both "link local" or "site local", copy
 * the scope id of the server address over to the service address.
 */
int
__rpc_fixup_addr(struct netbuf *new, const struct netbuf *svc)
{
#ifdef INET6
	struct sockaddr *sa_new, *sa_svc;
	struct sockaddr_in6 *sin6_new, *sin6_svc;

	sa_svc = (struct sockaddr *)svc->buf;
	sa_new = (struct sockaddr *)new->buf;

	if (sa_new->sa_family == sa_svc->sa_family
	    && sa_new->sa_family == AF_INET6) {
		sin6_new = (struct sockaddr_in6 *)new->buf;
		sin6_svc = (struct sockaddr_in6 *)svc->buf;

		if ((IN6_IS_ADDR_LINKLOCAL(&sin6_new->sin6_addr)
		     && IN6_IS_ADDR_LINKLOCAL(&sin6_svc->sin6_addr))
		    || (IN6_IS_ADDR_SITELOCAL(&sin6_new->sin6_addr)
			&& IN6_IS_ADDR_SITELOCAL(&sin6_svc->sin6_addr))) {
			sin6_new->sin6_scope_id = sin6_svc->sin6_scope_id;
		}
	}
#endif
	return 1;
}

int __rpc_sockisbound(int fd)
{
	struct sockaddr_storage ss;
	union {
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
		struct sockaddr_un usin;
#ifdef RPC_VSOCK
		struct sockaddr_vm svm;
#endif /* VSOCK */
	} u_addr;
	socklen_t slen;

	slen = sizeof(struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0)
		return 0;

	switch (ss.ss_family) {
	case AF_INET:
		memcpy(&u_addr.sin, &ss, sizeof(u_addr.sin));
		return (u_addr.sin.sin_port != 0);
#ifdef INET6
	case AF_INET6:
		memcpy(&u_addr.sin6, &ss, sizeof(u_addr.sin6));
		return (u_addr.sin6.sin6_port != 0);
#endif
#ifdef RPC_VSOCK
	case AF_VSOCK:
		memcpy(&u_addr.svm, &ss, sizeof(u_addr.svm));
		return (u_addr.svm.svm_port != 0);
#endif /* VSOCK */
	case AF_LOCAL:
		/* XXX check this */
		memcpy(&u_addr.usin, &ss, sizeof(u_addr.usin));
		return (u_addr.usin.sun_path[0] != 0);
	default:
		break;
	}

	return 0;
}

/*
 * Helper function to set up a netbuf
 */
struct netbuf *
__rpc_set_netbuf(struct netbuf *nb, const void *ptr, size_t len)
{
	if (nb->len != len) {
		if (nb->len)
			mem_free(nb->buf, nb->len);
		nb->buf = mem_zalloc(len);
		nb->maxlen = nb->len = len;
	}
	memcpy(nb->buf, ptr, len);
	return nb;
}
