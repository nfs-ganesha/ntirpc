/*
 * Copyright (c) 2013 Linux Box Corporation.
 * Copyright (c) 2013-2017 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include <intrinsic.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include <misc/opr.h>
#include "svc_ioq.h"

#define LAST_FRAG ((u_int32_t)(1 << 31))
#define LAST_FRAG_XDR_UNITS ((LAST_FRAG - 1) & ~(BYTES_PER_XDR_UNIT - 1))
#define MAXALLOCA (256)

/* Returns 0 on success, EWOULDBLOCK if would block, <0 on error */
static inline int
svc_ioq_flushv(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct msghdr msg;
	struct iovec *iov;
	struct xdr_vio *vio;
	ssize_t result;
	u_int32_t frag_header;
	u_int32_t fbytes;
	int error = 0;
	int frag_needed = 0;
	u_int32_t last_frag = 0;
	u_int32_t end, remaining, iov_count, vsize, isize;

	/* update the most recent data length, just in case */
	xdr_tail_update(xioq->xdrs);

	/* Some basic computations */
	end = XDR_GETPOS(xioq->xdrs);
	remaining = end - xioq->write_start;
	iov_count = XDR_IOVCOUNT(xioq->xdrs, xioq->write_start, remaining);
	vsize = (iov_count + 1) * sizeof(struct iovec);
	isize = iov_count * sizeof(struct xdr_vio);

	__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
		"-------> %s: remaining %"PRIu32" write_start %"PRIu32
		" end %"PRIu32,
		__func__, remaining, xioq->write_start, end);

	memset(&msg, 0, sizeof(msg));

	if (end > (2 * LAST_FRAG_XDR_UNITS)) {
		/* This data will need to be 3 fragments */
		if (xioq->write_start < LAST_FRAG_XDR_UNITS) {
			fbytes = LAST_FRAG_XDR_UNITS - xioq->write_start;
		} else if (xioq->write_start < (2 * LAST_FRAG_XDR_UNITS)) {
			fbytes = (2 * LAST_FRAG_XDR_UNITS) - xioq->write_start;
		} else {
			fbytes = end - xioq->write_start;
			last_frag = LAST_FRAG;
		}
	} else if (end > LAST_FRAG_XDR_UNITS) {
		/* This data will need to be 2 fragments */
		if (xioq->write_start < LAST_FRAG_XDR_UNITS) {
			fbytes = LAST_FRAG_XDR_UNITS - xioq->write_start;
		} else {
			fbytes = end - xioq->write_start;
			last_frag = LAST_FRAG;
		}
	} else {
		fbytes = remaining;
		last_frag = LAST_FRAG;
	}

	if (unlikely(vsize > MAXALLOCA)) {
		iov = mem_alloc(vsize);
	} else {
		iov = alloca(vsize);
	}

	if (unlikely(isize > MAXALLOCA)) {
		vio = mem_alloc(isize);
	} else {
		vio = alloca(isize);
	}

	while (remaining > 0) {
		int i;
		int frag_hdr_size = 0;

		/* Note that there may be lots of re-walking the ioq to
		 * count the number of buffers or fill the buffers in the vio,
		 * unfortunately, any mechanism to try and avoid that would
		 * still have to re-walk the ioq, so we don't save THAT much
		 * by just recomputing in preparation for each attempt to send
		 * data. We could shortcut a little bit if we could estimate
		 * how many bytes would fit in a single iovec so that we
		 * don't walk more of the ioq than we need to. But that adds a
		 * lot of complexity, and just saves walking a linked list.
		 *
		 * A more relevant improvement here might actually be to use
		 * larger buffers than 8k. Optionally, when we do more to
		 * implement zero copy, the largest responses which are
		 * READ and READDIR will be adding a single buffer, or a small
		 * number of buffers to the ioq instead of copying into the
		 * 8k byte buffers.
		 */
		iov_count = XDR_IOVCOUNT(xioq->xdrs, xioq->write_start, fbytes);

		if (xioq->write_start == 0 ||
		    xioq->write_start == LAST_FRAG_XDR_UNITS ||
		    xioq->write_start == (2 * LAST_FRAG_XDR_UNITS)) {
			/* We need a fragment header, or to complete it. Look
			 * at xioq->frag_hdr_bytes_sent to know how many bytes
			 * of it we have sent so far.
			 */
			frag_needed = 1;
			frag_header = htonl((u_int32_t) (fbytes | last_frag));
			iov[0].iov_base = ((char *) &frag_header) +
						xioq->frag_hdr_bytes_sent;
			iov[0].iov_len = sizeof(frag_header) -
						xioq->frag_hdr_bytes_sent;
			frag_hdr_size = iov[0].iov_len;
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d iov[0].vio_head %p vio_length %z",
				__func__, xprt, xprt->xp_fd,
				iov[0].iov_base, iov[0].iov_len);
		}

		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: %p fd %d msg_iov %p remaining %"PRIu32
			" fbytes %"PRIu32" iov_count %"PRIu32
			" write_start %"PRIu32" end %"PRIu32
			" frag_needed %d frag_hdr_size %d",
			__func__, xprt, xprt->xp_fd, msg.msg_iov,
			remaining, fbytes, iov_count,
			xioq->write_start, end, frag_needed, frag_hdr_size);

		/* Get an xdr_vio corresponding to the bytes of this fragment */
		if (!XDR_FILLBUFS(xioq->xdrs, xioq->write_start, vio, fbytes)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() XDR_FILLBUFS failed", __func__);
			error = -1;
			break;
		}

		if (iov_count + frag_needed > PRESUMED_UIO_MAXIOV) {
			/* sendmsg can only take UIO_MAXIOV iovecs */
			iov_count = PRESUMED_UIO_MAXIOV - frag_needed;
		}

		/* Convert the xdr_vio to an iovec */
		for (i = 0; i < iov_count; i++) {
			iov[i + frag_needed].iov_base = vio[i].vio_head;
			iov[i + frag_needed].iov_len = vio[i].vio_length;
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d iov[%d].vio_head %p vio_length %z",
				__func__, xprt, xprt->xp_fd, i + frag_needed,
				iov[i + frag_needed].iov_base,
				iov[i + frag_needed].iov_len);
		}

		msg.msg_iov = iov;
		msg.msg_iovlen = iov_count + frag_needed;

again:
		XPRT_AUTO_TRACEPOINT(xprt, sendmsg, TRACE_DEBUG,
			"Calling sendmsg. remaining: {}, frag_needed: {}, "
			"iov_count: {}", remaining, frag_needed,
			iov_count);

		/* non-blocking write */
		errno = 0;
		result = sendmsg(xprt->xp_fd, &msg, MSG_DONTWAIT);
		error = errno;

		__warnx((error == EWOULDBLOCK || error == EAGAIN || error == 0)
				? TIRPC_DEBUG_FLAG_SVC_VC
				: TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d msg_iov %p sendmsg remaining %"
			PRIu32" result %ld error %s (%d)",
			__func__, xprt, xprt->xp_fd, msg.msg_iov,
			remaining, (long int) result,
			strerror(error), error);

		if (unlikely(result < 0)) {
			if (error == EWOULDBLOCK || error == EAGAIN) {
				/* Socket buffer full; don't destroy */
				error = EWOULDBLOCK;
				xioq->has_blocked = true;
			} else {
				error = result;
			}
			break;
		}

		if (result < frag_hdr_size) {
			/* We had a fragment headerr and didn't manage to send
			 * the entire thing. For example, we want to send 5 bytes data,
			 * i.e. ABCDE. The header size, i.e. frag_hdr_size, is 4.
			 * The first, we send 1 byte header, then frag_hdr_size
			 * substract result, and become 3. The second, we send the left 3
			 * bytes header and 2 bytes data, i.e. AB. So result is 5, then
			 * result substract frag_hdr_size, and become 2. And remaning
			 * substract result, and become 3. So next time, we send the
			 * remaining 3 bytes, i.e. CDE.
			 */
			xioq->frag_hdr_bytes_sent += result;
			iov[0].iov_base += result;
			iov[0].iov_len -= result;
			frag_hdr_size -= result;
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d iov[0].vio_head %p vio_length %z",
				__func__, xprt, xprt->xp_fd,
				iov[0].iov_base, iov[0].iov_len);
			/* Shortcut because we don't need to recompute the
			 * iovec.
			 */
			goto again;
		}

		/* At this point, the frag header must have been fully sent,
		 * go ahead and indicate that... Also deduct any fragment
		 * header bytes from result.
		 */
		xioq->frag_hdr_bytes_sent = sizeof(frag_header);
		result -= frag_hdr_size;
		frag_hdr_size = 0;

		/* Keep track of progress */
		remaining -= result;
		fbytes -= result;

		/* Keep track of progress in the xioq */
		xioq->write_start += result;

		if (fbytes == 0) {
			/* We completed sending a fragment. */
			xioq->frag_hdr_bytes_sent = 0;
			if (remaining > LAST_FRAG_XDR_UNITS) {
				fbytes = LAST_FRAG_XDR_UNITS;
			} else {
				fbytes = remaining;
			}
			frag_needed = 1;
		} else {
			frag_needed = 0;
		}
	} /* while */

	if (unlikely(vsize > MAXALLOCA))
		mem_free(iov, vsize);

	if (unlikely(isize > MAXALLOCA))
		mem_free(vio, isize);

	__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
		"%s: %p fd %d returning %s (%d)",
		__func__, xprt, xprt->xp_fd, strerror(error), error);

	return error;
}

void svc_ioq_write(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct xdr_ioq *xioq;
	struct poolq_entry *have;

	mutex_lock(&rec->writeq.qmutex);
	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_lock, TRACE_DEBUG,
		"Locked mutex");

	/* Process the xioq from the head of the xprt queue */
	have = TAILQ_FIRST(&rec->writeq.qh);

	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock, TRACE_DEBUG,
		"Unlocking mutex");
	mutex_unlock(&rec->writeq.qmutex);

	while (have != NULL) {
		int rc = 0;

		xioq = _IOQ(have);

		/* Save has blocked before state */
		bool has_blocked = xioq->has_blocked;

		/* do i/o unlocked */
		if (svc_work_pool.params.thrd_max
		 && !(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
			/* all systems are go! */
			rc = svc_ioq_flushv(xprt, xioq);
		}

		mutex_lock(&rec->writeq.qmutex);
		XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_lock,
			TRACE_DEBUG, "Locked mutex");

		if (rc < 0) {
			/* IO failed, destroy the XPRT but continue the loop in order to
			   release resources */
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d About to destroy - rc = %d",
				__func__, xprt, xprt->xp_fd, rc);
			SVC_DESTROY(xprt);
		} else if (rc == EWOULDBLOCK){
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d EWOULDBLOCK",
				__func__, xprt, xprt->xp_fd);
			/* Add to epoll and stop processing this xprt's queue */

			XPRT_AUTO_TRACEPOINT(
				xprt, write_would_block,
				TRACE_DEBUG, "Write got EWOULDBLOCK.");

			svc_rqst_evchan_write(xprt, xioq, has_blocked);

			XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock,
				TRACE_DEBUG, "Unlocking mutex");
			mutex_unlock(&rec->writeq.qmutex);
			break;
		} else {
			if (xioq->has_blocked) {
				__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
					"%s: %p fd %d COMPLETED AFTER BLOCKING",
					__func__, xprt, xprt->xp_fd);

				XPRT_AUTO_TRACEPOINT(
					xprt, write_complete_blocked,
					TRACE_DEBUG, "Write completed after "
					"blocking.");

				svc_rqst_xprt_send_complete(xprt);
			} else {
				__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
					"%s: %p fd %d COMPLETED",
					__func__, xprt, xprt->xp_fd);

				XPRT_AUTO_TRACEPOINT(
					xprt, write_completed,
					TRACE_DEBUG,
					"Write completed. has_blocked: {}",
					xioq->has_blocked);
			}
		}

		/* Dequeue the completed request */
		TAILQ_REMOVE(&rec->writeq.qh, have, q);

		/* Fetch the next request */
		have = TAILQ_FIRST(&rec->writeq.qh);
		mutex_unlock(&rec->writeq.qmutex);

		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: %p fd %d About to release",
			__func__, xprt, xprt->xp_fd);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		XDR_DESTROY(xioq->xdrs);
	}
}

static void
svc_ioq_write_callback(struct work_pool_entry *wpe)
{
	struct xdr_ioq *xioq = opr_containerof(wpe, struct xdr_ioq, ioq_wpe);

	svc_ioq_write(xioq->xdrs[0].x_lib[1]);
}

void
svc_ioq_write_now(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	bool was_empty;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);


	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_lock,
		TRACE_DEBUG, "Locking mutex");
	mutex_lock(&rec->writeq.qmutex);

	was_empty = TAILQ_FIRST(&rec->writeq.qh) == NULL;

	/* always queue output requests on the duplex record's writeq */
	TAILQ_INSERT_TAIL(&rec->writeq.qh, &(xioq->ioq_s), q);

	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock,
		TRACE_DEBUG, "Unlocking mutex");
	mutex_unlock(&rec->writeq.qmutex);

	if (was_empty) {
		/* handle this output request without queuing, then any
		 * additional output requests without a task switch (using this
		 * thread).
		 */
		svc_ioq_write(xprt);
	}
}

/*
 * Handle rare case of first output followed by heavy traffic that prevents the
 * original thread from continuing for too long.
 *
 * In the more common case, server traffic will already have begun and this
 * will rapidly queue the output and return.
 */
void
svc_ioq_write_submit(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	bool was_empty;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);

	mutex_lock(&rec->writeq.qmutex);
	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_lock, TRACE_DEBUG,
		"Locked mutex");

	was_empty = TAILQ_FIRST(&rec->writeq.qh) == NULL;

	/* always queue output requests on the duplex record's writeq */
	TAILQ_INSERT_TAIL(&rec->writeq.qh, &(xioq->ioq_s), q);

	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock,
		TRACE_DEBUG, "Unlocking mutex");
	mutex_unlock(&rec->writeq.qmutex);

	if (was_empty) {
		/* Schedule work to process output for this duplex record. */
		xioq->ioq_wpe.fun = svc_ioq_write_callback;
		work_pool_submit(&svc_work_pool, &xioq->ioq_wpe);
	}
}
