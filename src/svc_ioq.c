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
#include <sched.h>
#include <misc/timespec.h>
#ifdef __linux__
#include <linux/errqueue.h>
#endif

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
#ifdef USE_TLS
#include "tls.h"
#endif

#define LAST_FRAG ((u_int32_t)(1 << 31))
#define LAST_FRAG_XDR_UNITS ((LAST_FRAG - 1) & ~(BYTES_PER_XDR_UNIT - 1))
#define MAXALLOCA (256)

#if defined(__linux__) && defined(MSG_ZEROCOPY) && defined(SO_ZEROCOPY)
#define HAVE_TCP_ZEROCOPY 1

/*
 * Cookies are the kernel's per-socket u32 sk_zckey, which wraps.  A long-lived
 * connection doing 64k ZC sends reaches 2^32 cookies after ~256TB, so every
 * comparison has to be modular rather than a plain magnitude test.
 */
#define ZC_COOKIE_LT(a, b)	((int32_t)((a) - (b)) < 0)
#define ZC_COOKIE_GE(a, b)	((int32_t)((a) - (b)) >= 0)

/*
 * svc_ioq_zc_note_send - record a successful MSG_ZEROCOPY sendmsg for xioq.
 *
 * Called once per successful sendmsg(MSG_ZEROCOPY) call.  The kernel assigns
 * a monotonically-increasing 32-bit cookie to each such call on the socket;
 * this function mirrors that assignment so we can later match errqueue
 * completions back to the right xioq.
 *
 * On the first ZC send for this xioq, zc_cookie_lo is set to the current
 * per-connection counter (zc_next_cookie), which is then incremented.
 * zc_outstanding tracks how many ZC sendmsgs are still in flight for this
 * xioq; the xioq cannot be freed until that count reaches zero.
 *
 * Locking: none (privately accessed by the serialized transmit thread).
 */
static void
svc_ioq_zc_note_send(struct rpc_dplx_rec *rec, struct xdr_ioq *xioq)
{
	if (xioq->zc_outstanding == 0)
		xioq->zc_cookie_lo = rec->zc_next_cookie;
	rec->zc_next_cookie++;
	xioq->zc_outstanding++;
}

/*
 * svc_ioq_zc_xioq_done - test whether all ZC sends for xioq are complete.
 *
 * Returns true if every MSG_ZEROCOPY sendmsg issued for this xioq has been
 * acknowledged by the kernel via the socket errqueue.
 *
 * The cookie range assigned to this xioq is [zc_cookie_lo,
 * zc_cookie_lo + zc_outstanding).  Completion is tracked by zc_acked, a
 * per-connection watermark: all cookies strictly below zc_acked have been
 * confirmed complete (in-order or via OOO merging).  If the exclusive upper
 * bound of this xioq's range (zc_cookie_lo + zc_outstanding) is at or below
 * zc_acked, every send is done and the xioq can be freed.
 *
 * A zc_outstanding of zero means no ZC sends were ever issued; returns true
 * immediately so the caller can free the xioq on the normal path.
 *
 * Locking: caller must hold zc_lock.
 */
static bool
svc_ioq_zc_xioq_done(struct rpc_dplx_rec *rec, struct xdr_ioq *xioq)
{
	uint32_t last;

	if (xioq->zc_outstanding == 0)
		return true;
	last = xioq->zc_cookie_lo + xioq->zc_outstanding;
	return ZC_COOKIE_GE(rec->zc_acked, last);
}

/*
 * svc_ioq_zc_reap_locked - free completed ZC xioqs from the deferred queue.
 *
 * Walks rec->zcq and destroys every xioq for which all MSG_ZEROCOPY sends
 * have been acknowledged (svc_ioq_zc_xioq_done() returns true).  Each freed
 * xioq increments *release_count so the caller can drive the matching
 * SVC_RELEASE calls outside the mutex (SVC_RELEASE can block; we must not
 * hold writeq.qmutex across it).
 *
 * Entries whose sends are still pending are left in zcq and will be visited
 * again the next time svc_ioq_zc_drain() processes an errqueue notification.
 *
 * Locking: caller must hold zc_lock.
 */
/*
 * svc_ioq_zc_harvest_locked - move completed xioqs off zcq into harvest_list.
 *
 * Called under zc_lock.  Does only pointer surgery and watermark
 * checks — no memory allocation, no XDR_DESTROY, no SVC_RELEASE.
 * Callers must walk harvest_list outside the mutex to do the heavy work.
 *
 * Returns the number of xioqs harvested (= how many SVC_RELEASE calls needed).
 */
static int
svc_ioq_zc_harvest_locked(struct rpc_dplx_rec *rec,
			   zc_harvest_list_t *harvest_list)
{
	struct poolq_entry *have;
	struct poolq_entry *next;
	int count = 0;

	have = TAILQ_FIRST(&rec->zcq);
	while (have != NULL) {
		struct xdr_ioq *xioq = _IOQ(have);

		next = TAILQ_NEXT(have, q);
		if (svc_ioq_zc_xioq_done(rec, xioq)) {
			TAILQ_REMOVE(&rec->zcq, have, q);
			uint32_t _inf = atomic_dec_uint32_t(&rec->zc_inflight);
			__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
				"%s: fd %d reap: inflight %"PRIu32"->%"PRIu32,
				__func__, rec->xprt.xp_fd,
				_inf + 1, _inf);
			xioq->zc_outstanding = 0;
			/* Move to caller's list — XDR_DESTROY happens outside mutex */
			TAILQ_INSERT_TAIL(harvest_list, have, q);
			count++;
		}
		have = next;
	}
	return count;
}

/* Destroy all xioqs on harvest_list (must be called without writeq.qmutex). */
static void
svc_ioq_zc_destroy_harvested(zc_harvest_list_t *harvest_list)
{
	struct poolq_entry *have;

	while ((have = TAILQ_FIRST(harvest_list)) != NULL) {
		struct xdr_ioq *xioq = _IOQ(have);

		TAILQ_REMOVE(harvest_list, have, q);
		XDR_DESTROY(xioq->xdrs);
	}
}

/* Test/set a completion bit at offset off from the base of the window. */
static inline bool
svc_ioq_zc_bit_test(const struct rpc_dplx_rec *rec, uint32_t off)
{
	return (rec->zc_ooo_bits[off >> 6] & ((uint64_t)1 << (off & 63))) != 0;
}

static inline void
svc_ioq_zc_bit_set(struct rpc_dplx_rec *rec, uint32_t off)
{
	rec->zc_ooo_bits[off >> 6] |= (uint64_t)1 << (off & 63);
}

/*
 * svc_ioq_zc_window_slide - shift the completion window down by n cookies.
 *
 * Called whenever zc_acked advances by n, so that bit k keeps meaning
 * "cookie zc_acked + k is confirmed".  Bits shifted off the bottom are
 * cookies now below the watermark and are simply discarded.
 *
 * Locking: caller must hold zc_lock.
 */
static void
svc_ioq_zc_window_slide(struct rpc_dplx_rec *rec, uint32_t n)
{
	uint32_t words = n >> 6;
	uint32_t bits = n & 63;
	uint32_t i;

	if (words >= SVC_ZC_ACK_WORDS) {
		/* Slid past everything we were tracking. */
		memset(rec->zc_ooo_bits, 0, sizeof(rec->zc_ooo_bits));
		return;
	}

	/* Writes only ever touch indices at or below the ones already read,
	 * so a single forward pass is safe without a scratch buffer.
	 */
	for (i = 0; i < SVC_ZC_ACK_WORDS; i++) {
		uint64_t v = (i + words < SVC_ZC_ACK_WORDS)
				? rec->zc_ooo_bits[i + words] : 0;

		if (bits != 0) {
			v >>= bits;
			if (i + words + 1 < SVC_ZC_ACK_WORDS)
				v |= rec->zc_ooo_bits[i + words + 1] <<
					(64 - bits);
		}
		rec->zc_ooo_bits[i] = v;
	}
}

/*
 * svc_ioq_zc_absorb_ooo - slide the watermark over confirmed cookies.
 *
 * The kernel delivers MSG_ZEROCOPY completions via the socket errqueue as
 * sock_extended_err messages carrying an inclusive [lo, hi] cookie range.
 * Under normal in-order delivery these touch the current zc_acked watermark
 * and svc_ioq_zc_complete_locked() advances it directly.  On retransmission,
 * or when a send that fell back to copy completes ahead of true ZC sends,
 * a later range can be reported first; those land in the zc_ooo_bits window.
 *
 * This runs after every recorded completion and advances zc_acked over the
 * run of confirmed cookies at the base of the window.  It stops at the first
 * cookie that is NOT confirmed, so the watermark can never cross a gap.
 *
 * Locking: caller must hold zc_lock.
 */
static void
svc_ioq_zc_absorb_ooo(struct rpc_dplx_rec *rec)
{
	uint32_t advance = 0;

	/* Whole words first — an all-ones word is 64 consecutive completions,
	 * which is the common case when the kernel batches notifications.
	 */
	while (advance <= SVC_ZC_ACK_WINDOW - 64 &&
	       rec->zc_ooo_bits[advance >> 6] == ~(uint64_t)0)
		advance += 64;

	/* Then bit at a time, up to the first hole. */
	while (advance < SVC_ZC_ACK_WINDOW && svc_ioq_zc_bit_test(rec, advance))
		advance++;

	if (advance == 0)
		return;

	rec->zc_acked += advance;
	svc_ioq_zc_window_slide(rec, advance);
}

/*
 * svc_ioq_zc_complete_locked - record a completed [lo, hi] cookie range and
 * harvest any zcq entries that are now fully acknowledged.
 *
 * Called from svc_ioq_zc_drain() with zc_lock already held.  Done
 * xioqs are moved into harvest_list (pointer surgery only — no XDR_DESTROY
 * under the lock).  The caller destroys them after dropping the mutex.
 *
 * Returns the number of xioqs moved onto harvest_list, which is exactly the
 * number of SVC_RELEASE calls the caller owes.  Do not recompute this by
 * walking harvest_list: the list accumulates across errqueue reads, so a
 * re-walk both costs O(n^2) and reports the running total as if it were this
 * call's contribution.
 *
 * Locking: caller must hold zc_lock.  Must NOT touch the mutex.
 */
static int
svc_ioq_zc_complete_locked(struct rpc_dplx_rec *rec, uint32_t lo, uint32_t hi,
			    zc_harvest_list_t *harvest_list)
{
	SVCXPRT *xprt = &rec->xprt;
	uint32_t c;

	/* Malformed range (hi before lo in cookie space). */
	if (ZC_COOKIE_LT(hi, lo))
		return 0;

	/* Entirely below the watermark — already accounted for. */
	if (ZC_COOKIE_LT(hi, rec->zc_acked))
		return 0;

	/* Clamp: cookies below the watermark are confirmed by definition. */
	if (ZC_COOKIE_LT(lo, rec->zc_acked))
		lo = rec->zc_acked;

	if (lo == rec->zc_acked) {
		/* In order.  The range is contiguous from the watermark, so
		 * advancing straight to hi + 1 crosses no unconfirmed cookie
		 * even when the kernel merged a range longer than the window.
		 */
		uint32_t span = hi - lo + 1;

		rec->zc_acked += span;
		svc_ioq_zc_window_slide(rec, span);
	} else {
		/* Out of order: record each cookie in the window.  Bounded by
		 * SVC_ZC_ACK_WINDOW iterations since lo > zc_acked and we stop
		 * at the window edge.
		 */
		for (c = lo; ; c++) {
			uint32_t off = c - rec->zc_acked;

			if (off >= SVC_ZC_ACK_WINDOW) {
				/*
				 * Reordered further than the window can track.
				 * Drop the completion rather than force the
				 * watermark past the gap: zc_acked is the only
				 * proof svc_ioq_zc_xioq_done() has that the
				 * kernel released an xioq's pages, so forging
				 * it would XDR_DESTROY buffers still pinned in
				 * the retransmit queue and put freed memory on
				 * the wire.  Losing the completion instead only
				 * strands the xioq on zcq until teardown.
				 */
				rec->zc_window_drops++;
				__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
					"%s: fd %d ZC completion %" PRIu32
					" beyond ack window (acked %" PRIu32
					", inflight %" PRIu32
					", drops %" PRIu32 ")",
					__func__, xprt->xp_fd, c,
					rec->zc_acked,
					atomic_fetch_uint32_t(
						&rec->zc_inflight),
					rec->zc_window_drops);
				break;
			}
			svc_ioq_zc_bit_set(rec, off);
			if (c == hi)
				break;
		}
	}

	svc_ioq_zc_absorb_ooo(rec);
	return svc_ioq_zc_harvest_locked(rec, harvest_list);
}

static bool
svc_ioq_zc_parse_serr(struct cmsghdr *cmsg, uint32_t *lo, uint32_t *hi,
		      bool *copied)
{
	struct sock_extended_err *serr;

	*copied = false;

	if (cmsg->cmsg_level == SOL_IP) {
		if (cmsg->cmsg_type != IP_RECVERR)
			return false;
	} else if (cmsg->cmsg_level == SOL_IPV6) {
		if (cmsg->cmsg_type != IPV6_RECVERR)
			return false;
	} else {
		return false;
	}

	if (cmsg->cmsg_len < CMSG_LEN(sizeof(*serr)))
		return false;

	serr = (struct sock_extended_err *)CMSG_DATA(cmsg);
	if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY)
		return false;

	*lo = serr->ee_info;
	*hi = serr->ee_data;
#ifdef SO_EE_CODE_ZEROCOPY_COPIED
	*copied = (serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED) != 0;
#endif
	return true;
}

/*
 * svc_ioq_zc_drain_collect - drain the errqueue and collect completed xioqs.
 *
 * Reads all pending MSG_ZEROCOPY completions from the socket errqueue,
 * advances zc_acked, and moves fully-done xioqs off zcq into harvest_list
 * (pointer surgery only — no XDR_DESTROY, no SVC_RELEASE under the mutex).
 *
 * Returns the number of xioqs harvested.  The caller must call
 * svc_ioq_zc_drain_release() to destroy them and drop the xprt refcounts.
 */
int
svc_ioq_zc_drain_collect(SVCXPRT *xprt,
			  zc_harvest_list_t *harvest_list)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct msghdr msg;
	struct cmsghdr *cmsg;
	char control[512];
	uint32_t lo;
	uint32_t hi;
	bool copied;
	int total = 0;

	if (!__svc_params->tcp_zerocopy_enabled)
		return 0;

	/* zc_sock_ok going false (SO_EE_CODE_ZEROCOPY_COPIED) only stops us
	 * issuing *new* ZC sends.  Anything already on zcq still needs its
	 * completions drained, so keep draining while xioqs are outstanding.
	 */
	if (!rec->zc_sock_ok && atomic_fetch_uint32_t(&rec->zc_inflight) == 0)
		return 0;

	for (;;) {
		int n = 0;

		memset(&msg, 0, sizeof(msg));
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);

		if (recvmsg(xprt->xp_fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
			break;

		if (msg.msg_flags & MSG_CTRUNC) {
			__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
				"%s: fd %d MSG_CTRUNC on errqueue; cookies may be lost",
				__func__, xprt->xp_fd);
		}

		/*
		 * Critical section: advance zc_acked and move completed xioqs
		 * off zcq into harvest_list.  No XDR_DESTROY — pointer surgery
		 * only so the mutex is held as briefly as possible.
		 */
		mutex_lock(&rec->zc_lock);
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
		     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (!svc_ioq_zc_parse_serr(cmsg, &lo, &hi, &copied))
				continue;

			n += svc_ioq_zc_complete_locked(rec, lo, hi,
							harvest_list);

			if (copied && rec->zc_sock_ok) {
				rec->zc_sock_ok = false;
				__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
					"%s: fd %d SO_EE_CODE_ZEROCOPY_COPIED; disabling MSG_ZEROCOPY",
					__func__, xprt->xp_fd);
			}
		}
		mutex_unlock(&rec->zc_lock);

		total += n;
		__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
			"%s: fd %d drain iter: reaped %d (total %d)"
			" inflight %"PRIu32,
			__func__, xprt->xp_fd, n, total,
			atomic_fetch_uint32_t(&rec->zc_inflight));
	}
	return total;
}

/*
 * svc_ioq_zc_drain_release - destroy harvested xioqs and drop xprt refcounts.
 *
 * Called after IOQ_WRITING is cleared so the TX path is unblocked before
 * any slow memory-freeing work happens.
 */
void
svc_ioq_zc_drain_release(SVCXPRT *xprt,
			  zc_harvest_list_t *harvest_list,
			  int release_count)
{
	__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
		"%s: fd %d drain done: releasing %d inflight %"PRIu32,
		__func__, xprt->xp_fd,
		release_count,
		atomic_fetch_uint32_t(&REC_XPRT(xprt)->zc_inflight));
	svc_ioq_zc_destroy_harvested(harvest_list);

	while (release_count-- > 0)
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
}

/* Convenience wrapper for callers that don't need to split the two phases. */
void
svc_ioq_zc_drain(SVCXPRT *xprt)
{
	zc_harvest_list_t harvest_list;
	int release_count;

	TAILQ_INIT(&harvest_list);
	release_count = svc_ioq_zc_drain_collect(xprt, &harvest_list);
	if (release_count > 0)
		svc_ioq_zc_drain_release(xprt, &harvest_list, release_count);
}

/* Teardown grace period for pages still pinned by MSG_ZEROCOPY:
 * SVC_ZC_TEARDOWN_TRIES * SVC_ZC_TEARDOWN_NSEC (20 * 500us = 10ms).
 */
#define SVC_ZC_TEARDOWN_TRIES 20
#define SVC_ZC_TEARDOWN_NSEC (500 * 1000)

void
svc_ioq_zc_release_all(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	zc_harvest_list_t harvest_list;
	struct poolq_entry *have;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SVC_ZC_TEARDOWN_NSEC,
	};
	int release_count = 0;
	int tries;

	TAILQ_INIT(&harvest_list);

	/*
	 * Everything left on zcq is about to be XDR_DESTROYed, but our fd is
	 * still open at this point — it is closed later, in xp_destroy — and
	 * the kernel may still hold these pages in the retransmit queue.
	 * Freeing them here is what puts freed memory on the wire.
	 *
	 * Send FIN so the peer acknowledges, then drain the errqueue for a
	 * bounded time.  In the normal case (live peer, data already acked)
	 * the pages come back and the frees below are clean.  Whatever is
	 * still pinned after the grace period is freed anyway — there is
	 * nowhere left to defer to — but that is now a logged rarity rather
	 * than every single teardown.
	 */
	if (__svc_params->tcp_zerocopy_enabled &&
	    atomic_fetch_uint32_t(&rec->zc_inflight) > 0 &&
	    xprt->xp_fd != RPC_ANYFD) {
		(void)shutdown(xprt->xp_fd, SHUT_WR);

		for (tries = 0; tries < SVC_ZC_TEARDOWN_TRIES; tries++) {
			svc_ioq_zc_drain(xprt);
			if (atomic_fetch_uint32_t(&rec->zc_inflight) == 0)
				break;
			nanosleep(&ts, NULL);
		}

		if (atomic_fetch_uint32_t(&rec->zc_inflight) > 0) {
			__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
				"%s: fd %d teardown: %" PRIu32
				" xioq(s) still pinned after grace period;"
				" freeing before close (acked %" PRIu32
				", next %" PRIu32 ", window drops %" PRIu32 ")",
				__func__, xprt->xp_fd,
				atomic_fetch_uint32_t(&rec->zc_inflight),
				rec->zc_acked, rec->zc_next_cookie,
				rec->zc_window_drops);
		}
	}

	mutex_lock(&rec->zc_lock);
	while ((have = TAILQ_FIRST(&rec->zcq)) != NULL) {
		uint32_t _inf = atomic_dec_uint32_t(&rec->zc_inflight);

		__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
			"%s: fd %d release_all: inflight %"PRIu32"->%"PRIu32,
			__func__, xprt->xp_fd,
			_inf + 1, _inf);
		TAILQ_REMOVE(&rec->zcq, have, q);
		TAILQ_INSERT_TAIL(&harvest_list, have, q);
		release_count++;
	}
	memset(rec->zc_ooo_bits, 0, sizeof(rec->zc_ooo_bits));
	mutex_unlock(&rec->zc_lock);

	/* XDR_DESTROY outside the mutex. */
	svc_ioq_zc_destroy_harvested(&harvest_list);

	while (release_count-- > 0)
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
}
#else
#define HAVE_TCP_ZEROCOPY 0

void
svc_ioq_zc_drain(SVCXPRT *xprt)
{
	(void)xprt;
}

void
svc_ioq_zc_release_all(SVCXPRT *xprt)
{
	(void)xprt;
}
#endif /* __linux__ && MSG_ZEROCOPY && SO_ZEROCOPY */

/* Returns 0 on success, EWOULDBLOCK if would block, <0 on error
 */
static inline int
svc_ioq_flushv(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct msghdr msg;
	struct iovec *iov;
	struct xdr_vio *vio;
	ssize_t result;
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
		int send_flags;
#if HAVE_TCP_ZEROCOPY
		bool use_zc = false;
#endif

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
		 * Large READ/READDIR payloads are already attached via
		 * xdr_ioq_putbufs(UIO_FLAG_REFER) — not copied into the 8k
		 * XDR buffers. MSG_ZEROCOPY pins those referred pages.
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
			/* Store on xioq so MSG_ZEROCOPY can pin durable memory
			 * (never the stack local).
			 */
			xioq->zc_frag_header = htonl((u_int32_t)(fbytes | last_frag));
			iov[0].iov_base = ((char *)&xioq->zc_frag_header) +
						xioq->frag_hdr_bytes_sent;
			iov[0].iov_len = sizeof(xioq->zc_frag_header) -
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

		send_flags = MSG_DONTWAIT;
		if (fbytes < remaining)
			send_flags |= MSG_MORE;
#if HAVE_TCP_ZEROCOPY
		use_zc = false;
		if (__svc_params->tcp_zerocopy_enabled &&
		    REC_XPRT(xprt)->zc_sock_ok &&
		    remaining >= __svc_params->tcp_zerocopy_min_bytes) {
			send_flags |= MSG_ZEROCOPY;
			use_zc = true;
		}
#endif

again:
		XPRT_AUTO_TRACEPOINT(xprt, sendmsg, TRACE_DEBUG,
			"Calling sendmsg. remaining: {}, frag_needed: {}, "
			"iov_count: {}", remaining, frag_needed,
			iov_count);

		/* non-blocking write */
		errno = 0;

#ifdef USE_TLS
		/* TLS encrypts into its own buffers; MSG_ZEROCOPY on the
		 * cleartext iov is not applicable.
		 */
#if HAVE_TCP_ZEROCOPY
		use_zc = false;
		result = svc_tls_send(xprt, &msg, send_flags & ~MSG_ZEROCOPY);
#else
		result = svc_tls_send(xprt, &msg, send_flags);
#endif
#else
		result = sendmsg(xprt->xp_fd, &msg, send_flags);
#if HAVE_TCP_ZEROCOPY
		/* ENOBUFS: ZC path unavailable; retry once without it. */
		if (unlikely(result < 0 && use_zc && errno == ENOBUFS)) {
			send_flags &= ~MSG_ZEROCOPY;
			use_zc = false;
			errno = 0;
			result = sendmsg(xprt->xp_fd, &msg, send_flags);
		}
#endif
#endif

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

#if HAVE_TCP_ZEROCOPY
		if (use_zc && result > 0) {
			__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
				"%s: fd %d ZC sendmsg: result %ld"
				" remaining %"PRIu32" zc_outstanding %"PRIu32,
				__func__, xprt->xp_fd,
				(long)result, remaining,
				xioq->zc_outstanding);
			svc_ioq_zc_note_send(REC_XPRT(xprt), xioq);
		}
#endif

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
		xioq->frag_hdr_bytes_sent = sizeof(xioq->zc_frag_header);
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
	bool destroy_xprt = false;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 0,
	};
	uint32_t local_counter = 0;

	while (atomic_postset_uint16_t_bits(&xprt->xp_flags,
				SVC_XPRT_FLAG_IOQ_WRITING)
		       & SVC_XPRT_FLAG_IOQ_WRITING) {
		if(__svc_params->tcp_zerocopy_enabled) {
			sched_yield();
		} else {
			nanosleep(&ts, NULL);
		}
		if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
			XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, ioq_working, TRACE_INFO,
				"xprt is being cleared, no need for transmit");
			return;
		}
	}

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

		if (rc < 0 || (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
			/* IO failed, destroy the XPRT but continue the loop in order to
			   release resources */
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: %p fd %d About to destroy - rc = %d",
				__func__, xprt, xprt->xp_fd, rc);
			destroy_xprt = true;
			XPRT_AUTO_TRACEPOINT(xprt, destroy_xprt,
				TRACE_INFO, "IO failed, destroy xprt.");
			mutex_unlock(&rec->writeq.qmutex);
			break;
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

#if HAVE_TCP_ZEROCOPY
		if (xioq->zc_outstanding > 0) {
			bool done;
			++local_counter;
			mutex_lock(&rec->zc_lock);
			done = svc_ioq_zc_xioq_done(rec, xioq);
			if (!done) {
				/*
				 * Pages still pinned by the kernel — defer this
				 * xioq's free onto zcq.
				 * svc_ioq_zc_drain() when the ACKs arrive and
				 * svc_ioq_zc_reap_locked() will do the
				 * SVC_RELEASE + XDR_DESTROY at that point.
				 *
				 */
				TAILQ_INSERT_TAIL(&rec->zcq, &(xioq->ioq_s), q);
				uint32_t _inf = atomic_inc_uint32_t(&rec->zc_inflight);
				mutex_unlock(&rec->zc_lock);
				__warnx(TIRPC_DEBUG_FLAG_ZEROCOPY_TX,
				 "%s: fd %d enqueue: inflight %"PRIu32"->%"PRIu32
				 " zc_outstanding %"PRIu32,
				 __func__, xprt->xp_fd,
				 _inf - 1, _inf, xioq->zc_outstanding);
				/* Skip SVC_RELEASE/XDR_DESTROY — zcq owns it */
				goto next_reply;
			}
			mutex_unlock(&rec->zc_lock);
			xioq->zc_outstanding = 0;
		}
#endif

		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: %p fd %d About to release",
			__func__, xprt, xprt->xp_fd);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		XDR_DESTROY(xioq->xdrs);

next_reply:;

	}
#if HAVE_TCP_ZEROCOPY
	{
		/* Unblock other threads waiting to TX on this xprt before
		 * doing the slow XDR_DESTROY + SVC_RELEASE work.
		 */
		atomic_postclear_uint16_t_bits(&xprt->xp_flags,
					       SVC_XPRT_FLAG_IOQ_WRITING);
		if ((__svc_params->tcp_zerocopy_enabled) &&
		    (local_counter < atomic_fetch_uint32_t(&rec->zc_inflight) ||
		     local_counter > 8)) {
			svc_ioq_zc_drain(xprt);
			local_counter = 0;
		}

		/*
		 * New data to send may have arrived in writeq while we were draining.
		 * be opportunistic here, the possiblity of netwrok buffer being
		 * full is less because of ZeroCopy, and threads might be busy
		 * somewhere else if there is more data availbale in queue to
		 * process, process it.
		 * Only loop if: xprt is alive, not blocked on EWOULDBLOCK, and
		 * no other thread grabbed IOQ_WRITING in the gap.
		 */
		if (__svc_params->tcp_zerocopy_enabled &&
		    !destroy_xprt &&
		    !(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) &&
		    !(atomic_postset_uint16_t_bits(&xprt->xp_flags,
					SVC_XPRT_FLAG_IOQ_WRITING)
		      & SVC_XPRT_FLAG_IOQ_WRITING)) {
			mutex_lock(&rec->writeq.qmutex);
			have = TAILQ_FIRST(&rec->writeq.qh);
			mutex_unlock(&rec->writeq.qmutex);
			if (have && !(_IOQ(have)->has_blocked)) {
				goto next_reply;
			}
			/* Nothing to send or socket blocked — drop the flag. */
			atomic_postclear_uint16_t_bits(&xprt->xp_flags,
						SVC_XPRT_FLAG_IOQ_WRITING);
		}
	}
#else
	atomic_postclear_uint16_t_bits(&xprt->xp_flags,
				SVC_XPRT_FLAG_IOQ_WRITING);
#endif

	if (destroy_xprt) {
		SVC_DESTROY(xprt);
	}
}

static void
svc_ioq_write_callback(struct work_pool_entry *wpe)
{
	struct xdr_ioq *xioq = opr_containerof(wpe, struct xdr_ioq, ioq_wpe);

	svc_ioq_write(xioq->xdrs[0].x_lib[1]);
}


static bool add_ioq_to_write_queue(SVCXPRT *xprt, struct xdr_ioq *xioq) {
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	bool need_processing;

	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_lock,
		TRACE_DEBUG, "Locking mutex");
	mutex_lock(&rec->writeq.qmutex);
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		//Queue already cleared, do not add more requests
		XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock,
			TRACE_DEBUG, "Unlocking mutex");
		mutex_unlock(&rec->writeq.qmutex);
		XDR_DESTROY(xioq->xdrs);
		return false;
	}
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	need_processing = TAILQ_FIRST(&rec->writeq.qh) == NULL;
	/* always queue output requests on the duplex record's writeq */
	TAILQ_INSERT_TAIL(&rec->writeq.qh, &(xioq->ioq_s), q);

	XPRT_UNIQUE_AUTO_TRACEPOINT(xprt, mutex_unlock,
		TRACE_DEBUG, "Unlocking mutex");
	mutex_unlock(&rec->writeq.qmutex);
	return need_processing;
}

void
svc_ioq_write_now(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	const bool need_processing = add_ioq_to_write_queue(xprt, xioq);
	if (need_processing) {
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
	const bool need_processing = add_ioq_to_write_queue(xprt, xioq);

	if (need_processing) {
		/* Schedule work to process output for this duplex record. */
		xioq->ioq_wpe.fun = svc_ioq_write_callback;
		work_pool_submit(&svc_work_pool, &xioq->ioq_wpe);
	}
}
