/*
 * Copyright (c) 2010, Oracle America, Inc.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the "Oracle America, Inc." nor the names of its
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

#include "config.h"
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <err.h>
#include <rpc/rpc.h>
#include <rpc/nettype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "rpc_com.h"
#include "clnt_internal.h"

int __rpc_raise_fd(int);

int __rpc_minfd = 3;

#ifndef NETIDLEN
#define NETIDLEN 32
#endif

/* retry timeout default to the moon and back */
static const struct timespec to = { 3, 0 };

/*
 * Generic client creation with version checking the value of
 * vers_out is set to the highest server supported value
 * vers_low <= vers_out <= vers_high  AND an error results
 * if this can not be done.
 *
 * A NULL value for the timeout pointer indicates that the default
 * value for the timeout should be used.
 */
CLIENT *
clnt_ncreate_vers_timed(const char *hostname, rpcprog_t prog,
			rpcvers_t *vers_out, rpcvers_t vers_low,
			rpcvers_t vers_high, const char *nettype,
			const struct timeval *tp)
{
	CLIENT *clnt;
	struct clnt_req *cc;
	enum clnt_stat rpc_stat;

	clnt = clnt_ncreate_timed(hostname, prog, vers_high, nettype, tp);
	if (CLNT_FAILURE(clnt))
		return (clnt);

	cc = mem_alloc(sizeof(*cc));
	clnt_req_fill(cc, clnt, authnone_ncreate(), NULLPROC,
		      (xdrproc_t) xdr_void, NULL,
		      (xdrproc_t) xdr_void, NULL);
	rpc_stat = clnt_req_setup(cc, to);
	if (rpc_stat != RPC_SUCCESS) {
		goto error;
	}

	rpc_stat = CLNT_CALL_WAIT(cc);
	if (rpc_stat == RPC_SUCCESS) {
		clnt_req_release(cc);
		*vers_out = vers_high;
		return (clnt);
	}

	while (cc->cc_error.re_status == RPC_PROGVERSMISMATCH
	    && vers_high > vers_low) {
		unsigned int minvers, maxvers;

		minvers = cc->cc_error.re_vers.low;
		maxvers = cc->cc_error.re_vers.high;
		if (maxvers < vers_high)
			vers_high = maxvers;
		else
			vers_high--;
		if (minvers > vers_low)
			vers_low = minvers;
		if (vers_low > vers_high)
			break;
		CLNT_CONTROL(clnt, CLSET_VERS, (char *)&vers_high);

		clnt_req_reset(cc);
		rpc_stat = clnt_req_setup(cc, to);
		if (rpc_stat != RPC_SUCCESS) {
			break;
		}
		rpc_stat = CLNT_CALL_WAIT(cc);
		if (rpc_stat == RPC_SUCCESS) {
			clnt_req_release(cc);
			*vers_out = vers_high;
			return (clnt);
		}
	}

 error:
	clnt->cl_error = cc->cc_error;
	clnt_req_release(cc);
	return (clnt);
}

/*
 * Top level client creation routine.
 * Generic client creation: takes (servers name, program-number, nettype) and
 * returns client handle. Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s.
 *
 * It tries for all the netids in that particular class of netid until
 * it succeeds.
 * XXX The error message in the case of failure will be the one
 * pertaining to the last create error.
 *
 * A NULL value for the timeout pointer indicates that the default
 * value for the timeout should be used.
 */
CLIENT *
clnt_ncreate_timed(const char *hostname, rpcprog_t prog, rpcvers_t vers,
		   const char *netclass, const struct timeval *tp)
{
	struct netconfig *nconf;
	CLIENT *clnt;
	void *handle;
	struct rpc_err save_cf_error;
	char nettype_array[NETIDLEN];
	char *nettype = &nettype_array[0];

	if (netclass == NULL)
		nettype = NULL;
	else {
		size_t len = strlen(netclass);

		if (len >= sizeof(nettype_array)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: netclass too long %zu >= %zu",
				__func__, len, sizeof(nettype_array));
			clnt = clnt_raw_ncreate(prog, vers);
			clnt->cl_error.re_status = RPC_TLIERROR;
			return (clnt);
		}
		strcpy(nettype, netclass);
	}

	handle = __rpc_setconf((char *)nettype);
	if (handle == NULL) {
		clnt = clnt_raw_ncreate(prog, vers);
		clnt->cl_error.re_status = RPC_UNKNOWNPROTO;
		return (clnt);
	}
	save_cf_error.re_status = RPC_SUCCESS;

	for (;;) {
		nconf = __rpc_getconf(handle);
		if (nconf == NULL) {
			clnt = clnt_raw_ncreate(prog, vers);
			clnt->cl_error.re_status = RPC_UNKNOWNPROTO;
			break;
		}
		__warnx(TIRPC_DEBUG_FLAG_CLNT, "%s: trying netid %s",
			__func__, nconf->nc_netid);

		clnt = clnt_tp_ncreate_timed(hostname, prog, vers, nconf, tp);
		if (CLNT_SUCCESS(clnt))
			break;

		if (clnt->cl_error.re_status != RPC_N2AXLATEFAILURE
		    && clnt->cl_error.re_status != RPC_UNKNOWNHOST) {
			/*
			 * Since we didn't get a name-to-address
			 * translation failure here, we remember
			 * this particular error.  The object of
			 * this is to enable us to return to the
			 * caller a more-specific error than the
			 * unhelpful ``Name to address translation
			 * failed'' which might well occur if we
			 * merely returned the last error (because
			 * the local loopbacks are typically the
			 * last ones in /etc/netconfig and the most
			 * likely to be unable to translate a host
			 * name).  We also check for a more
			 * meaningful error than ``unknown host
			 * name'' for the same reasons.
			 */
			save_cf_error = clnt->cl_error;
			CLNT_DESTROY(clnt);
			clnt = NULL;
		}
	}

	/*
	 * Attempt to return an error more specific than ``Name to address
	 * translation failed'' or ``unknown host name''
	 */
	if ((clnt->cl_error.re_status == RPC_N2AXLATEFAILURE
	     || clnt->cl_error.re_status == RPC_UNKNOWNHOST)
	    && (save_cf_error.re_status != RPC_SUCCESS)) {
		clnt->cl_error = save_cf_error;
	}
	__rpc_endconf(handle);
	return (clnt);
}

/*
 * Generic client creation: takes (servers name, program-number, netconf) and
 * returns client handle. Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s : clnt_control()
 * It finds out the server address from rpcbind and calls clnt_tli_create().
 *
 * A NULL value for the timeout pointer indicates that the default
 * value for the timeout should be used.
 */
CLIENT *
clnt_tp_ncreate_timed(const char *hostname, rpcprog_t prog,
		      rpcvers_t vers, const struct netconfig *nconf,
		      const struct timeval *tp)
{
	struct netbuf *svcaddr;	/* servers address */
	CLIENT *cl = NULL;	/* client handle */

	if (nconf == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: %s",
			__func__, clnt_sperrno(RPC_TLIERROR));
		cl = clnt_raw_ncreate(prog, vers);
		cl->cl_error.re_status = RPC_TLIERROR;
		return (cl);
	}

	/*
	 * Get the address of the server
	 */
	svcaddr =
		__rpcb_findaddr_timed(prog, vers, (struct netconfig *)nconf,
				      (char *)hostname, &cl,
				      (struct timeval *)tp);
	if (svcaddr == NULL) {
		/* appropriate error number is set by rpcbind libraries */
		return (cl);
	}
	if (cl == NULL) {
		/* __rpc_findaddr_timed failed? */
		cl = clnt_tli_ncreate(RPC_ANYFD, nconf, svcaddr, prog, vers, 0,
				      0);
	}
	if (CLNT_SUCCESS(cl)) {
		/* Reuse the CLIENT handle and change the appropriate fields */
		if (CLNT_CONTROL(cl, CLSET_SVC_ADDR, (void *)svcaddr) == true) {
			if (cl->cl_netid == NULL)
				cl->cl_netid = mem_strdup(nconf->nc_netid);
			if (cl->cl_tp == NULL)
				cl->cl_tp = mem_strdup(nconf->nc_device);
			(void)CLNT_CONTROL(cl, CLSET_PROG, (void *)&prog);
			(void)CLNT_CONTROL(cl, CLSET_VERS, (void *)&vers);
		} else {
			CLNT_DESTROY(cl);
			cl = clnt_tli_ncreate(RPC_ANYFD, nconf, svcaddr, prog,
					      vers, 0, 0);
		}
	}
	mem_free(svcaddr->buf, sizeof(*svcaddr->buf));
	mem_free(svcaddr, sizeof(*svcaddr));
	return (cl);
}

/*
 * Generic client creation:  returns client handle.
 * Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s : clnt_control().
 * If fd is RPC_ANYFD, it will be opened using nconf.
 * It will be bound if not so.
 * If sizes are 0; appropriate defaults will be chosen.
 */
CLIENT *
clnt_tli_ncreate(int fd, const struct netconfig *nconf,
		 struct netbuf *svcaddr, rpcprog_t prog,
		 rpcvers_t vers, u_int sendsz, u_int recvsz)
{
	CLIENT *cl;		/* client handle */
	struct __rpc_sockinfo si;
	long servtype;
	int save_errno;
	int one = 1;
	uint32_t flags = CLNT_CREATE_FLAG_CONNECT;
	struct timeval timeval;

	if (fd == RPC_ANYFD) {
		if (nconf == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: %s",
				__func__, clnt_sperrno(RPC_TLIERROR));
			cl = clnt_raw_ncreate(prog, vers);
			cl->cl_error.re_status = RPC_TLIERROR;
			return (cl);
		}

		fd = __rpc_nconf2fd(nconf);

		if (fd == -1)
			goto err;
		if (fd < __rpc_minfd)
			fd = __rpc_raise_fd(fd);
		flags |= CLNT_CREATE_FLAG_CLOSE;
		servtype = nconf->nc_semantics;
		if (!__rpc_fd2sockinfo(fd, &si))
			goto err;
		bindresvport(fd, NULL);
	} else {
		if (!__rpc_fd2sockinfo(fd, &si))
			goto err;
		servtype = __rpc_socktype2seman(si.si_socktype);
		if (servtype == -1) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: %s",
				__func__, clnt_sperrno(RPC_UNKNOWNPROTO));
			cl = clnt_raw_ncreate(prog, vers);
			cl->cl_error.re_status = RPC_UNKNOWNPROTO;
			return (cl);
		}
	}

	if (si.si_af != ((struct sockaddr *)svcaddr->buf)->sa_family) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: %s",
			__func__, clnt_sperrno(RPC_UNKNOWNHOST));
		cl = clnt_raw_ncreate(prog, vers);
		cl->cl_error.re_status = RPC_UNKNOWNHOST;	/* XXX */
		goto err1;
	}

	switch (servtype) {
	case NC_TPI_COTS:
		if (nconf && ((strcmp(nconf->nc_protofmly, "inet") == 0)
			      || (strcmp(nconf->nc_protofmly, "inet6") == 0))) {
			/* set SO_SNDTIMEO to deal with bad clients */
			timeval.tv_sec = 5;
			timeval.tv_usec = 0;
			if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
				       (char *)&timeval, sizeof(timeval))) {
				__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
					"%s: fd %d SO_SNDTIMEO failed (%d)",
					__func__, fd, errno);
			}
		}
		cl = clnt_vc_ncreatef(fd, svcaddr, prog, vers, sendsz, recvsz,
				      flags);
		break;
	case NC_TPI_COTS_ORD:
		if (nconf && ((strcmp(nconf->nc_protofmly, "inet") == 0)
			      || (strcmp(nconf->nc_protofmly, "inet6") == 0))) {
			(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
					  sizeof(one));
			/* set SO_SNDTIMEO to deal with bad clients */
			timeval.tv_sec = 5;
			timeval.tv_usec = 0;
			if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
				       (char *)&timeval, sizeof(timeval))) {
				__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
					"%s: fd %d SO_SNDTIMEO failed (%d)",
					__func__, fd, errno);
			}

		}
		cl = clnt_vc_ncreatef(fd, svcaddr, prog, vers, sendsz, recvsz,
				      flags);
		break;
	case NC_TPI_CLTS:
		cl = clnt_dg_ncreatef(fd, svcaddr, prog, vers, sendsz, recvsz,
				      flags);
		break;
	default:
		goto err;
	}

	if (CLNT_FAILURE(cl))
		goto err1;

	if (flags & CLNT_CREATE_FLAG_CLOSE) {
		/* We got a new FD; this makes it a local client */
		cl->cl_flags |= CLNT_FLAG_LOCAL;
	}

	if (nconf) {
		cl->cl_netid = mem_strdup(nconf->nc_netid);
		cl->cl_tp = mem_strdup(nconf->nc_device);
	} else {
		cl->cl_netid = "";
		cl->cl_tp = "";
	}

	return (cl);

 err:
	save_errno = errno;
	cl = clnt_raw_ncreate(prog, vers);
	cl->cl_error.re_status = RPC_SYSTEMERROR;
	cl->cl_error.re_errno = save_errno;
 err1:	if (flags & CLNT_CREATE_FLAG_CLOSE)
		(void)close(fd);
	return (cl);
}

int
clnt_req_xid_cmpf(const struct opr_rbtree_node *lhs,
		  const struct opr_rbtree_node *rhs)
{
	struct clnt_req *lk, *rk;

	lk = opr_containerof(lhs, struct clnt_req, cc_dplx);
	rk = opr_containerof(rhs, struct clnt_req, cc_dplx);

	if (lk->cc_xid < rk->cc_xid)
		return (-1);

	if (lk->cc_xid == rk->cc_xid)
		return (0);

	return (1);
}

enum clnt_stat
clnt_req_callback(struct clnt_req *cc)
{
	svc_rqst_expire_insert(cc);

	return CLNT_CALL_ONCE(cc);
}

/*
 * waitq_entry is locked in clnt_req_setup()
 */
void
clnt_req_callback_default(struct clnt_req *cc)
{
	mutex_lock(&cc->cc_we.mtx);
	cond_signal(&cc->cc_we.cv);
	mutex_unlock(&cc->cc_we.mtx);
}

enum clnt_stat
clnt_req_refresh(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct opr_rbtree_node *nv;

	/* this lock protects both xid and rbtree */
	rpc_dplx_rli(rec);
	opr_rbtree_remove(&rec->call_replies, &cc->cc_dplx);
	cc->cc_xid = ++(rec->call_xid);
	nv = opr_rbtree_insert(&rec->call_replies, &cc->cc_dplx);
	rpc_dplx_rui(rec);
	if (nv) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d insert failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd,
			cc->cc_xid);
		cc->cc_error.re_status = RPC_TLIERROR;
		return (RPC_TLIERROR);
	}

	cc->cc_error.re_status = RPC_SUCCESS;
	return (RPC_SUCCESS);
}

void
clnt_req_reset(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);

	rpc_dplx_rli(cx->cx_rec);
	opr_rbtree_remove(&cx->cx_rec->call_replies, &cc->cc_dplx);
	rpc_dplx_rui(cx->cx_rec);

	if (atomic_postclear_uint16_t_bits(&cc->cc_flags,
					   CLNT_REQ_FLAG_ACKSYNC |
					   CLNT_REQ_FLAG_EXPIRING)
	    & CLNT_REQ_FLAG_EXPIRING) {
		svc_rqst_expire_remove(cc);
		cc->cc_expire_ms = 0;	/* atomic barrier(s) */
	}
}

enum clnt_stat
clnt_req_setup(struct clnt_req *cc, struct timespec timeout)
{
	CLIENT *clnt = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct opr_rbtree_node *nv;

	cc->cc_error.re_errno = 0;
	cc->cc_error.re_status = RPC_SUCCESS;
	cc->cc_flags = CLNT_REQ_FLAG_NONE;
	cc->cc_process_cb = clnt_req_callback_default;
	cc->cc_refreshes = 2;
	cc->cc_timeout = timeout;

	if (timeout.tv_nsec < 0 || timeout.tv_nsec > 999999999
	 || timeout.tv_sec < 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d bad timeout (%ld.%09ld)",
			__func__, &rec->xprt, rec->xprt.xp_fd,
			timeout.tv_sec, timeout.tv_nsec);
		cc->cc_error.re_status = RPC_TLIERROR;
		return (RPC_TLIERROR);
	}
	if (timeout.tv_sec > 10) {
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s: tv_sec %ld > 10",
			__func__, timeout.tv_sec);
	}

	/* this lock protects both xid and rbtree */
	rpc_dplx_rli(rec);
	cc->cc_xid = ++(rec->call_xid);
	nv = opr_rbtree_insert(&rec->call_replies, &cc->cc_dplx);
	rpc_dplx_rui(rec);
	if (nv) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d insert failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid);
		cc->cc_error.re_status = RPC_TLIERROR;
		return (RPC_TLIERROR);
	}

	CLNT_REF(clnt, CLNT_REF_FLAG_NONE);
	return (RPC_SUCCESS);
}

/*
 * unlocked
 */
enum xprt_stat
clnt_req_process_reply(SVCXPRT *xprt, struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct opr_rbtree_node *nv;
	struct clnt_req *cc;
	struct clnt_req cc_k;

	rpc_dplx_rli(rec);
	cc_k.cc_xid = req->rq_msg.rm_xid;
	nv = opr_rbtree_lookup(&rec->call_replies, &cc_k.cc_dplx);
	rpc_dplx_rui(rec);
	if (!nv) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d lookup failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd, cc_k.cc_xid);
		return SVC_STAT(xprt);
	}
	cc = opr_containerof(nv, struct clnt_req, cc_dplx);

	/* order dependent */
	if (atomic_postclear_uint16_t_bits(&cc->cc_flags,
					   CLNT_REQ_FLAG_EXPIRING)
	    & CLNT_REQ_FLAG_EXPIRING) {
		svc_rqst_expire_remove(cc);
		cc->cc_expire_ms = 0;	/* atomic barrier(s) */
	}

	if (atomic_postset_uint16_t_bits(&cc->cc_flags, CLNT_REQ_FLAG_ACKSYNC)
	    & (CLNT_REQ_FLAG_ACKSYNC | CLNT_REQ_FLAG_BACKSYNC)) {
		__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
			"%s: %p fd %d xid %" PRIu32 " ignored=%d",
			__func__, xprt, xprt->xp_fd, cc->cc_xid,
			cc->cc_error.re_status);
		cc->cc_refreshes = 0;
		return SVC_STAT(xprt);
	}

	_seterr_reply(&req->rq_msg, &(cc->cc_error));
	if (cc->cc_error.re_status == RPC_SUCCESS) {
		if (!AUTH_VALIDATE(cc->cc_auth, &(cc->cc_verf))) {
			cc->cc_error.re_status = RPC_AUTHERROR;
			cc->cc_error.re_why = AUTH_INVALIDRESP;
		} else if (cc->cc_reply.proc
			   && !AUTH_UNWRAP(cc->cc_auth, xdrs,
					   cc->cc_reply.proc,
					   cc->cc_reply.where)) {
			if (cc->cc_error.re_status == RPC_SUCCESS)
				cc->cc_error.re_status = RPC_CANTDECODERES;
		}
		cc->cc_refreshes = 0;
	}

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d xid %" PRIu32 " result=%d",
		__func__, xprt, xprt->xp_fd, cc->cc_xid,
		cc->cc_error.re_status);

	(*cc->cc_process_cb)(cc);
	return SVC_STAT(xprt);
}

enum clnt_stat
clnt_req_wait_reply(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct timespec ts;
	int code;

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d xid %" PRIu32 " (%ld.%09ld)",
		__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid,
		cc->cc_timeout.tv_sec, cc->cc_timeout.tv_nsec);

 call_again:
	cc->cc_error.re_status = CLNT_CALL_ONCE(cc);
	if (cc->cc_error.re_status != RPC_SUCCESS) {
		return (cc->cc_error.re_status);
	}

	if (!(cc->cc_timeout.tv_sec + cc->cc_timeout.tv_nsec)) {
		return (RPC_SUCCESS);
	}

	(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
	timespecadd(&ts, &cc->cc_timeout, &ts);
	code = cond_timedwait(&cc->cc_we.cv, &cc->cc_we.mtx, &ts);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d replied xid %" PRIu32,
		__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid);

	if (!(atomic_fetch_uint16_t(&cc->cc_flags) & CLNT_REQ_FLAG_ACKSYNC)
	 && (code == ETIMEDOUT)) {
		if (rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
			/* XXX should also set error.re_why, but the
			 * facility is not well developed. */
			cc->cc_error.re_status = RPC_TIMEDOUT;
			return (RPC_TIMEDOUT);
		}
	}

	if (cc->cc_refreshes-- > 0) {
		if (cc->cc_error.re_status == RPC_AUTHERROR) {
			if (!AUTH_REFRESH(cc->cc_auth, NULL)) {
				return (RPC_AUTHERROR);
			}
			if (clnt_req_refresh(cc) != RPC_SUCCESS) {
				return (cc->cc_error.re_status);
			}
		}
		atomic_clear_uint16_t_bits(&cc->cc_flags,
					   CLNT_REQ_FLAG_ACKSYNC);
		goto call_again;
	}
	if (code == ETIMEDOUT) {
		/* We have refreshed/retried, just log it */
		__warnx(TIRPC_DEBUG_FLAG_CLNT_DG,
			"%s: %p fd %d ETIMEDOUT",
			__func__, &rec->xprt, rec->xprt.xp_fd);
		cc->cc_error.re_status = RPC_TIMEDOUT;
		return (RPC_TIMEDOUT);
	}

	__warnx(TIRPC_DEBUG_FLAG_CLNT_DG,
		"%s: %p fd %d result=%d",
		__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_error.re_status);

	return (cc->cc_error.re_status);
}

int
clnt_req_release(struct clnt_req *cc)
{
	int refs = atomic_dec_int32_t(&cc->cc_refcnt);

	if (likely(refs > 0)) {
		/* normal case */
		return (refs);
	}

	clnt_req_reset(cc);
	clnt_req_fini(cc);
	CLNT_RELEASE(cc->cc_clnt, CLNT_RELEASE_FLAG_NONE);

	(*cc->cc_free_cb)(cc, cc->cc_size);
	return (refs);
}

/*
 *  To avoid conflicts with the "magic" file descriptors (0, 1, and 2),
 *  we try to not use them.  The __rpc_raise_fd() routine will dup
 *  a descriptor to a higher value.  If we fail to do it, we continue
 *  to use the old one (and hope for the best).
 */
int __rpc_raise_fd(int fd)
{
	int nfd;

	if (fd >= __rpc_minfd)
		return (fd);

	nfd = fcntl(fd, F_DUPFD, __rpc_minfd);
	if (nfd == -1)
		return (fd);

	if (fsync(nfd) == -1) {
		close(nfd);
		return (fd);
	}

	if (close(fd) == -1) {
		/* this is okay, we will log an error, then use the new fd */
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"could not close() fd %d; mem & fd leak", fd);
	}

	return (nfd);
}
