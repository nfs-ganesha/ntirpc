/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2013-2017 Red Hat, Inc. and/or its affiliates.
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
#include <sys/cdefs.h>

/*
 * xdr_mem.h, XDR implementation using memory buffers.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * If you have some data to be interpreted as external data representation
 * or to be converted to external data representation in a memory buffer,
 * then this is the package for you.
 *
 */

#include "namespace.h"
#include <sys/types.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#endif

#include <string.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include "un-namespace.h"

typedef bool (*dummyfunc3)(XDR *, int, void *);
typedef bool (*dummy_getbufs)(XDR *, xdr_uio *, u_int);
typedef bool (*dummy_newbuf)(struct rpc_xdr *);

static const struct xdr_ops xdrmem_ops_aligned;

/*
 * The procedure xdrmem_create initializes a stream descriptor for a
 * memory buffer.
 */
void
xdrmem_ncreate(XDR *xdrs, char *addr, u_int size, enum xdr_op op)
{
	xdrs->x_op = op;
	if ((uintptr_t)addr & (sizeof(int32_t) - 1)) {
		abort();
	} else {
		xdrs->x_ops = &xdrmem_ops_aligned;
	}
	xdrs->x_public = NULL;
	xdrs->x_private = NULL;
	xdrs->x_lib[0] = NULL;
	xdrs->x_lib[1] = NULL;
	xdrs->x_data = addr;
	xdrs->x_v.vio_base = addr;
	xdrs->x_v.vio_head = addr;
	switch (op) {
	case XDR_ENCODE:
		xdrs->x_v.vio_tail = addr;
		break;
	case XDR_DECODE:
		xdrs->x_v.vio_tail = addr + size;
		break;
	default:
		abort();
		break;
	};
	xdrs->x_v.vio_wrap = addr + size;
	xdrs->x_base = &xdrs->x_v;
}

/* ARGSUSED */
static bool
xdrmem_getunit(XDR *xdrs, uint32_t *p)
{
	/* xdr_get* exceeds contiguous buffer */
	return (false);
}

/* ARGSUSED */
static bool
xdrmem_putunit(XDR *xdrs, const uint32_t v)
{
	/* xdr_put* exceeds contiguous buffer */
	return (false);
}

/* in glibc 2.14+ x86_64, memcpy no longer tries to handle overlapping areas,
 * see Fedora Bug 691336 (NOTABUG); we dont permit overlapping segments,
 * so memcpy may be a small win over memmove.
 */

static bool
xdrmem_getbytes(XDR *xdrs, char *addr, u_int len)
{
	uint8_t *future = xdrs->x_data + len;

	if (future > xdrs->x_v.vio_tail)
		return (false);
	memcpy(addr, xdrs->x_data, len);
	xdrs->x_data = future;
	return (true);
}

static bool
xdrmem_putbytes(XDR *xdrs, const char *addr, u_int len)
{
	uint8_t *future = xdrs->x_data + len;

	if (future > xdrs->x_v.vio_wrap)
		return (false);
	memcpy(xdrs->x_data, addr, len);
	xdrs->x_data = future;
	return (true);
}

static u_int
xdrmem_getpos(XDR *xdrs)
{
	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	return ((uintptr_t)xdrs->x_data - (uintptr_t)xdrs->x_v.vio_head);
}

static u_int
xdrmem_getstartdatapos(XDR *xdrs, u_int start, u_int datalen)
{
	return start;
}

static u_int
xdrmem_getenddatapos(XDR *xdrs, u_int start, u_int datalen)
{
	return start + datalen;
}

static bool
xdrmem_setpos(XDR *xdrs, u_int pos)
{
	uint8_t *future = xdrs->x_v.vio_head + pos;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	if (future > xdrs->x_v.vio_wrap)
		return (false);
	xdrs->x_data = future;
	return (true);
}

/* ARGSUSED */
static void xdrmem_destroy(XDR *xdrs)
{
}

static bool
xdrmem_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
	int ix;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s Before putbufs - pos %lu",
		__func__, (unsigned long) XDR_GETPOS(xdrs));

	for (ix = 0; ix < uio->uio_count; ++ix) {
		xdr_vio *v = &(uio->uio_vio[ix]);

		if (!XDR_PUTBYTES(xdrs, v->vio_head, v->vio_length))
			return (FALSE);

		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s After putbufs Examining vio %p (base %p head %p tail %p wrap %p len %lu) pos %lu",
			__func__, v, v->vio_base, v->vio_head,
			v->vio_tail, v->vio_wrap, (unsigned long) v->vio_length,
			(unsigned long) XDR_GETPOS(xdrs));
	}

	if (!(--uio->uio_references)) {
		if (uio->uio_release) {
			uio->uio_release(uio, UIO_FLAG_NONE);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() memory leak, unexpected or no release flags (%u)\n",
				__func__, uio->uio_flags);
			abort();
		}
	}

	return (TRUE);
}

static bool
xdrmem_noop(void)
{
	return (false);
}

static int
xdrmem_iovcount(XDR *xdrs, u_int start, u_int datalen)
{
	if ((xdrs->x_v.vio_head + start + datalen) > xdrs->x_v.vio_tail) {
		/* start and datalen reference outside the size of the data
		 * in the buffer.
		 */
		return -1;
	}

	return 1;
}

static bool
xdrmem_fillbufs(XDR *xdrs, u_int start, xdr_vio *vector, u_int datalen)
{
	if ((xdrs->x_v.vio_head + start + datalen) > xdrs->x_v.vio_tail) {
		/* start and datalen reference outside the size of the data
		 * in the buffer.
		 */
		return false;
	}

	vector[0] = xdrs->x_v;
	vector[0].vio_type = VIO_DATA;
	vector[0].vio_length = vector[0].vio_tail - vector[0].vio_head;
	return true;
}

static bool
xdrmem_allochdrs(XDR *xdrs, u_int start, xdr_vio *vector, int iov_count)
{
	int i;
	bool found_data = false;
	uint8_t *current = xdrs->x_data;

	for (i = 0; i < iov_count; i++) {
		/* If we have found_data and we find another VIO_DATA oops...
		 * If we have not found_data and we find a non-VIO_DATA oops...
		 * This simplifies to a single test...
		 */
		if (found_data == (vector[i].vio_type != VIO_DATA)) {
			/* We are being called with a vector we can't support.
			 * Fixup xdrs and leave.
			 */
			xdrs->x_data = current;
			return false;
		}

		if (vector[i].vio_type != VIO_DATA) {
			/* Append a reserved buffer for this */
			uint8_t *future = xdrs->x_data + vector[i].vio_length;

			if (future > xdrs->x_v.vio_wrap) {
				/* Not enough space, fixup xdrs and leave */
				xdrs->x_data = current;
				return false;
			}
			vector[i].vio_base = xdrs->x_v.vio_base;
			vector[i].vio_head = xdrs->x_data;
			vector[i].vio_tail = future;
			vector[i].vio_wrap = xdrs->x_v.vio_wrap;
			xdrs->x_data = future;
		} else {
			found_data = true;
		}
	}

	/* update the most recent data length */
	xdr_tail_update(xdrs);
	return true;
}

static const struct xdr_ops xdrmem_ops_aligned = {
	xdrmem_getunit,
	xdrmem_putunit,
	xdrmem_getbytes,
	xdrmem_putbytes,
	xdrmem_getpos,
	xdrmem_getstartdatapos,
	xdrmem_getenddatapos,
	xdrmem_setpos,
	xdrmem_destroy,
	(dummyfunc3) xdrmem_noop,	/* x_control */
	(dummy_getbufs) xdrmem_noop,	/* x_getbufs */
	xdrmem_putbufs,			/* x_putbufs */
	(dummy_newbuf) xdrmem_noop,	/* x_newbuf */
	xdrmem_iovcount,		/* x_iovcount */
	xdrmem_fillbufs,		/* x_fillbufs */
	xdrmem_allochdrs,		/* x_allochdrs */
};
