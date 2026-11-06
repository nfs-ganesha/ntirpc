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

#include <sys/cdefs.h>

/*
 * xdr.c, Generic XDR routines implementation.
 *
 * Copyright (C) 1986, Sun Microsystems, Inc.
 *
 * These are the "generic" xdr routines used to serialize and de-serialize
 * most common data items.  See xdr.h for more info on the interface to
 * xdr.
 */

#ifndef _TIRPC_INLINE_XDR_H
#define _TIRPC_INLINE_XDR_H

#if !defined(_WIN32)
#include <err.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <rpc/xdr.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UNUSED
#define UNUSED_ATTR __attribute__((unused))
#define UNUSED(...) UNUSED_(__VA_ARGS__)
#define UNUSED_(arg) NOT_USED_##arg UNUSED_ATTR
#endif


/*
 * XDR integers
 */
static inline bool
inline_xdr_int(XDR *xdrs, int *ip)
{
	long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (long)*ip;
		return (XDR_PUTLONG(xdrs, &l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &l))
			return (false);
		*ip = (int)l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned integers
 */
static inline bool
inline_xdr_u_int(XDR *xdrs, u_int *up)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *up;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*up = (u_int) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR long integers
 * same as xdr_u_long - open coded to save a proc call!
 */
static inline bool
inline_xdr_long(XDR *xdrs, long *lp)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (XDR_PUTLONG(xdrs, lp));
	case XDR_DECODE:
		return (XDR_GETLONG(xdrs, lp));
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned long integers
 * same as xdr_long - open coded to save a proc call!
 */
static inline bool
inline_xdr_u_long(XDR *xdrs, u_long *ulp)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (XDR_PUTLONG(xdrs, (long *)ulp));
	case XDR_DECODE:
		return (XDR_GETLONG(xdrs, (long *)ulp));
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR 64-bit integers
 * (always transmitted as unsigned 32-bit)
 */
static inline bool
xdr_uint64_t(XDR *xdrs, uint64_t *uint64_p)
{
	uint32_t u[2];

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		u[0] = (uint32_t) (*uint64_p >> 32) & 0xffffffff;
		u[1] = (uint32_t) (*uint64_p) & 0xffffffff;
		if (!XDR_PUTUINT32(xdrs, u[0]))
			return (false);
		return (XDR_PUTUINT32(xdrs, u[1]));
	case XDR_DECODE:
		if (!XDR_GETUINT32(xdrs, &u[0])
		 || !XDR_GETUINT32(xdrs, &u[1]))
			return (false);
		*uint64_p = (((uint64_t) u[0] << 32) | ((uint64_t) u[1]));
		return (true);
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

static inline bool
xdr_u_int64_t(XDR *xdrs, u_int64_t *u_int64_p)
{
	return (xdr_uint64_t(xdrs, (uint64_t *)u_int64_p));
}
#define inline_xdr_u_int64_t xdr_u_int64_t

static inline bool
xdr_int64_t(XDR *xdrs, int64_t *int64_p)
{
	return (xdr_uint64_t(xdrs, (uint64_t *)int64_p));
}
#define inline_xdr_int64_t xdr_int64_t

/*
 * XDR 32-bit integers
 */
static inline bool
xdr_int32_t(XDR *xdrs, int32_t *int32_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTINT32(xdrs, *int32_p));

	case XDR_DECODE:
		return (XDR_GETINT32(xdrs, int32_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}
#define inline_xdr_int32_t xdr_int32_t

/*
 * XDR unsigned 32-bit integers
 */
static inline bool
xdr_uint32_t(XDR *xdrs, uint32_t *u_int32_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTUINT32(xdrs, *u_int32_p));

	case XDR_DECODE:
		return (XDR_GETUINT32(xdrs, u_int32_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

static inline bool
xdr_u_int32_t(XDR *xdrs, u_int32_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}
#define inline_xdr_u_int32_t xdr_u_int32_t

/*
 * Solaris strips the '_t' from these types -- not sure why.
 * But, let's be compatible.
 */
static inline bool
xdr_rpcprog(XDR *xdrs, rpcprog_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}

static inline bool
xdr_rpcvers(XDR *xdrs, rpcvers_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}

static inline bool
xdr_rpcproc(XDR *xdrs, rpcproc_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}

static inline bool
xdr_rpcprot(XDR *xdrs, rpcprot_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}

static inline bool
xdr_rpcport(XDR *xdrs, rpcport_t *u_int32_p)
{
	return (xdr_uint32_t(xdrs, (uint32_t *)u_int32_p));
}

/*
 * XDR 16-bit integers
 */
static inline bool
xdr_int16_t(XDR *xdrs, int16_t *int16_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTINT16(xdrs, *int16_p));

	case XDR_DECODE:
		return (XDR_GETINT16(xdrs, int16_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}
#define inline_xdr_int16_t xdr_int16_t

/*
 * XDR unsigned 16-bit integers
 */
static inline bool
xdr_uint16_t(XDR *xdrs, uint16_t *u_int16_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTUINT16(xdrs, *u_int16_p));

	case XDR_DECODE:
		return (XDR_GETUINT16(xdrs, u_int16_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

static inline bool
xdr_u_int16_t(XDR *xdrs, u_int16_t *u_int16_p)
{
	return (xdr_uint16_t(xdrs, (uint16_t *)u_int16_p));
}
#define inline_xdr_u_int16_t xdr_u_int16_t

/*
 * XDR 8-bit integers
 */
static inline bool
xdr_int8_t(XDR *xdrs, int8_t *int8_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTINT8(xdrs, *int8_p));

	case XDR_DECODE:
		return (XDR_GETINT8(xdrs, int8_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}
#define inline_xdr_int8_t xdr_int8_t

/*
 * XDR unsigned 8-bit integers
 */
static inline bool
xdr_uint8_t(XDR *xdrs, uint8_t *u_int8_p)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (XDR_PUTUINT8(xdrs, *u_int8_p));

	case XDR_DECODE:
		return (XDR_GETUINT8(xdrs, u_int8_p));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

static inline bool
xdr_u_int8_t(XDR *xdrs, u_int8_t *u_int8_p)
{
	return (xdr_uint8_t(xdrs, (uint8_t *)u_int8_p));
}
#define inline_xdr_u_int8_t xdr_u_int8_t

/*
 * XDR booleans
 */
static inline bool
xdr_bool(XDR *xdrs, bool_t *bp)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (xdr_putbool(xdrs, *bp));

	case XDR_DECODE:
		return (xdr_getbool(xdrs, bp));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}
#define inline_xdr_bool xdr_bool

/*
 * XDR enumerations
 */
static inline bool
xdr_enum(XDR *xdrs, enum_t *ep)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (xdr_putenum(xdrs, *ep));

	case XDR_DECODE:
		return (xdr_getenum(xdrs, ep));

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}
#define inline_xdr_enum xdr_enum

/*
 * decode opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
xdr_opaque_decode(XDR *xdrs, char *cp, u_int cnt)
{
	u_int rndup;

	/*
	 * if no data we are done
	 */
	if (cnt == 0)
		return (true);

	/*
	 * XDR_INLINE is just as likely to do a function call,
	 * so don't bother with it here.
	 */
	if (!XDR_GETBYTES(xdrs, cp, cnt)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR opaque",
			__func__, __LINE__);
		return (false);
	}

	/*
	 * round byte count to full xdr units
	 */
	rndup = cnt & (BYTES_PER_XDR_UNIT - 1);

	if (rndup > 0) {
		uint32_t crud;

		if (!XDR_GETBYTES(xdrs, (char *) &crud,
				  BYTES_PER_XDR_UNIT - rndup)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR crud",
				__func__, __LINE__);
			return (false);
		}
	}

	return (true);
}

/*
 * encode opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
xdr_opaque_encode(XDR *xdrs, const char *cp, u_int cnt)
{
	u_int rndup;

	/*
	 * if no data we are done
	 */
	if (cnt == 0)
		return (true);

	/*
	 * XDR_INLINE is just as likely to do a function call,
	 * so don't bother with it here.
	 */
	if (!XDR_PUTBYTES(xdrs, cp, cnt))
		return (false);

	/*
	 * round byte count to full xdr units
	 */
	rndup = cnt & (BYTES_PER_XDR_UNIT - 1);

	if (rndup > 0) {
		uint32_t zero = 0;

		if (!XDR_PUTBYTES(xdrs, (char *) &zero,
				  BYTES_PER_XDR_UNIT - rndup))
			return (false);
	}

	return (true);
}

/*
 * XDR opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
xdr_opaque(XDR *xdrs, char *cp, u_int cnt)
{
	switch (xdrs->x_op) {
	case XDR_DECODE:
		return (xdr_opaque_decode(xdrs, cp, cnt));
	case XDR_ENCODE:
		return (xdr_opaque_encode(xdrs, cp, cnt));
	case XDR_FREE:
		return (true);
	}

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s:%u ERROR xdrs->x_op (%u)",
		__func__, __LINE__,
		xdrs->x_op);
	return (false);
}

/*
 * XDR counted opaques
 * *sp is a pointer to the bytes, *sizep is the count.
 */
static inline bool
xdr_opaques(XDR *xdrs, char *sp, uint32_t *sizep, uint32_t maxsize)
{
	uint32_t nodesize;

	/*
	 * first deal with the length since xdr bytes are counted
	 */
	if (!xdr_uint32_t(xdrs, sizep))
		return (false);

	nodesize = *sizep;
	if ((nodesize > maxsize) && (xdrs->x_op != XDR_FREE))
		return (false);

	/*
	 * now deal with the actual bytes
	 */
	switch (xdrs->x_op) {
	case XDR_DECODE:
		return (xdr_opaque_decode(xdrs, sp, nodesize));
	case XDR_ENCODE:
		return (xdr_opaque_encode(xdrs, sp, nodesize));
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR counted bytes
 * *cpp is a pointer to the bytes, *sizep is the count.
 * If *cpp is NULL maxsize bytes are allocated
 */
static inline bool
xdr_bytes_decode(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize)
{
	char *sp = *cpp;	/* sp is the actual string pointer */
	uint32_t size;
	bool ret;

	/*
	 * first deal with the length since xdr bytes are counted
	 */
	if (!XDR_GETUINT32(xdrs, &size)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size",
			__func__, __LINE__);
		return (false);
	}
	if (size > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %" PRIu32 " > max %u",
			__func__, __LINE__,
			size, maxsize);
		return (false);
	}
	*sizep = (u_int)size;		/* only valid size */

	/*
	 * now deal with the actual bytes
	 */
	if (!size)
		return (true);
	if (!sp)
		sp = (char *)mem_alloc(size);

	ret = xdr_opaque_decode(xdrs, sp, size);
	if (!ret) {
		if (!*cpp) {
			/* Only free if we allocated */
			mem_free(sp, size);
		}
		return (ret);
	}
	*cpp = sp;			/* only valid pointer */
	return (ret);
}

static inline bool
xdr_bytes_encode(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize)
{
	char *sp = *cpp;	/* sp is the actual string pointer */
	uint32_t size = (uint32_t)*sizep;

	if (*sizep > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %u > max %u",
			__func__, __LINE__,
			*sizep, maxsize);
		return (false);
	}

	if (*sizep > size) {
		/* caller provided very large maxsize */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR overflow %u",
			__func__, __LINE__,
			*sizep);
		return (false);
	}

	if (!XDR_PUTUINT32(xdrs, size))
		return (false);

	return (xdr_opaque_encode(xdrs, sp, size));
}

static inline bool
xdr_bytes_free(XDR *UNUSED(xdrs), char **cpp, size_t size)
{
	if (*cpp) {
		mem_free(*cpp, size);
		*cpp = NULL;
		return (true);
	}

	/* normal for switch x_op, sometimes useful to track */
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s:%u already free",
		__func__, __LINE__);
	return (true);
}

static inline bool
xdr_bytes(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize)
{
	switch (xdrs->x_op) {
	case XDR_DECODE:
		return (xdr_bytes_decode(xdrs, cpp, sizep, maxsize));
	case XDR_ENCODE:
		return (xdr_bytes_encode(xdrs, cpp, sizep, maxsize));
	case XDR_FREE:
		return (xdr_bytes_free(xdrs, cpp, *sizep));
	}

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s:%u ERROR xdrs->x_op (%u)",
		__func__, __LINE__,
		xdrs->x_op);
	return (false);
}
#define inline_xdr_bytes xdr_bytes

/*
 * XDR a descriminated union
 * Support routine for discriminated unions.
 * You create an array of xdrdiscrim structures, terminated with
 * an entry with a null procedure pointer.  The routine gets
 * the discriminant value and then searches the array of xdrdiscrims
 * looking for that value.  It calls the procedure given in the xdrdiscrim
 * to handle the discriminant.  If there is no specific routine a default
 * routine may be called.
 * If there is no specific or default routine an error is returned.
 */
static inline bool
xdr_union(XDR *xdrs,
	  enum_t *dscmp,	/* enum to decide which arm to work on */
	  void *unp,		/* the union itself */
	  const struct xdr_discrim *choices,
				/* [value, xdr proc] for each arm */
	  xdrproc_t dfault /* default xdr routine */)
{
	enum_t dscm;

	/*
	 * we deal with the discriminator;  it's an enum
	 */
	if (!inline_xdr_enum(xdrs, dscmp))
		return (false);
	dscm = *dscmp;

	/*
	 * search choices for a value that matches the discriminator.
	 * if we find one, execute the xdr routine for that value.
	 */
	for (; choices->proc != NULL_xdrproc_t; choices++) {
		if (choices->value == dscm)
			return ((*(choices->proc)) (xdrs, unp));
	}

	/*
	 * no match - execute the default xdr routine if there is one
	 */
	return ((dfault == NULL_xdrproc_t) ? false : (*dfault) (xdrs, unp));
}
#define inline_xdr_union xdr_union

/*
 * XDR a fixed length array. Unlike variable-length arrays,
 * the storage of fixed length arrays is static and unfreeable.
 * > basep: pointer of the array
 * > nelem: number of elements
 * > selem: size (in bytes) of each element
 * > xdr_elem: routine to XDR each element
 */
static inline bool
xdr_vector(XDR *xdrs, char *basep, u_int nelem, u_int selem,
	   xdrproc_t xdr_elem)
{
	char *target = basep;
	u_int i = 0;

	for (; i < nelem; i++) {
		if (!(*xdr_elem) (xdrs, target))
			return (false);
		target += selem;
	}
	return (true);
}

/*
 * XDR an array of arbitrary elements
 * > **cpp: pointer to the array
 * > *sizep: number of elements
 * > maxsize: maximum number of elements
 * > selem: size (in bytes) of each element
 * > xdr_elem: routine to XDR each element
 *
 * If *cpp is NULL, (*sizep * selem) bytes are allocated.
 */
static inline bool
xdr_array_decode(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize,
		 u_int selem, xdrproc_t xdr_elem)
{
	char *target = *cpp;
	u_int i = 0;
	uint32_t size;
	bool stat = true;

	if (maxsize > (UINT_MAX / selem)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR maxsize %u > max %u",
			__func__, __LINE__,
			maxsize, (UINT_MAX / selem));
		return (false);
	}

	/*
	 * first deal with the length since xdr arrays are counted-arrays
	 */
	if (!XDR_GETUINT32(xdrs, &size)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size",
			__func__, __LINE__);
		return (false);
	}
	if (size > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %" PRIu32 " > max %u",
			__func__, __LINE__,
			size, maxsize);
		return (false);
	}
	*sizep = (u_int)size;		/* only valid size */

	/*
	 * now deal with the actual elements
	 */
	if (!size)
		return (true);
	if (!target)
		*cpp = target = (char*) mem_zalloc(size * selem);

	for (; (i < size) && stat; i++) {
		stat = (*xdr_elem) (xdrs, target);
		target += selem;
	}

	return (stat);
}

static inline bool
xdr_array_encode(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize,
		 u_int selem, xdrproc_t xdr_elem)
{
	char *target = *cpp;
	u_int i = 0;
	uint32_t size = (uint32_t)*sizep;
	bool stat = true;

	if (*sizep > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %u > max %u",
			__func__, __LINE__,
			*sizep, maxsize);
		return (false);
	}

	if (*sizep > size) {
		/* caller provided very large maxsize */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR overflow %u",
			__func__, __LINE__,
			*sizep);
		return (false);
	}

	if (!XDR_PUTUINT32(xdrs, size))
		return (false);

	for (; (i < size) && stat; i++) {
		stat = (*xdr_elem) (xdrs, target);
		target += selem;
	}

	return (stat);
}

static inline bool
xdr_array_free(XDR *xdrs, char **cpp, u_int *sizep, u_int UNUSED(maxsize),
	       u_int selem, xdrproc_t xdr_elem)
{
	char *target = *cpp;
	u_int i = 0;
	uint32_t size = *sizep;
	bool stat = true;

	if (!target) {
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s:%u already free",
			__func__, __LINE__);
		return (true);
	}

	for (; (i < size) && stat; i++) {
		stat = (*xdr_elem) (xdrs, target);
		target += selem;
	}

	mem_free(*cpp, size * selem);
	*cpp = NULL;

	return (stat);
}

static inline bool
xdr_array(XDR *xdrs, char **cpp, u_int *sizep, u_int maxsize,
	  u_int selem, xdrproc_t xdr_elem)
{
	switch (xdrs->x_op) {
	case XDR_DECODE:
		return (xdr_array_decode(xdrs, cpp, sizep, maxsize, selem,
					 xdr_elem));
	case XDR_ENCODE:
		return (xdr_array_encode(xdrs, cpp, sizep, maxsize, selem,
					 xdr_elem));
	case XDR_FREE:
		return (xdr_array_free(xdrs, cpp, sizep, maxsize, selem,
				       xdr_elem));
	}

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s:%u ERROR xdrs->x_op (%u)",
		__func__, __LINE__,
		xdrs->x_op);
	return (false);
}

/*
 * Non-portable xdr primitives.
 * Care should be taken when moving these routines to new architectures.
 */

/*
 * XDR null terminated ASCII strings
 * xdr_string deals with "C strings" - arrays of bytes that are
 * terminated by a NULL character.  The parameter cpp references a
 * pointer to storage; If the pointer is null, then the necessary
 * storage is allocated.  The last parameter is the max allowed length
 * of the string as specified by a protocol.
 */
static inline bool
xdr_string_decode(XDR *xdrs, char **cpp, u_int maxsize)
{
	char *sp = *cpp;	/* sp is the actual string pointer */
	uint32_t size;
	u_int nodesize;
	bool ret;

	/*
	 * first deal with the length since xdr strings are counted-strings
	 */
	if (!XDR_GETUINT32(xdrs, &size)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size",
			__func__, __LINE__);
		return (false);
	}

	if (size > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %" PRIu32 " > max %u",
			__func__, __LINE__,
			size, maxsize);
		return (false);
	}

	/* The terminator must fit before adding one in 32-bit arithmetic. */
	if (size == UINT32_MAX) {
		/* caller provided very large maxsize */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR overflow %" PRIu32,
			__func__, __LINE__,
			size);
		return (false);
	}

	nodesize = size + 1;

	/*
	 * now deal with the actual bytes
	 */
	if (!sp)
		sp = (char *)mem_alloc(nodesize);

	ret = xdr_opaque_decode(xdrs, sp, size);
	if (!ret) {
		mem_free(sp, nodesize);
		return (ret);
	}
	sp[size] = '\0';
	*cpp = sp;			/* only valid pointer */
	return (ret);
}

static inline bool
xdr_string_encode(XDR *xdrs, char **cpp, u_int maxsize)
{
	u_long size;
	u_int nodesize;

	if (!(*cpp)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR missing string pointer",
			__func__, __LINE__);
		return (false);
	}

	size = strlen(*cpp);
	if (size > maxsize) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR size %ul > max %u",
			__func__, __LINE__,
			size, maxsize);
		return (false);
	}

	nodesize = (uint32_t)(size + 1);
	if (nodesize < (size + 1)) {
		/* caller provided very large maxsize */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR overflow %ul",
			__func__, __LINE__,
			size);
		return (false);
	}

	if (!XDR_PUTUINT32(xdrs, size))
		return (false);

	return (xdr_opaque_encode(xdrs, *cpp, size));
}

static inline bool
xdr_string_free(XDR *UNUSED(xdrs), char **cpp)
{
	if (*cpp) {
		mem_free(*cpp, strlen(*cpp) + 1);
		*cpp = NULL;
		return (true);
	}

	/* normal for switch x_op, sometimes useful to track */
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s:%u already free",
		__func__, __LINE__);
	return (true);
}

static inline bool
xdr_string(XDR *xdrs, char **cpp, u_int maxsize)
{
	switch (xdrs->x_op) {
	case XDR_DECODE:
		return (xdr_string_decode(xdrs, cpp, maxsize));
	case XDR_ENCODE:
		return (xdr_string_encode(xdrs, cpp, maxsize));
	case XDR_FREE:
		return (xdr_string_free(xdrs, cpp));
	}

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s:%u ERROR xdrs->x_op (%u)",
		__func__, __LINE__,
		xdrs->x_op);
	return (false);
}
#define inline_xdr_string xdr_string

/*
 * NOTE: xdr_hyper(), xdr_u_hyper(), xdr_longlong_t(), and xdr_u_longlong_t()
 * are in the "non-portable" section because they require that a `long long'
 * be a 64-bit type.
 *
 * --thorpej@netbsd.org, November 30, 1999
 */

/*
 * XDR longlong_t's
 */
static inline bool
inline_xdr_longlong_t(XDR *xdrs, quad_t *llp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_int64_t().
	 */
	return (inline_xdr_int64_t(xdrs, (int64_t *) llp));
}

/*
 * XDR u_longlong_t's
 */
static inline bool
inline_xdr_u_longlong_t(XDR *xdrs, u_quad_t *ullp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_u_int64_t().
	 */
	return (inline_xdr_u_int64_t(xdrs, (u_int64_t *) ullp));
}

#ifdef __cplusplus
}
#endif /* extern "C" */

#endif				/* _TIRPC_INLINE_XDR_H */
