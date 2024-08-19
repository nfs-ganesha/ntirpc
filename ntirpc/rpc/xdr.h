/*	$NetBSD: xdr.h,v 1.19 2000/07/17 05:00:45 matt Exp $	*/

/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2010-2018 Red Hat, Inc. and/or its affiliates.
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
 *	from: @(#)xdr.h 1.19 87/04/22 SMI
 *	from: @(#)xdr.h	2.2 88/07/29 4.0 RPCSRC
 * $FreeBSD: src/include/rpc/xdr.h,v 1.23 2003/03/07 13:19:40 nectar Exp $
 */

/*
 * xdr.h, External Data Representation Serialization Routines.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */

#ifndef _TIRPC_XDR_H
#define _TIRPC_XDR_H

#include <sys/cdefs.h>
#include <misc/stdio.h>
#include <stdbool.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#endif
#include <rpc/types.h>
#include <intrinsic.h>

/*
 * XDR provides a conventional way for converting between C data
 * types and an external bit-string representation.  Library supplied
 * routines provide for the conversion on built-in C data types.  These
 * routines and utility routines defined here are used to help implement
 * a type encode/decode routine for each user-defined type.
 *
 * Each data type provides a single procedure which takes two arguments:
 *
 *	bool
 *	xdrproc(xdrs, argresp)
 *		XDR *xdrs;
 *		<type> *argresp;
 *
 * xdrs is an instance of a XDR handle, to which or from which the data
 * type is to be converted.  argresp is a pointer to the structure to be
 * converted.  The XDR handle contains an operation field which indicates
 * which of the operations (ENCODE, DECODE * or FREE) is to be performed.
 *
 * XDR_DECODE may allocate space if the pointer argresp is null.  This
 * data can be freed with the XDR_FREE operation.
 *
 * We write only one procedure per data type to make it easy
 * to keep the encode and decode procedures for a data type consistent.
 * In many cases the same code performs all operations on a user defined type,
 * because all the hard work is done in the component type routines.
 * decode as a series of calls on the nested data types.
 */

/*
 * Xdr operations.  XDR_ENCODE causes the type to be encoded into the
 * stream.  XDR_DECODE causes the type to be extracted from the stream.
 * XDR_FREE can be used to release the space allocated by an XDR_DECODE
 * request.
 */
enum xdr_op {
	XDR_ENCODE = 0,
	XDR_DECODE = 1,
	XDR_FREE = 2
};

/*
 * This is the number of bytes per unit of external data.
 */
#define BYTES_PER_XDR_UNIT (4)

/*
 * constants specific to the xdr "protocol"
 */
#define XDR_FALSE (0)
#define XDR_TRUE (1)

/* Taken verbatim from a system xdr.h, which carries a BSD-style
 * license (Matt) */
/*
 * This only works if the above is a power of 2.  But it's defined to be
 * 4 by the appropriate RFCs.  So it will work.  And it's normally quicker
 * than the old routine.
 */
#if 1
#define RNDUP(x)  (((x) + BYTES_PER_XDR_UNIT - 1) & ~(BYTES_PER_XDR_UNIT - 1))
#else /* this is the old routine */
#define RNDUP(x)  ((((x) + BYTES_PER_XDR_UNIT - 1) / BYTES_PER_XDR_UNIT) \
		   * BYTES_PER_XDR_UNIT)
#endif

/* XDR vector buffer types */
typedef enum vio_type {
	VIO_HEADER,             /* header buffer before data */
	VIO_DATA,               /* data buffer */
	VIO_TRAILER_LEN,	/* length field for following TRAILER buffer */
	VIO_TRAILER,            /* trailer buffer after data */
} vio_type;

/* XDR buffer vector descriptors */
typedef struct xdr_vio {
	uint8_t *vio_base;
	uint8_t *vio_head;	/* minimum vio_tail (header offset) */
	uint8_t *vio_tail;	/* end of the used part of the buffer */
	uint8_t *vio_wrap;	/* maximum vio_tail */
	uint32_t vio_length;	/* length of buffer, used for vector
				   pre-allocation */
	vio_type vio_type;	/* type of buffer */   
} xdr_vio;

/* vio_wrap >= vio_tail >= vio_head >= vio_base */

#define UIO_FLAG_NONE		0x0000
#define UIO_FLAG_BUFQ		0x0001
#define UIO_FLAG_FREE		0x0002
#define UIO_FLAG_GIFT		0x0004
#define UIO_FLAG_MORE		0x0008
#define UIO_FLAG_REALLOC	0x0010
#define UIO_FLAG_REFER		0x0020

struct xdr_uio;
typedef void (*xdr_uio_release)(struct xdr_uio *, u_int);

typedef struct xdr_uio {
	struct xdr_uio	*uio_refer;
	xdr_uio_release uio_release;
	void	*uio_p1;
	void	*uio_p2;
	void	*uio_u1;
	void	*uio_u2;
	void	*uio_u3;

	size_t	uio_count;	/* count of entries in vio array,
				 * 0: not allocated */
	u_int	uio_flags;
	int32_t uio_references;
	xdr_vio	uio_vio[0];	/* appended vectors */
} xdr_uio;

/* Op flags */
#define XDR_PUTBUFS_FLAG_NONE    0x0000
#define XDR_PUTBUFS_FLAG_RDNLY   0x0001

#define XDR_FLAG_NONE		0x0000
#define XDR_FLAG_CKSUM		0x0001
#define XDR_FLAG_FREE		0x0002
#define XDR_FLAG_VIO		0x0004

/*
 * The XDR handle.
 * Contains operation which is being applied to the stream,
 * an operations vector for the particular implementation (e.g. see xdr_mem.c),
 * and two private fields for the use of the particular implementation.
 * XXX: w/64-bit pointers, u_int not enough!
 */
typedef struct rpc_xdr {
	const struct xdr_ops {
		/* get 4 unsigned bytes from underlying stream */
		bool (*x_getunit)(struct rpc_xdr *, uint32_t *);
		/* put 4 unsigned bytes to underlying stream */
		bool (*x_putunit)(struct rpc_xdr *, const uint32_t);
		/* get some bytes from " */
		bool (*x_getbytes)(struct rpc_xdr *, char *, u_int);
		/* put some bytes to " */
		bool (*x_putbytes)(struct rpc_xdr *, const char *, u_int);
		/* returns bytes off from beginning */
		u_int (*x_getpostn)(struct rpc_xdr *);
		/* return bytes off from beginning for start of data */
		u_int (*x_getstartdatapos)(struct rpc_xdr *, u_int, u_int);
		/* return bytes off from beginning for end of data */
		u_int (*x_getenddatapos)(struct rpc_xdr *, u_int, u_int);
		/* lets you reposition the stream */
		bool (*x_setpostn)(struct rpc_xdr *, u_int);
		/* free private resources of this xdr_stream */
		void (*x_destroy)(struct rpc_xdr *);
		bool (*x_control)(struct rpc_xdr *, int, void *);
		/* new vector and refcounted interfaces */
		bool (*x_getbufs)(struct rpc_xdr *, xdr_uio *, u_int);
		bool (*x_putbufs)(struct rpc_xdr *, xdr_uio *, u_int);
		/* Force a new buffer to start (or fail) */
		bool (*x_newbuf)(struct rpc_xdr *);
		/* Return the count of buffers in the vector from pos */
		int (*x_iovcount)(struct rpc_xdr *, u_int, u_int);
		/* Fill xdr_vio with buffers from pos */
		bool (*x_fillbufs)(struct rpc_xdr *, u_int , xdr_vio *, u_int);
		/* Allocate bufs for headers and trailers and insert into vio */
		bool (*x_allochdrs)(struct rpc_xdr *, u_int , xdr_vio *, int);
	} *x_ops;
	void *x_public; /* users' data */
	void *x_private; /* pointer to private data */
	void *x_lib[2]; /* RPC library private */
	uint8_t *x_data;  /* private used for position inline */
	void *x_base;  /* private used for position info */
	struct xdr_vio x_v; /* private buffer vector */
	u_int x_handy; /* extra private word */
	u_int x_flags; /* shared flags */
	enum xdr_op x_op;  /* operation; fast additional param */
} XDR;

#define XDR_VIO(x) ((xdr_vio *)((x)->x_base))

static inline size_t
xdr_size_inline(XDR *xdrs)
{
	return ((uintptr_t)xdrs->x_v.vio_wrap - (uintptr_t)xdrs->x_data);
}

static inline size_t
xdr_tail_inline(XDR *xdrs)
{
	return ((uintptr_t)xdrs->x_v.vio_tail - (uintptr_t)xdrs->x_data);
}

static inline void
xdr_tail_update(XDR *xdrs)
{
	if ((uintptr_t)xdrs->x_v.vio_tail < (uintptr_t)xdrs->x_data) {
		xdrs->x_v.vio_tail = xdrs->x_data;
		XDR_VIO(xdrs)->vio_tail = xdrs->x_data;
	}
}

/*
 * A xdrproc_t exists for each data type which is to be encoded or decoded.
 *
 * The second argument to the xdrproc_t is a pointer to an opaque pointer.
 * The opaque pointer generally points to a structure of the data type
 * to be decoded.  If this pointer is 0, then the type routines should
 * allocate dynamic storage of the appropriate size and return it.
 */
typedef bool(*xdrproc_t) (XDR *, void *);

/*
 * Operations defined on a XDR handle
 *
 * XDR  *xdrs;
 * long  *longp;
 * char *  addr;
 * u_int  len;
 * u_int  pos;
 */

#define char_ptr(x) ((char*)(x))

static inline bool
xdr_getlong(XDR *xdrs, long *lp)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);
	uint32_t u;
	bool b;

	if (future <= xdrs->x_v.vio_tail) {
		*lp = (long)ntohl(*((uint32_t *) (xdrs->x_data)));
		xdrs->x_data = future;
		return (true);
	}

	b = (*xdrs->x_ops->x_getunit)(xdrs, &u);
	if (b) {
		*lp = (int32_t)u;	/* sign extends */
	}
	return b;
}

static inline bool
xdr_putlong(XDR *xdrs, const long *lp)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	if (future <= xdrs->x_v.vio_wrap) {
		*((int32_t *) (xdrs->x_data)) =
			(int32_t) htonl((int32_t) (*lp));
		xdrs->x_data = future;
		return (true);
	}
	return (*xdrs->x_ops->x_putunit)(xdrs, (uint32_t) (*lp));
}

#define XDR_GETLONG(xdrs, lp) xdr_getlong(xdrs, lp)
#define XDR_PUTLONG(xdrs, lp) xdr_putlong(xdrs, lp)

#define XDR_GETBYTES(xdrs, addr, len)   \
	(*(xdrs)->x_ops->x_getbytes)(xdrs, addr, len)
#define xdr_getbytes(xdrs, addr, len)			\
	(*(xdrs)->x_ops->x_getbytes)(xdrs, addr, len)

#define XDR_PUTBYTES(xdrs, addr, len)			\
	(*(xdrs)->x_ops->x_putbytes)(xdrs, addr, len)
#define xdr_putbytes(xdrs, addr, len)			\
	(*(xdrs)->x_ops->x_putbytes)(xdrs, addr, len)

#define XDR_GETBUFS(xdrs, uio, len, flags)		\
	(*(xdrs)->x_ops->x_getbufs)(xdrs, uio, len, flags)
#define xdr_getbufs(xdrs, uio, len, flags)		\
	(*(xdrs)->x_ops->x_getbufs)(xdrs, uio, len, flags)

#define XDR_PUTBUFS(xdrs, uio, flags)			\
	(*(xdrs)->x_ops->x_putbufs)(xdrs, uio, flags)
#define xdr_putbufs(xdrs, uio, flags)			\
	(*(xdrs)->x_ops->x_putbufs)(xdrs, uio, flags)

#define XDR_GETPOS(xdrs)			\
	(*(xdrs)->x_ops->x_getpostn)(xdrs)
#define xdr_getpos(xdrs)			\
	(*(xdrs)->x_ops->x_getpostn)(xdrs)

#define XDR_GETSTARTDATAPOS(xdrs, start, datalen)	\
	(*(xdrs)->x_ops->x_getstartdatapos)(xdrs, start, datalen)
#define xdr_getstartdatapos(xdrs, start, datalen)	\
	(*(xdrs)->x_ops->x_getstartdatapos)(xdrs, start, datalen)

#define XDR_GETENDDATAPOS(xdrs, start, datalen)		\
	(*(xdrs)->x_ops->x_getenddatapos)(xdrs, start, datalen)
#define xdr_getenddatapos(xdrs, start, datalen)		\
	(*(xdrs)->x_ops->x_getenddatapos)(xdrs, start, datalen)

#define XDR_SETPOS(xdrs, pos)			\
	(*(xdrs)->x_ops->x_setpostn)(xdrs, pos)
#define xdr_setpos(xdrs, pos)			\
	(*(xdrs)->x_ops->x_setpostn)(xdrs, pos)

#define XDR_DESTROY(xdrs)	     \
	if ((xdrs)->x_ops->x_destroy)			\
		(*(xdrs)->x_ops->x_destroy)(xdrs)
#define xdr_destroy(xdrs)	     \
	if ((xdrs)->x_ops->x_destroy)			\
		(*(xdrs)->x_ops->x_destroy)(xdrs)

#define XDR_CONTROL(xdrs, req, op)				\
	if ((xdrs)->x_ops->x_control)				\
		(*(xdrs)->x_ops->x_control)(xdrs, req, op)
#define xdr_control(xdrs, req, op) XDR_CONTROL(xdrs, req, op)

#define XDR_NEWBUF(xdrs)	\
	(*(xdrs)->x_ops->x_newbuf)(xdrs)
#define xdr_newbuf(xdrs, pos) XDR_NEWBUF(xdrs)

#define XDR_IOVCOUNT(xdrs, pos, len) \
	(*(xdrs)->x_ops->x_iovcount)(xdrs, pos, len)
#define xdr_iovcount(xdrs, pos, len) XDR_IOVCOUNT(xdrs, pos, len)

#define XDR_FILLBUFS(xdrs, pos, iov, len)	\
	(*(xdrs)->x_ops->x_fillbufs)(xdrs, pos, iov, len)
#define xdr_fillbufs(xdrs, pos, iov, len) XDR_FILLBUFS(xdrs, pos, iov, len)

#define XDR_ALLOCHDRS(xdrs, pos, iov, iov_count)	\
	(*(xdrs)->x_ops->x_allochdrs)(xdrs, pos, iov, iov_count)
#define xdr_allochdrs(xdrs, pos, iov, iov_count) \
	XDR_ALLOCHDRS(xdrs, pos, iov, iov_count)

/*
 * Support struct for discriminated unions.
 * You create an array of xdrdiscrim structures, terminated with
 * an entry with a null procedure pointer.  The xdr_union routine gets
 * the discriminant value and then searches the array of structures
 * for a matching value.  If a match is found the associated xdr routine
 * is called to handle that part of the union.  If there is
 * no match, then a default routine may be called.
 * If there is no match and no default routine it is an error.
 */
#define NULL_xdrproc_t ((xdrproc_t)0)
struct xdr_discrim {
	int value;
	xdrproc_t proc;
};

/*
 * In-line routines for fast encode/decode of primitive data types.
 * Caveat emptor: these use single memory cycles to get the
 * data from the underlying buffer, and will fail to operate
 * properly where the data is not aligned.  The standard way to use
 * these is to say:
 *      if ((buf = xdr_inline_decode(xdrs, count)) == NULL)
 *              return (FALSE);
 *      <<< IXDR_GET_* macro calls >>>
 *      if ((buf = xdr_inline_encode(xdrs, count)) == NULL)
 *              return (FALSE);
 *      <<< IXDR_PUT_* macro calls >>>
 * where ``count'' is the number of bytes of data occupied
 * by the primitive data types.
 *
 * N.B. and frozen for all time: each data type here uses 4 bytes
 * of external representation.
 */
static inline int32_t *
xdr_inline_decode(XDR *xdrs, size_t count)
{
	int32_t *buf = (int32_t *)xdrs->x_data;
	uint8_t *future = xdrs->x_data + count;

	/* re-consuming bytes in a stream
	 * (after SETPOS/rewind) */
	if (future <= xdrs->x_v.vio_tail) {
		xdrs->x_data = future;
		return (buf);
	}
	return (NULL);
}

static inline int32_t *
xdr_inline_encode(XDR *xdrs, size_t count)
{
	int32_t *buf = (int32_t *)xdrs->x_data;
	uint8_t *future = xdrs->x_data + count;

	if (future <= xdrs->x_v.vio_wrap) {
		xdrs->x_data = future;
		xdr_tail_update(xdrs);
		return (buf);
	}
	return (NULL);
}

#define IXDR_GET_INT32(buf)  ((int32_t)ntohl((u_int32_t)*(buf)++))
#define IXDR_PUT_INT32(buf, v)  (*(buf)++ = (int32_t)htonl((u_int32_t)v))
#define IXDR_GET_U_INT32(buf)  ((u_int32_t)IXDR_GET_INT32(buf))
#define IXDR_PUT_U_INT32(buf, v) IXDR_PUT_INT32((buf), ((int32_t)(v)))

#define IXDR_GET_LONG(buf)  ((long)ntohl((u_int32_t)*(buf)++))
#define IXDR_PUT_LONG(buf, v)  (*(buf)++ = (int32_t)htonl((u_int32_t)v))

#define IXDR_GET_BOOL(buf)  ((bool)IXDR_GET_LONG(buf))
#define IXDR_GET_ENUM(buf, t)  ((t)IXDR_GET_LONG(buf))
#define IXDR_GET_U_LONG(buf)  ((u_long)IXDR_GET_LONG(buf))

#define IXDR_PUT_BOOL(buf, v)  IXDR_PUT_LONG((buf), (v))
#define IXDR_PUT_ENUM(buf, v)  IXDR_PUT_LONG((buf), (v))
#define IXDR_PUT_U_LONG(buf, v)  IXDR_PUT_LONG((buf), (v))

/*
 * In-line routines for vector encode/decode of primitive data types.
 * Intermediate speed, avoids function calls in most cases, at the expense of
 * checking the remaining space available for each item.
 *
 * Caveat emptor: these use single memory cycles to get the
 * data from the underlying buffer, and will fail to operate
 * properly where the data is not aligned.
 *
 * if (!FUNCTION(xdrs, &variable)) {
 *  print(warning);
 *  return (false);
 * }
 *
 * N.B. and frozen for all time: each data type here uses 4 bytes
 * of external representation.
 */

static inline bool
xdr_getuint32(XDR *xdrs, uint32_t *ip)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	if (future <= xdrs->x_v.vio_tail) {
		*ip = ntohl(*((uint32_t *) (xdrs->x_data)));
		xdrs->x_data = future;
		return (true);
	}
	return (*xdrs->x_ops->x_getunit)(xdrs, ip);
}

static inline bool
xdr_putuint32(XDR *xdrs, uint32_t v)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	if (future <= xdrs->x_v.vio_wrap) {
		*((int32_t *) (xdrs->x_data)) = htonl(v);
		xdrs->x_data = future;
		return (true);
	}
	return (*xdrs->x_ops->x_putunit)(xdrs, v);
}

#define XDR_GETUINT32(xdrs, uint32p) xdr_getuint32(xdrs, uint32p)
#define XDR_PUTUINT32(xdrs, uint32v) xdr_putuint32(xdrs, uint32v)

static inline bool
xdr_getint32(XDR *xdrs, int32_t *ip)
{
	return xdr_getuint32(xdrs, (uint32_t *)ip);
}

static inline bool
xdr_putint32(XDR *xdrs, int32_t v)
{
	return xdr_putuint32(xdrs, (uint32_t)v);
}

#define XDR_GETINT32(xdrs, int32p) xdr_getint32(xdrs, int32p)
#define XDR_PUTINT32(xdrs, int32v) xdr_putint32(xdrs, int32v)

static inline bool
xdr_getuint16(XDR *xdrs, uint16_t *ip)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);
	uint32_t u;

	if (future <= xdrs->x_v.vio_tail) {
		*ip = (uint16_t)ntohl(*((uint32_t *) (xdrs->x_data)));
		xdrs->x_data = future;
		return (true);
	}
	if ((*xdrs->x_ops->x_getunit)(xdrs, &u)) {
		*ip = (uint16_t) u;
		return (true);
	}
	return (false);
}

static inline bool
xdr_putuint16(XDR *xdrs, uint32_t uint16v)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	if (future <= xdrs->x_v.vio_wrap) {
		*((int32_t *) (xdrs->x_data)) = htonl(uint16v);
		xdrs->x_data = future;
		return (true);
	}
	return (*xdrs->x_ops->x_putunit)(xdrs, (uint32_t)uint16v);
}

#define XDR_GETUINT16(xdrs, uint16p) xdr_getuint16(xdrs, uint16p)
#define XDR_PUTUINT16(xdrs, uint16v) xdr_putuint16(xdrs, uint16v)

static inline bool
xdr_getint16(XDR *xdrs, int16_t *ip)
{
	return xdr_getuint16(xdrs, (uint16_t *)ip);
}

/* extend sign before storage */
static inline bool
xdr_putint16(XDR *xdrs, int32_t int16v)
{
	return xdr_putuint16(xdrs, (uint16_t)int16v);
}

#define XDR_GETINT16(xdrs, int16p) xdr_getint16(xdrs, int16p)
#define XDR_PUTINT16(xdrs, int16v) xdr_putint16(xdrs, int16v)

static inline bool
xdr_getuint8(XDR *xdrs, uint8_t *ip)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);
	uint32_t u;

	if (future <= xdrs->x_v.vio_tail) {
		*ip = (uint8_t)ntohl(*((uint32_t *) (xdrs->x_data)));
		xdrs->x_data = future;
		return (true);
	}
	if ((*xdrs->x_ops->x_getunit)(xdrs, &u)) {
		*ip = (uint8_t) u;
		return (true);
	}
	return (false);
}

static inline bool
xdr_putuint8(XDR *xdrs, uint32_t uint8v)
{
	uint8_t *future = xdrs->x_data + sizeof(uint32_t);

	if (future <= xdrs->x_v.vio_wrap) {
		*((int32_t *) (xdrs->x_data)) = htonl(uint8v);
		xdrs->x_data = future;
		return (true);
	}
	return (*xdrs->x_ops->x_putunit)(xdrs, (uint32_t)uint8v);
}

#define XDR_GETUINT8(xdrs, uint8p) xdr_getuint8(xdrs, uint8p)
#define XDR_PUTUINT8(xdrs, uint8v) xdr_putuint8(xdrs, uint8v)

static inline bool
xdr_getint8(XDR *xdrs, int8_t *ip)
{
	return xdr_getuint8(xdrs, (uint8_t *)ip);
}

/* extend sign before storage */
static inline bool
xdr_putint8(XDR *xdrs, int32_t int8v)
{
	return xdr_putuint8(xdrs, (uint8_t)int8v);
}

#define XDR_GETINT8(xdrs, int8p) xdr_getint8(xdrs, int8p)
#define XDR_PUTINT8(xdrs, int8v) xdr_putint8(xdrs, int8v)

static inline bool
xdr_getenum(XDR *xdrs, enum_t *ip)
{
	return xdr_getint32(xdrs, (int32_t *)ip);
}

static inline bool
xdr_putenum(XDR *xdrs, enum_t enumv)
{
	return xdr_putint32(xdrs, (int32_t)enumv);
}

#define XDR_GETENUM(xdrs, enump) xdr_getenum(xdrs, enump)
#define XDR_PUTENUM(xdrs, enumv) xdr_putenum(xdrs, enumv)

static inline bool
xdr_getbool(XDR *xdrs, bool_t *ip)
{
	int32_t lv;

	if (!xdr_getint32(xdrs, &lv))
		return (false);
	*ip = lv ? XDR_TRUE : XDR_FALSE;
	return (true);
}

static inline bool
xdr_putbool(XDR *xdrs, bool_t boolv)
{
	return xdr_putint32(xdrs, boolv ? XDR_TRUE : XDR_FALSE);
}

#define XDR_GETBOOL(xdrs, boolp) xdr_getbool(xdrs, boolp)
#define XDR_PUTBOOL(xdrs, boolv) xdr_putbool(xdrs, boolv)

/*
 * These are the "generic" xdr routines.
 */
__BEGIN_DECLS
extern XDR xdr_free_null_stream;

extern bool xdr_void(XDR *, void *);
extern bool xdr_int(XDR *, int *);
extern bool xdr_u_int(XDR *, u_int *);
extern bool xdr_long(XDR *, long *);
extern bool xdr_u_long(XDR *, u_long *);
extern bool xdr_float(XDR *, float *);
extern bool xdr_double(XDR *, double *);
extern bool xdr_reference(XDR *, void **, u_int, xdrproc_t);
extern bool xdr_pointer(XDR *, void **, u_int, xdrproc_t);
extern bool xdr_wrapstring(XDR *, char **);
extern bool xdr_longlong_t(XDR *, quad_t *);
extern bool xdr_u_longlong_t(XDR *, u_quad_t *);

__END_DECLS

/*
 * Free a data structure using XDR
 * Not a filter, but a convenient utility nonetheless
 */
static inline bool
xdr_nfree(xdrproc_t proc, void *objp)
{
	return (*proc) (&xdr_free_null_stream, objp);
}

/*
 * Common opaque bytes objects used by many rpc protocols;
 * declared here due to commonality.
 */
#define MAX_NETOBJ_SZ 1024
struct netobj {
	u_int n_len;
	char *n_bytes;
};
typedef struct netobj netobj;
extern bool xdr_nnetobj(XDR *, struct netobj *);

/*
 * These are the public routines for the various implementations of
 * xdr streams.
 */
__BEGIN_DECLS
/* XDR using memory buffers */
extern void xdrmem_ncreate(XDR *, char *, u_int, enum xdr_op);

/* intrinsic checksum (be careful) */
extern uint64_t xdrmem_cksum(XDR *, u_int);

__END_DECLS
/* For backward compatibility */
#include <rpc/tirpc_compat.h>
#endif				/* !_TIRPC_XDR_H */
