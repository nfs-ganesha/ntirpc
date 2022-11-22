/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
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

#include "config.h"

/*
 * svc_vc.c, Server side for Connection Oriented RPC.
 *
 * Actually implements two flavors of transporter -
 * a tcp rendezvouser (a listner and connection establisher)
 * and a record/tcp stream.
 */
#include <sys/cdefs.h>
#include <sys/socket.h>
#ifdef RPC_VSOCK
#include <linux/vm_sockets.h>
#endif /* VSOCK */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getpeereid.h>

#include <rpc/types.h>
#include <misc/city.h>
#include <misc/portable.h>
#include <misc/timespec.h>
#include <rpc/clnt.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>
#include <rpc/svc_rqst.h>
#include <rpc/xdr_ioq.h>

#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "svc_ioq.h"

static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_override_ops(SVCXPRT *, SVCXPRT *);

/*
 * A record is composed of one or more record fragments.
 * A record fragment is a four-byte header followed by zero to
 * 2**32-1 bytes.  The header is treated as a long unsigned and is
 * encode/decoded to the network via htonl/ntohl.  The low order 31 bits
 * are a byte count of the fragment.  The highest order bit is a boolean:
 * 1 => this fragment is the last fragment of the record,
 * 0 => this fragment is followed by more fragment(s).
 *
 * The fragment/record machinery is not general;  it is constructed to
 * meet the needs of xdr and rpc based on tcp.
 */

#define LAST_FRAG ((u_int32_t)(1 << 31))

/*
 * Usage:
 * xprt = svc_vc_ncreate(sock, send_buf_size, recv_buf_size);
 *
 * Creates, registers, and returns a (rpc) tcp based transport.
 * If a problem occurred, this routine returns a NULL.
 *
 * Since streams do buffered io similar to stdio, the caller can specify
 * how big the send and receive buffers are via the second and third parms;
 * 0 => use the system default.
 *
 * Added svc_vc_ncreatef with flags argument, has the behavior of the
 * original function with flags SVC_CREATE_FLAG_CLOSE.
 *
 */
static void
svc_vc_xprt_free(struct svc_vc_xprt *xd)
{
	XDR_DESTROY(xd->sx_dr.ioq.xdrs);
	rpc_dplx_rec_destroy(&xd->sx_dr);
	mem_free(xd, sizeof(struct svc_vc_xprt));
}

static struct svc_vc_xprt *
svc_vc_xprt_zalloc(void)
{
	struct svc_vc_xprt *xd = mem_zalloc(sizeof(struct svc_vc_xprt));

	/* Init SVCXPRT locks, etc */
	rpc_dplx_rec_init(&xd->sx_dr);
	xdr_ioq_setup(&xd->sx_dr.ioq);
	return (xd);
}

void
svc_vc_xprt_setup(SVCXPRT **sxpp)
{
	if (unlikely(*sxpp)) {
		svc_vc_xprt_free(VC_DR(REC_XPRT(*sxpp)));
		*sxpp = NULL;
	} else {
		struct svc_vc_xprt *xd = svc_vc_xprt_zalloc();

		*sxpp = &xd->sx_dr.xprt;
	}
}

SVCXPRT *
svc_vc_ncreatef(const int fd, const u_int sendsz, const u_int recvsz,
		const uint32_t flags)
{
	struct __rpc_sockinfo si;
	SVCXPRT *xprt;
	struct rpc_dplx_rec *rec;
	struct svc_vc_xprt *xd;
	const char *netid;
	u_int recvsize;
	u_int sendsize;
	u_int xp_flags;
	int rc;

	/* atomically find or create shared fd state; ref+1; locked */
	xprt = svc_xprt_lookup(fd, svc_vc_xprt_setup);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d svc_xprt_lookup failed",
			__func__, fd);
		return (NULL);
	}
	rec = REC_XPRT(xprt);

	xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags,
						(flags & SVC_XPRT_FLAG_CLOSE)
						| SVC_XPRT_FLAG_INITIALIZED);
	if (xp_flags & SVC_XPRT_FLAG_INITIALIZED) {
		rpc_dplx_rui(rec);
		XPRT_TRACE(xprt, __func__, __func__, __LINE__);
		return (xprt);
	}

	if (!__rpc_fd2sockinfo(fd, &si)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get transport information",
			__func__, fd);
		return (NULL);
	}

	if (!__rpc_sockinfo2netid(&si, &netid)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get network information",
			__func__, fd);
		return (NULL);
	}

	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
	recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);
	/*
	 * Should be multiple of 4 for XDR.
	 */
	xd = VC_DR(rec);
	xd->sx_dr.sendsz = ((sendsize + 3) / 4) * 4;
	xd->sx_dr.recvsz = ((recvsize + 3) / 4) * 4;
	xd->sx_dr.pagesz = sysconf(_SC_PAGESIZE);
	xd->sx_dr.maxrec = __svc_maxrec;

	/* duplex streams are not used by the rendezvous transport */
	xdrmem_create(xd->sx_dr.ioq.xdrs, NULL, 0, XDR_ENCODE);

	svc_vc_rendezvous_ops(xprt);
#ifdef RPC_VSOCK
	if (si.si_af == AF_VSOCK)
		 xprt->xp_type = XPRT_VSOCK_RENDEZVOUS;
#endif /* VSOCK */

	/* caller should know what it's doing */
	if (flags & SVC_CREATE_FLAG_LISTEN) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d listen",
			 __func__, fd);
		listen(fd, SOMAXCONN);
	}

	__rpc_address_setup(&xprt->xp_local);
	rc = getsockname(fd, xprt->xp_local.nb.buf, &xprt->xp_local.nb.len);
	if (rc < 0) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}

	xprt->xp_netid = mem_strdup(netid);

	/* Conditional register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
	     && !(flags & SVC_CREATE_FLAG_XPRT_NOREG))
	    || (flags & SVC_CREATE_FLAG_XPRT_DOREG))
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    RPC_DPLX_LOCKED |
				    SVC_RQST_FLAG_CHAN_AFFINITY);

	/* release */
	rpc_dplx_rui(rec);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_vc");
#endif

	return (xprt);
}

static SVCXPRT *
makefd_xprt(const int fd, const u_int sendsz, const u_int recvsz,
	    struct __rpc_sockinfo *si, u_int flags)
{
	SVCXPRT *xprt;
	struct svc_vc_xprt *xd;
	struct rpc_dplx_rec *rec;
	const char *netid;
	u_int recvsize;
	u_int sendsize;
	u_int xp_flags;

	assert(fd != -1);

	/* atomically find or create shared fd state; ref+1; locked */
	xprt = svc_xprt_lookup(fd, svc_vc_xprt_setup);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d svc_xprt_lookup failed",
			__func__, fd);
		return (NULL);
	}
	rec = REC_XPRT(xprt);

	xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags, flags
						| SVC_XPRT_FLAG_INITIALIZED);
	if (xp_flags & SVC_XPRT_FLAG_INITIALIZED) {
		rpc_dplx_rui(rec);
		XPRT_TRACE(xprt, __func__, __func__, __LINE__);
		return (xprt);
	}

	if (!__rpc_fd2sockinfo(fd, si)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get transport information",
			__func__, fd);
		return (NULL);
	}

	if (!__rpc_sockinfo2netid(si, &netid)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get network information",
			__func__, fd);
		return (NULL);
	}

	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si->si_af, si->si_proto, (int)sendsz);
	recvsize = __rpc_get_t_size(si->si_af, si->si_proto, (int)recvsz);
	/*
	 * Should be multiple of 4 for XDR.
	 */
	xd = VC_DR(rec);
	xd->sx_dr.sendsz = ((sendsize + 3) / 4) * 4;
	xd->sx_dr.recvsz = ((recvsize + 3) / 4) * 4;
	xd->sx_dr.pagesz = sysconf(_SC_PAGESIZE);
	xd->sx_dr.maxrec = __svc_maxrec;

#ifdef RPC_VSOCK
	if (si->si_af == AF_VSOCK)
		 xprt->xp_type = XPRT_VSOCK;
#endif /* VSOCK */

	xprt->xp_netid = mem_strdup(netid);

	/* release */
	rpc_dplx_rui(rec);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	return (xprt);
}

/*
 * Like sv_fd_ncreate(), except export flags for additional control.
 */
SVCXPRT *
svc_fd_ncreatef(const int fd, const u_int sendsize, const u_int recvsize,
		const uint32_t flags)
{
	SVCXPRT *xprt;
	struct __rpc_sockinfo si;
	int rc;

	assert(fd != -1);

	xprt = makefd_xprt(fd, sendsize, recvsize, &si,
			   flags & SVC_XPRT_FLAG_CLOSE);
	if ((!xprt) || (!(xprt->xp_flags & SVC_XPRT_FLAG_INITIAL)))
		return (xprt);

	svc_vc_override_ops(xprt, NULL);

	__rpc_address_setup(&xprt->xp_local);
	rc = getsockname(fd, xprt->xp_local.nb.buf, &xprt->xp_local.nb.len);
	if (rc < 0) {
		xprt->xp_local.nb.len = sizeof(struct sockaddr_storage);
		memset(xprt->xp_local.nb.buf, 0xfe, xprt->xp_local.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}

	__rpc_address_setup(&xprt->xp_remote);
	rc = getpeername(fd, xprt->xp_remote.nb.buf, &xprt->xp_remote.nb.len);
	if (rc < 0) {
		xprt->xp_remote.nb.len = sizeof(struct sockaddr_storage);
		memset(xprt->xp_remote.nb.buf, 0xfe, xprt->xp_remote.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getpeername failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	/* Conditional register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
	     && !(flags & SVC_CREATE_FLAG_XPRT_NOREG))
	    || (flags & SVC_CREATE_FLAG_XPRT_DOREG))
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_vc");
#endif

	return (xprt);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_vc_rendezvous(SVCXPRT *xprt)
{
	struct svc_vc_xprt *req_xd = VC_DR(REC_XPRT(xprt));
	SVCXPRT *newxprt;
	struct svc_vc_xprt *xd;
	struct sockaddr_storage addr;
	struct __rpc_sockinfo si;
	int fd;
	int rc;
	socklen_t len;
	static int n = 1;
	struct timeval timeval;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, funcin, __func__, __LINE__, xprt);
#endif /* USE_LTTNG_NTIRPC */

 again:
	len = sizeof(addr);
	fd = accept(xprt->xp_fd, (struct sockaddr *)(void *)&addr, &len);
	if (fd < 0) {
		if (errno == EINTR)
			goto again;
		/*
		 * Clean out the most idle file descriptor when we're
		 * running out.
		 */
		if (errno == EMFILE || errno == ENFILE) {
			switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
			case SVC_EVENT_EPOLL:
				break;
#endif
			default:
				abort();	/* XXX */
				break;
			}	/* switch */
			goto again;
		}
		return (XPRT_DIED);
	}
	if (unlikely(svc_rqst_rearm_events(xprt, SVC_XPRT_FLAG_ADDED_RECV))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d svc_rqst_rearm_events failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		close(fd);
		return (XPRT_DIED);
	}

	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));

	/*
	 * make a new transport (re-uses xprt)
	 */
	newxprt = makefd_xprt(fd, req_xd->sx_dr.sendsz, req_xd->sx_dr.recvsz,
			      &si, SVC_XPRT_FLAG_CLOSE);
	if ((!newxprt) || (!(newxprt->xp_flags & SVC_XPRT_FLAG_INITIAL))) {
		close(fd);
		return (XPRT_DIED);
	}

	svc_vc_override_ops(newxprt, xprt);

	__rpc_address_setup(&newxprt->xp_remote);
	memcpy(newxprt->xp_remote.nb.buf, &addr, len);
	newxprt->xp_remote.nb.len = len;
	XPRT_TRACE(newxprt, __func__, __func__, __LINE__);

	/* XXX fvdl - is this useful? (Yes.  Matt) */
	if (si.si_proto == IPPROTO_TCP) {
		len = 1;
		(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len,
				  sizeof(len));
	}

	/* set SO_SNDTIMEO to deal with bad clients */
	timeval.tv_sec = 5;
	timeval.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeval,
		       sizeof(timeval))) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d SO_SNDTIMEO failed (%d)",
			 __func__, fd, errno);
	}

	__rpc_address_setup(&newxprt->xp_local);
	rc = getsockname(fd, newxprt->xp_local.nb.buf,
			 &newxprt->xp_local.nb.len);
	if (rc < 0) {
		newxprt->xp_local.nb.len = sizeof(struct sockaddr_storage);
		memset(newxprt->xp_local.nb.buf, 0xfe,
		       newxprt->xp_local.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
	}

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(newxprt, "svc_vc");
#endif

	xd = VC_DR(REC_XPRT(newxprt));
	xd->sx_dr.sendsz = req_xd->sx_dr.sendsz;
	xd->sx_dr.recvsz = req_xd->sx_dr.recvsz;
	xd->sx_dr.pagesz = req_xd->sx_dr.pagesz;
	xd->sx_dr.maxrec = req_xd->sx_dr.maxrec;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	newxprt->xp_parent = xprt;
	if (xprt->xp_dispatch.rendezvous_cb(newxprt)
	 || svc_rqst_xprt_register(newxprt, xprt)) {
		SVC_DESTROY(newxprt);
		/* Was never added to epoll */
		SVC_RELEASE(newxprt, SVC_RELEASE_FLAG_NONE);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		return (XPRT_DESTROYED);
	}

	/* We're not using a ref for the hook anymore, since epoll doesn't store
	 * the transport pointer.  Drop the extra ref here. */
	SVC_RELEASE(newxprt, SVC_RELEASE_FLAG_NONE);
	return (XPRT_IDLE);
}

static void
svc_vc_destroy_task(struct work_pool_entry *wpe)
{
	struct rpc_dplx_rec *rec =
			opr_containerof(wpe, struct rpc_dplx_rec, ioq.ioq_wpe);
	uint16_t xp_flags;

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p fd %d xp_refcnt %" PRId32,
		__func__, rec, rec->xprt.xp_fd, rec->xprt.xp_refcnt);

	if (rec->xprt.xp_refcnt) {
		/* instead of nanosleep */
		work_pool_submit(&svc_work_pool, &(rec->ioq.ioq_wpe));
		return;
	}

	xp_flags = atomic_postclear_uint16_t_bits(&rec->xprt.xp_flags,
						  SVC_XPRT_FLAG_CLOSE);
	if ((xp_flags & SVC_XPRT_FLAG_CLOSE)
	    && rec->xprt.xp_fd != RPC_ANYFD) {
		(void)close(rec->xprt.xp_fd);
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d closed",
			 __func__, rec->xprt.xp_fd);
		rec->xprt.xp_fd = RPC_ANYFD;
	}

	if (rec->xprt.xp_ops->xp_free_user_data)
		rec->xprt.xp_ops->xp_free_user_data(&rec->xprt);

	if (rec->xprt.xp_tp)
		mem_free(rec->xprt.xp_tp, 0);
	if (rec->xprt.xp_netid)
		mem_free(rec->xprt.xp_netid, 0);

	if (rec->xprt.xp_parent)
		SVC_RELEASE(rec->xprt.xp_parent, SVC_RELEASE_FLAG_NONE);

	svc_vc_xprt_free(VC_DR(rec));
}

static void
svc_vc_unlink_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	svc_rqst_xprt_unregister(xprt, flags);
}

static void
svc_vc_destroy_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 0,
	};

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p fd %d xp_refcnt %" PRId32 " @%s:%d",
		__func__, xprt, xprt->xp_fd, xprt->xp_refcnt, tag, line);

	while (atomic_postset_uint16_t_bits(&(REC_XPRT(xprt)->ioq.ioq_s.qflags),
					    IOQ_FLAG_WORKING)
	       & IOQ_FLAG_WORKING) {
		nanosleep(&ts, NULL);
	}

	REC_XPRT(xprt)->ioq.ioq_wpe.fun = svc_vc_destroy_task;
	work_pool_submit(&svc_work_pool, &(REC_XPRT(xprt)->ioq.ioq_wpe));
}

extern mutex_t ops_lock;

 /*ARGSUSED*/
static bool
svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
		*(u_int *) in = xprt->xp_flags;
		break;
	case SVCSET_XP_FLAGS:
		xprt->xp_flags = *(u_int *) in;
		break;
	case SVCGET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		*(svc_xprt_fun_t *) in = xprt->xp_ops->xp_free_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_free_user_data = *(svc_xprt_fun_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (FALSE);
	}
	return (TRUE);
}

static bool
svc_vc_rendezvous_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	switch (rq) {
	case SVCGET_CONNMAXREC:
		*(int *)in = xd->sx_dr.maxrec;
		break;
	case SVCSET_CONNMAXREC:
		xd->sx_dr.maxrec = *(int *)in;
		break;
	case SVCGET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		*(svc_xprt_fun_t *) in = xprt->xp_ops->xp_free_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_free_user_data = *(svc_xprt_fun_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (FALSE);
	}
	return (TRUE);
}

static enum xprt_stat
svc_vc_stat(SVCXPRT *xprt)
{
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)
		return (XPRT_DESTROYED);

	return (XPRT_IDLE);
}

static enum xprt_stat
svc_vc_recv(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_vc_xprt *xd = VC_DR(rec);
	struct poolq_entry *have;
	struct xdr_ioq_uv *uv;
	struct xdr_ioq *xioq;
	ssize_t rlen;
	u_int flags;
	int code;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, funcin, __func__, __LINE__, xprt);
#endif /* USE_LTTNG_NTIRPC */

	/* no need for locking, only one svc_rqst_xprt_task() per event.
	 * depends upon svc_rqst_rearm_events() for ordering.
	 */
	have = TAILQ_LAST(&rec->ioq.ioq_uv.uvqh.qh, poolq_head_s);
	if (!have) {
		xioq = xdr_ioq_create(xd->sx_dr.pagesz, xd->sx_dr.maxrec,
				      UIO_FLAG_BUFQ);
		(rec->ioq.ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&rec->ioq.ioq_uv.uvqh.qh, &xioq->ioq_s, q);
	} else {
		xioq = _IOQ(have);
	}

	if (!xd->sx_fbtbc) {
		rlen = recv(xprt->xp_fd, &xd->sx_fbtbc, BYTES_PER_XDR_UNIT,
			    MSG_WAITALL);

		if (unlikely(rlen < 0)) {
			code = errno;

			if (code == EAGAIN || code == EWOULDBLOCK) {
				__warnx(TIRPC_DEBUG_FLAG_WARN,
					"%s: %p fd %d recv errno %d (try again)",
					"svc_vc_wait", xprt, xprt->xp_fd, code);
				if (unlikely(svc_rqst_rearm_events(
						xprt,
						SVC_XPRT_FLAG_ADDED_RECV))) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s: %p fd %d svc_rqst_rearm_events failed (will set dead)",
						"svc_vc_wait",
						xprt, xprt->xp_fd);
					SVC_DESTROY(xprt);
					code = EINVAL;
				}
#ifdef USE_LTTNG_NTIRPC
				tracepoint(xprt, recv_exit, __func__, __LINE__,
					   xprt, "EAGAIN", code);
#endif /* USE_LTTNG_NTIRPC */
				return SVC_STAT(xprt);
			}
			__warnx(TIRPC_DEBUG_FLAG_WARN,
				"%s: %p fd %d recv errno %d (will set dead)",
				"svc_vc_wait", xprt, xprt->xp_fd, code);
			SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "ERROR", code);
#endif /* USE_LTTNG_NTIRPC */
			return SVC_STAT(xprt);
		}

		if (unlikely(!rlen)) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d recv closed (will set dead)",
				"svc_vc_wait", xprt, xprt->xp_fd);
			SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "EMPTY", 0);
#endif /* USE_LTTNG_NTIRPC */
			return SVC_STAT(xprt);
		}

		xd->sx_fbtbc = (int32_t)ntohl((long)xd->sx_fbtbc);
		flags = UIO_FLAG_FREE | UIO_FLAG_MORE;

		if (xd->sx_fbtbc & LAST_FRAG) {
			xd->sx_fbtbc &= (~LAST_FRAG);
			flags = UIO_FLAG_FREE;
		}

		if (unlikely(!xd->sx_fbtbc)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d fragment is zero (will set dead)",
				__func__, xprt, xprt->xp_fd);
			SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "NO RECORD", 0);
#endif /* USE_LTTNG_NTIRPC */
			return SVC_STAT(xprt);
		}

#ifdef USE_LTTNG_NTIRPC
		tracepoint(xprt, recv_frag, __func__, __LINE__,
			   xprt, xd->sx_fbtbc);
#endif /* USE_LTTNG_NTIRPC */
		/* one buffer per fragment */
		uv = xdr_ioq_uv_create(xd->sx_fbtbc, flags);
		(xioq->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
	} else {
		uv = IOQ_(TAILQ_LAST(&xioq->ioq_uv.uvqh.qh, poolq_head_s));
		flags = uv->u.uio_flags;
	}

	rlen = recv(xprt->xp_fd, uv->v.vio_tail, xd->sx_fbtbc, MSG_DONTWAIT);

	if (unlikely(rlen < 0)) {
		code = errno;

		if (code == EAGAIN || code == EWOULDBLOCK) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d recv errno %d (try again)",
				__func__, xprt, xprt->xp_fd, code);
			if (unlikely(svc_rqst_rearm_events(
						xprt,
						SVC_XPRT_FLAG_ADDED_RECV))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s: %p fd %d svc_rqst_rearm_events failed (will set dead)",
					__func__, xprt, xprt->xp_fd);
				SVC_DESTROY(xprt);
				code = EINVAL;
			}
#ifdef USE_LTTNG_NTIRPC
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "EAGAIN", code);
#endif /* USE_LTTNG_NTIRPC */
			return SVC_STAT(xprt);
		}
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d recv errno %d (will set dead)",
			__func__, xprt, xprt->xp_fd, code);
		SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
		tracepoint(xprt, recv_exit, __func__, __LINE__,
			   xprt, "ERROR", code);
#endif /* USE_LTTNG_NTIRPC */
		return SVC_STAT(xprt);
	}

	if (unlikely(!rlen)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: %p fd %d recv closed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
		tracepoint(xprt, recv_exit, __func__, __LINE__,
			   xprt, "EMPTY", 0);
#endif /* USE_LTTNG_NTIRPC */
		return SVC_STAT(xprt);
	}

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, recv_bytes, __func__, __LINE__,
		   xprt, xd->sx_fbtbc, rlen);
#endif /* USE_LTTNG_NTIRPC */

	uv->v.vio_tail += rlen;
	xd->sx_fbtbc -= rlen;

	__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
		"%s: %p fd %d recv %zd, need %" PRIu32 ", flags %x",
		__func__, xprt, xprt->xp_fd, rlen, xd->sx_fbtbc, flags);

	if (xd->sx_fbtbc || (flags & UIO_FLAG_MORE)) {
		if (unlikely(svc_rqst_rearm_events(xprt,
						   SVC_XPRT_FLAG_ADDED_RECV))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d svc_rqst_rearm_events failed (will set dead)",
				__func__, xprt, xprt->xp_fd);
			SVC_DESTROY(xprt);
#ifndef USE_LTTNG_NTIRPC
		}
#else
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "REARM FAILED", -1);
		} else {
			tracepoint(xprt, recv_exit, __func__, __LINE__,
				   xprt, "MORE", 0);
		}
#endif /* USE_LTTNG_NTIRPC */
		return SVC_STAT(xprt);
	}

	/* finished a request */
	(rec->ioq.ioq_uv.uvqh.qcount)--;
	TAILQ_REMOVE(&rec->ioq.ioq_uv.uvqh.qh, &xioq->ioq_s, q);
	xdr_ioq_reset(xioq, 0);

	if (unlikely(svc_rqst_rearm_events(xprt, SVC_XPRT_FLAG_ADDED_RECV))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d svc_rqst_rearm_events failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		xdr_ioq_destroy(xioq, xioq->ioq_s.qsize);
		SVC_DESTROY(xprt);
#ifdef USE_LTTNG_NTIRPC
		tracepoint(xprt, recv_exit, __func__, __LINE__,
			   xprt, "REARM FAILED", -1);
#endif /* USE_LTTNG_NTIRPC */
		return SVC_STAT(xprt);
	}

#ifdef USE_LTTNG_NTIRPC
	tracepoint(xprt, recv_exit, __func__, __LINE__,
		   xprt, "CALLING svc_request", 0);
#endif /* USE_LTTNG_NTIRPC */
	return svc_request(xprt, xioq->xdrs);
}

static enum xprt_stat
svc_vc_decode(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	SVCXPRT *xprt = req->rq_xprt;

	/* No need, already positioned to beginning ...
	XDR_SETPOS(xdrs, 0);
	 */
	xdrs->x_op = XDR_DECODE;
	rpc_msg_init(&req->rq_msg);

	if (!xdr_dplx_decode(xdrs, &req->rq_msg)) {
		/* stream is unsynchronized beyond recovery */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		SVC_DESTROY(xprt);
		return SVC_STAT(xprt);
	}

	/* in order of likelihood */
	if (req->rq_msg.rm_direction == CALL) {
		/* an ordinary call header */
		return xprt->xp_dispatch.process_cb(req);
	}

	if (req->rq_msg.rm_direction == REPLY) {
		/* reply header (xprt OK) */
		return clnt_req_process_reply(xprt, req);
	}

	__warnx(TIRPC_DEBUG_FLAG_WARN,
		"%s: %p fd %d failed direction %" PRIu32
		" (will set dead)",
		__func__, xprt, xprt->xp_fd,
		req->rq_msg.rm_direction);
	SVC_DESTROY(xprt);
	return SVC_STAT(xprt);
}

static void
svc_vc_checksum(struct svc_req *req, void *data, size_t length)
{
	req->rq_cksum =
#if 1
	/* CithHash64 is -substantially- faster than crc32c from FreeBSD
	 * SCTP, so prefer it until fast crc32c bests it */
		CityHash64WithSeed(data, MIN(256, length), 103);
#else
		calculate_crc32c(0, data, MIN(256, length));
#endif
}

static enum xprt_stat
svc_vc_reply(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct xdr_ioq *xioq;

	/* Nb, we should probably use getpagesize() on Unix.  Need
	 * an equivalent for Windows.
	 */
	xioq = xdr_ioq_create(RPC_MAXDATA_DEFAULT,
			      __svc_params->ioq.send_max + RPC_MAXDATA_DEFAULT,
			      UIO_FLAG_FREE);

	if (!xdr_reply_encode(xioq->xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d xdr_reply_encode failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_DIED);
	}
	xdr_tail_update(xioq->xdrs);

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	 && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS
	 && req->rq_auth
	 && !SVCAUTH_WRAP(req, xioq->xdrs)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d SVCAUTH_WRAP failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_DIED);
	}
	xdr_tail_update(xioq->xdrs);

	xioq->xdrs[0].x_lib[1] = (void *)req->rq_xprt;
	svc_ioq_write_now(req->rq_xprt, xioq);
	return (XPRT_IDLE);
}

static void
svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *rendezvous)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */
	mutex_lock(&ops_lock);

	xprt->xp_type = XPRT_TCP;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_vc_recv;
		ops.xp_stat = svc_vc_stat;
		ops.xp_decode = svc_vc_decode;
		ops.xp_reply = svc_vc_reply;
		ops.xp_checksum = svc_vc_checksum;
		ops.xp_unlink = svc_vc_unlink_it;
		ops.xp_destroy = svc_vc_destroy_it;
		ops.xp_control = svc_vc_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	svc_override_ops(&ops, rendezvous);
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

static void
svc_vc_rendezvous_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	extern mutex_t ops_lock;

	mutex_lock(&ops_lock);

	xprt->xp_type = XPRT_TCP_RENDEZVOUS;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_vc_rendezvous;
		ops.xp_stat = svc_rendezvous_stat;
		ops.xp_decode = (svc_req_fun_t)abort;
		ops.xp_reply = (svc_req_fun_t)abort;
		ops.xp_checksum = NULL;		/* not used */
		ops.xp_unlink = svc_vc_unlink_it;
		ops.xp_destroy = svc_vc_destroy_it;
		ops.xp_control = svc_vc_rendezvous_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

/*
 * Get the effective UID of the sending process. Used by rpcbind, keyserv
 * and rpc.yppasswdd on AF_LOCAL.
 */
int
__rpc_get_local_uid(SVCXPRT *transp, uid_t *uid)
{
	int sock, ret;
	gid_t egid;
	uid_t euid;
	struct sockaddr *sa;

	sock = transp->xp_fd;
	sa = (struct sockaddr *)&transp->xp_remote.ss;
	if (sa->sa_family == AF_LOCAL) {
		ret = getpeereid(sock, &euid, &egid);
		if (ret == 0)
			*uid = euid;
		return (ret);
	} else
		return (-1);
}
