/*
  authgss_prot.c

  Copyright (c) 2000 The Regents of the University of Michigan.
  All rights reserved.

  Copyright (c) 2000 Dug Song <dugsong@UMICH.EDU>.
  All rights reserved, all wrongs reversed.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  3. Neither the name of the University nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <rpc/types.h>
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>
#include <rpc/auth.h>
#include <rpc/auth_gss.h>
#include <rpc/rpc.h>
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>

/* additional space needed for encoding */
#define RPC_SLACK_SPACE 1024
#define AUTHGSS_MAX_TOKEN_SIZE 24576 /* default MS PAC is 12000 bytes */
#define MAXALLOCA (256)

bool
xdr_rpc_gss_encode(XDR *xdrs, gss_buffer_t buf, u_int maxsize)
{
	u_int tmplen = buf->length;
	bool xdr_stat;

	if (buf->length > UINT_MAX)
		return FALSE;

	xdr_stat = xdr_bytes_encode(xdrs, (char **)&buf->value, &tmplen,
					   maxsize);

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() %s (%p:%d)",
		__func__,
		(xdr_stat == TRUE) ? "success" : "failure",
		buf->value, buf->length);

	return xdr_stat;
}

bool
xdr_rpc_gss_decode(XDR *xdrs, gss_buffer_t buf)
{
	u_int tmplen = 0;
	bool xdr_stat;

	xdr_stat = xdr_bytes_decode(xdrs, (char **)&buf->value, &tmplen,
					   UINT_MAX);

	if (xdr_stat)
		buf->length = tmplen;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() %s (%p:%d)",
		__func__,
		(xdr_stat == TRUE) ? "success" : "failure",
		buf->value, buf->length);

	return xdr_stat;
}

static bool
xdr_rpc_gss_buf(XDR *xdrs, gss_buffer_t buf, u_int maxsize)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (xdr_rpc_gss_encode(xdrs, buf, maxsize));
	case XDR_DECODE:
		return (xdr_rpc_gss_decode(xdrs, buf));
	case XDR_FREE:
		mem_free(buf->value, buf->length);
		return (TRUE);
	};
	return (FALSE);
}

bool
xdr_rpc_gss_cred(XDR *xdrs, struct rpc_gss_cred *p)
{
	bool xdr_stat;

	xdr_stat = (inline_xdr_u_int32_t(xdrs, &p->gc_v)
		    && inline_xdr_enum(xdrs, (enum_t *) &p->gc_proc)
		    && inline_xdr_u_int32_t(xdrs, &p->gc_seq)
		    && inline_xdr_enum(xdrs, (enum_t *) &p->gc_svc)
		    && xdr_rpc_gss_buf(xdrs, &p->gc_ctx, MAX_AUTH_BYTES));

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"%s() %s %s (v %" PRIu32 ", proc %" PRIu32 ", seq %" PRIu32 ", svc %" PRIu32 ", ctx %p:%d)",
		__func__,
		(xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		(xdr_stat == TRUE) ? "success" : "failure",
		p->gc_v, p->gc_proc, p->gc_seq, p->gc_svc,
		p->gc_ctx.value, p->gc_ctx.length);

	return (xdr_stat);
}

bool
xdr_rpc_gss_init_args(XDR *xdrs, gss_buffer_desc *p)
{
	bool xdr_stat;
	u_int maxlen = AUTHGSS_MAX_TOKEN_SIZE;

	xdr_stat = xdr_rpc_gss_buf(xdrs, p, maxlen);

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() %s %s (token %p:%d)",
		__func__,
		(xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		(xdr_stat == TRUE) ? "success" : "failure",
		p->value, p->length);

	return (xdr_stat);
}

bool
xdr_rpc_gss_init_res(XDR *xdrs, struct rpc_gss_init_res *p)
{
	bool xdr_stat;

	u_int ctx_maxlen = (u_int) (p->gr_ctx.length + RPC_SLACK_SPACE);
	u_int tok_maxlen = (u_int) (p->gr_token.length + RPC_SLACK_SPACE);

	xdr_stat = (xdr_rpc_gss_buf(xdrs, &p->gr_ctx, ctx_maxlen)
		    && inline_xdr_u_int32_t(xdrs, &p->gr_major)
		    && inline_xdr_u_int32_t(xdrs, &p->gr_minor)
		    && inline_xdr_u_int32_t(xdrs, &p->gr_win)
		    && xdr_rpc_gss_buf(xdrs, &p->gr_token, tok_maxlen));

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"%s() %s %s (ctx %p:%d, maj %" PRIu32 ", min %" PRIu32 ", win %" PRIu32 ", token %p:%d)",
		__func__,
		(xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		(xdr_stat == TRUE) ? "success" : "failure",
		p->gr_ctx.value, p->gr_ctx.length,
		p->gr_major, p->gr_minor, p->gr_win,
		p->gr_token.value, p->gr_token.length);

	return (xdr_stat);
}

void
gss_log_error(char *m, OM_uint32 maj_stat, OM_uint32 min_stat)
{
	OM_uint32 min;
	gss_buffer_desc msg1, msg2;
	unsigned int msg_ctx = 0;

	gss_display_status(&min, maj_stat, GSS_C_GSS_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg1);

	gss_display_status(&min, min_stat, GSS_C_MECH_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg2);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "rpcsec_gss: %s: %s - %s\n",
		m, (char *)msg1.value, (char *)msg2.value);
	gss_release_buffer(&min, &msg1);
	gss_release_buffer(&min, &msg2);
}

void show_gss_xdr_iov(gss_iov_buffer_desc *gss_iov, int gss_count,
		      xdr_vio *xdr_iov, int xdr_count, const char *desc)
{
	int i;

	/* Now show the gss_iov */
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"Show the gss_iov %s %p", desc, gss_iov);

	for (i = 0; i < gss_count; i++) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"buf %d type %d length %d value %p",
			i, gss_iov[i].type, gss_iov[i].buffer.length,
			gss_iov[i].buffer.value);
	}

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"Show the xdr_iov %s %p", desc, xdr_iov);

	if (xdr_iov == NULL)
		return;

	for (i = 0; i < xdr_count; i++) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"buf %d type %d (base %p head %p tail %p wrap %p) length %lu",
			i, xdr_iov[i].vio_type,
			xdr_iov[i].vio_base, xdr_iov[i].vio_head,
			xdr_iov[i].vio_tail, xdr_iov[i].vio_wrap,
			(unsigned long)(xdr_iov[i].vio_tail -
						xdr_iov[i].vio_head));
	}
}

bool
xdr_rpc_gss_wrap(XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr,
		 gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc, u_int seq)
{
	gss_buffer_desc databuf, wrapbuf;
	OM_uint32 maj_stat, min_stat;
	int start, end, conf_state, xv_count, gv_count, data_count, after_data, i;
	bool xdr_stat, vector;
	u_int databuflen, maxwrapsz;
	gss_iov_buffer_desc *gss_iov = NULL;
	xdr_vio *xdr_iov = NULL, *data;
	u_int32_t xvsize = 0, gvsize = 0;

	if (svc != RPCSEC_GSS_SVC_PRIVACY &&
	    svc != RPCSEC_GSS_SVC_INTEGRITY) {
		/* For some reason we got here with not supported type. */
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() svc != RPCSEC_GSS_SVC_PRIVACY or RPCSEC_GSS_SVC_INTEGRITY",
			__func__);
		return (FALSE);
	}

	/* Write dummy for databody length. The length will be filled in later.
	 * - For RPCSEC_GSS_SVC_PRIVACY the length will include the whole
	 *   result of gss_wrap.
	 * - For RPCSEC_GSS_SVC_INTEGRITY the length will just be the response
	 *   data length.
	 * No matter what type or how we process, we will come back and fill
	 * the length in exactly here.
	 */
	start = XDR_GETPOS(xdrs);
	databuflen = 0xaaaaaaaa;	/* should always overwrite */
	if (!XDR_PUTUINT32(xdrs, databuflen)) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() could not put databuflen",
			__func__);
		return (FALSE);
	}

	/* If we are doing PRIVACY, determine if XDR is a vector or not.
	 * INTEGRITY can work with non-vector xdrs like xdrmem because the
	 * MIC token will just be appended at the end.
	 * If it's privacy, and NEWBUF is supported (because xdrs is a vector)
	 * then NEWBUF will have allocated the new buffer.
	 */
	vector = (svc == RPCSEC_GSS_SVC_INTEGRITY) || XDR_NEWBUF(xdrs);

	/* Marshal rpc_gss_data_t (sequence number + arguments).
	 * If it's a vector, the response has been marshalled into a new
	 * buffer so that we will be able to insert any header.
	 */
	if (!XDR_PUTUINT32(xdrs, seq) || !(*xdr_func) (xdrs, xdr_ptr)) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() could not enocde rpc_gss_data_t",
			__func__);
		return (FALSE);
	}

	end = XDR_GETPOS(xdrs);
	databuflen = end - start - 4;

	if (vector) {
		/* Now we have the response encoded, time to build out iov for
		 * gss_get_mic_iov or gss_wrap_iov.
		 *
		 * vsize = ioq count + 2 (for header and trailer)
		 */
		data_count = XDR_IOVCOUNT(xdrs, start + 4, databuflen);

		if (data_count < 0) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() data_count = %d",
				__func__, data_count);
			return (FALSE);
		}

		if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
			/* Add a trailer length (which won't be part of the gss_iov
			 * and trailer buffer for the MIC
			 */
			xv_count = data_count + 2;
			gv_count = data_count + 1;
			after_data = data_count;
		} else /* svc == RPCSEC_GSS_SVC_PRIVACY */ {
			/* Add header, padding, and trailer for the wrap */
			xv_count = data_count + 3;
			gv_count = data_count + 3;
			after_data = data_count + 1;
		}

		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"data_count=%d, gv_count=%d, xv_count=%d, after_data=%d",
			data_count, gv_count, xv_count, after_data);

		/* Determine the size of the gss_iov */
		gvsize = gv_count * sizeof(gss_iov_buffer_desc);
		xvsize = xv_count * sizeof(xdr_vio);

		/* Allocate the gss_iov */
		if (unlikely(gvsize > MAXALLOCA)) {
			gss_iov = mem_alloc(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				gv_count);
		} else {
			gss_iov = alloca(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				gv_count);
		}

		/* Allocate the xdr_iov */
		if (unlikely(xvsize > MAXALLOCA)) {
			xdr_iov = mem_alloc(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				xv_count);
		} else {
			xdr_iov = alloca(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				xv_count);
		}

		memset(gss_iov, 0, gvsize);
		memset(xdr_iov, 0, xvsize);

		/* Point to where the first buffer in the data will be. */
		data = &xdr_iov[(svc == RPCSEC_GSS_SVC_PRIVACY) ? 1 : 0];

		/* Now fill in the data buffers
		 * vector is empty on entry
		 * DATA buffers are completely filled (vio_base, vio_head,
		 *   vio_tail, vio_wrap, vio_length, and vio_type) on exit.
		 * No other buffers are touched at this point.
		 */
		xdr_stat = XDR_FILLBUFS(xdrs, start + 4, data, databuflen);

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
				 "after XDR_FILLBUFS");

		/* Now set up the gss_iov */
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "Set up gss_iov");
		for (i = 0; i < gv_count; i++) {
			if (i == 0 && svc == RPCSEC_GSS_SVC_PRIVACY) {
				/* Fill in HEADER buffer */
				gss_iov[i].type = GSS_IOV_BUFFER_TYPE_HEADER;
			} else if (i < after_data) {
				/* Copy over a DATA buffer */
				gss_iov[i].type = GSS_IOV_BUFFER_TYPE_DATA;
				gss_iov[i].buffer.length =
							xdr_iov[i].vio_length;
				gss_iov[i].buffer.value =
							xdr_iov[i].vio_head;
			} else if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
				/* Set up TRAILER buffer for INTEGRITY*/
				gss_iov[i].type = GSS_IOV_BUFFER_TYPE_MIC_TOKEN;
			} else if (i == after_data) {
				/* Set up PADDING buffer for PRIVACY*/
				gss_iov[i].type = GSS_IOV_BUFFER_TYPE_PADDING;
			} else {
				/* Set up TRAILER buffer for PRIVACY*/
				gss_iov[i].type = GSS_IOV_BUFFER_TYPE_TRAILER;
			}
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"buf %d type %d length %d value %p",
				i, gss_iov[i].type, gss_iov[i].buffer.length,
				gss_iov[i].buffer.value);
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
				 "after setting up gss_iov");

		/* At this point gss_iov HEADER, PADDING, and TRAILER have
		 * type set and buffer is empty.
		 * DATA is completely filled in.
		 * xdr_iov DATA buffers are completely filled in.
		 * xdr_iov HEADER and TRAILER buffers are empty.
		 */

		if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
			/* Now call gss_get_mic_iov_length */
			maj_stat = gss_get_mic_iov_length(&min_stat, ctx, qop,
							  gss_iov, gv_count);

			if (maj_stat != GSS_S_COMPLETE) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() gss_get_mic_iov_length failed",
					__func__);
				gss_log_error("gss_get_mic_iov_length",
					      maj_stat, min_stat);
				xdr_stat = FALSE;
				goto out;
			}

			/* Set up the VIO_TRAILER_LEN buffer in the xdr_iov */
			xdr_iov[after_data].vio_length = BYTES_PER_XDR_UNIT;
			xdr_iov[after_data].vio_type = VIO_TRAILER_LEN;

			/* Copy the TRAILER buffer length into the xdr_iov */
			xdr_iov[after_data + 1].vio_length =
				gss_iov[after_data].buffer.length;
			xdr_iov[after_data + 1].vio_type = VIO_TRAILER;

			/* Marshal databody_integ length. Note tha this will
			 * leave the cursor position at start + 4 but the
			 * forthcoming XDR_ALLOCHDRS is going to fix the
			 * cursor position to the end of everything.
			 */
			if (!XDR_SETPOS(xdrs, start)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_SETPOS #2 failed",
					__func__);
				return (FALSE);
			}

			if (!XDR_PUTUINT32(xdrs, databuflen))
				return (FALSE);
		} else {
			u_int databody_priv_len;

			/* Now call gss_wrap_iov_length */
			maj_stat = gss_wrap_iov_length(&min_stat, ctx, true,
						       qop, GSS_C_QOP_DEFAULT,
						       gss_iov, gv_count);

			if (maj_stat != GSS_S_COMPLETE) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() gss_wrap_iov_length failed",
					__func__);
				gss_log_error("gss_wrap_iov_length",
					      maj_stat, min_stat);
				xdr_stat = FALSE;
				goto out;
			}

			/* Copy the HEADER buffer length into the xdr_iov */
			xdr_iov[0].vio_length = gss_iov[0].buffer.length;
			xdr_iov[0].vio_type = VIO_HEADER;

			/* Copy the PADDING buffer length into the xdr_iov */
			xdr_iov[after_data].vio_length =
				gss_iov[after_data].buffer.length;
			xdr_iov[after_data].vio_type = VIO_TRAILER;

			/* Copy the TRAILER buffer length into the xdr_iov */
			xdr_iov[after_data + 1].vio_length =
				gss_iov[after_data + 1].buffer.length;
			xdr_iov[after_data + 1].vio_type = VIO_TRAILER;

			/* Compute the databody_priv length as sum of
			 * the databuflen and the HEADER, PADDING, and
			 * TRAILER buffers.
			 */
			databody_priv_len = databuflen +
					gss_iov[0].buffer.length +
					gss_iov[after_data].buffer.length +
					gss_iov[after_data + 1].buffer.length;

			/* Marshal databody_priv length. Note tha this will
			 * leave the cursor position at start + 4 but the
			 * forthcoming XDR_ALLOCHDRS is going to fix the
			 * cursor position to the end of everything.
			 */
			if (!XDR_SETPOS(xdrs, start)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_SETPOS #2 failed",
					__func__);
				return (FALSE);
			}

			if (!XDR_PUTUINT32(xdrs, databody_priv_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_PUTUINT32 databody_priv_len failed",
					__func__);
				return (FALSE);
			}
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
				 "after gss_...length");

		/* At this point:
		 * The xdr_iov DATA buffers are completely filled in.
		 * The xdr_iov HEADER and TRAILER buffers have type and length
		 *   filled in.
		 */

		/* Now actually allocate the HEADER, PADDING, and TRAILER.
		 * The cursor position will be updated to the end of the
		 * TRAILER.
		 */
		xdr_stat = XDR_ALLOCHDRS(xdrs, start + 4, xdr_iov, xv_count);

		if (!xdr_stat)
			goto out;

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
				 "after XDR_ALLOCHDRS");

		/* At this point the xdr_iov is completely filled in. */

		if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
			/* Copy the TRAILER buffer into the gss_iov (remember
			 * it's AFTER the VIO_TRAILER_LEN buffer.
			 */
			gss_iov[after_data].buffer.value =
				xdr_iov[after_data + 1].vio_head;

			/* At this point the gss_iov is completely filled in */

			/* Now show the gss_iov and xdr_iov */
			show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
					 "just before gss_get_mic_iov");

			/* Now call gss_get_mic_iov */
			maj_stat = gss_get_mic_iov(&min_stat, ctx, qop,
						   gss_iov, gv_count);

			if (maj_stat != GSS_S_COMPLETE) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() gss_get_mic_iov failed",
					__func__);
				gss_log_error("gss_get_mic_iov",
					      maj_stat, min_stat);
				xdr_stat = FALSE;
				goto out;
			}
		} else {
			/* Copy the HEADER buffer into the gss_iov */
			gss_iov[0].buffer.value = xdr_iov[0].vio_head;

			/* Copy the PADDING buffer into the gss_iov */
			gss_iov[after_data].buffer.value =
				xdr_iov[after_data].vio_head;

			/* Copy the TRAILER buffer into the gss_iov */
			gss_iov[after_data + 1].buffer.value =
				xdr_iov[after_data + 1].vio_head;

			/* At this point the gss_iov is completely filled in */

			/* Now show the gss_iov and xdr_iov */
			show_gss_xdr_iov(gss_iov, gv_count, xdr_iov, xv_count,
					 "just before gss_wrap_iov");

			/* Now call gss_wrap_iov */
			maj_stat = gss_wrap_iov(&min_stat, ctx, true,
						GSS_C_QOP_DEFAULT, NULL,
						gss_iov, gv_count);

			if (maj_stat != GSS_S_COMPLETE) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() gss_wrap_iov failed",
					__func__);
				gss_log_error("gss_wrap_iov",
					      maj_stat, min_stat);
				xdr_stat = FALSE;
				goto out;
			}
		}

		/* At this point, the xdr_iov now has all the GSS data in it
		 * and wrapping is complete. Now we need to go back and write
		 * the length back at start.
		 */

		goto out;
	} /* else fall through to legacy single buffer implementation */

	/* Initialize the static buffers */
	memset(&databuf, 0, sizeof(databuf));
	memset(&wrapbuf, 0, sizeof(wrapbuf));

	/* Set databuf to marshalled rpc_gss_data_t. */
	if (!XDR_SETPOS(xdrs, start+4)) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() XDR_SETPOS #1 failed",
			__func__);
		return (FALSE);
	}
	databuf.length = databuflen;
	databuf.value = xdr_inline_encode(xdrs, databuflen);

	if (!databuf.value) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() xdr_inline_encode failed",
			__func__);
		return (FALSE);
	}

	/* We only need the legacy inplementation for RPCSEC_GSS_SVC_PRIVACY */

	/* Encrypt rpc_gss_data_t. */
	maj_stat = gss_wrap(&min_stat, ctx, TRUE, qop, &databuf, &conf_state,
			    &wrapbuf);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_wrap", maj_stat, min_stat);
		return (FALSE);
	}

	/* Marshal databody_priv. */
	if (!XDR_SETPOS(xdrs, start)) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() XDR_SETPOS #4 failed",
			__func__);
		gss_release_buffer(&min_stat, &wrapbuf);
		return (FALSE);
	}

	maxwrapsz = (u_int) (wrapbuf.length + RPC_SLACK_SPACE);
	xdr_stat = xdr_rpc_gss_encode(xdrs, &wrapbuf, maxwrapsz);
	gss_release_buffer(&min_stat, &wrapbuf);

	if (!xdr_stat) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() failed", __func__);
	}

out:

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"check free gss_iov=%p size %llu",
		gss_iov, (unsigned long long) gvsize);

	if (unlikely(gvsize > MAXALLOCA)) {
		mem_free(gss_iov, gvsize);
	}

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"check free gss_iov=%p size %llu",
		xdr_iov, (unsigned long long) xvsize);

	if (unlikely(xvsize > MAXALLOCA)) {
		mem_free(xdr_iov, xvsize);
	}

	return (xdr_stat);
}

bool
xdr_rpc_gss_unwrap(XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr,
		   gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc,
		   u_int seq, checksum_func_t checksum_func, void *priv)
{
	XDR tmpxdrs, *usexdrs = xdrs;
	OM_uint32 maj_stat, min_stat;
	u_int qop_state, data_start, token_start, buffer_len = 0;
	int conf_state, iov_count, token_iov_count, i;
	uint32_t seq_num, xvsize = 0, gvsize = 0, data_len, token_len;
	bool xdr_stat;
	gss_iov_buffer_desc *gss_iov = NULL;
	xdr_vio *xdr_iov = NULL;
	char *buffer = NULL;

	if (svc != RPCSEC_GSS_SVC_PRIVACY &&
	    svc != RPCSEC_GSS_SVC_INTEGRITY) {
		/* For some reason we got here with not supported type. */
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() svc != RPCSEC_GSS_SVC_PRIVACY or RPCSEC_GSS_SVC_INTEGRITY",
			__func__);
		return (FALSE);
	}

	if (xdr_func == (xdrproc_t) xdr_void || xdr_ptr == NULL)
		return (TRUE);

	if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
		/*
		 * first deal with the data length since xdr bytes are counted
		 */
		if (!XDR_GETUINT32(xdrs, &data_len)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s:%u ERROR size",
				__func__, __LINE__);
			return (FALSE);
		}
		data_start = XDR_GETPOS(xdrs);
		iov_count = XDR_IOVCOUNT(xdrs, data_start, data_len);
		if (iov_count < 0) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_IOVCOUNT signed data failed",
				__func__);
			return (FALSE);
		}
		if (!XDR_SETPOS(xdrs, data_start + data_len)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_SETPOS failed",
				__func__);
			return (FALSE);
		}
		if (!XDR_GETUINT32(xdrs, &token_len)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s:%u ERROR size",
				__func__, __LINE__);
			return (FALSE);
		}
		token_start = XDR_GETPOS(xdrs);
		token_iov_count = XDR_IOVCOUNT(xdrs, token_start, token_len); 
		if (token_iov_count < 0) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_IOVCOUNT MIC token failed",
				__func__);
			return (FALSE);
		}

		/* Determine the size of the gss_iov and xdr_iov. */
		gvsize = (iov_count + 1) * sizeof(gss_iov_buffer_desc);
		xvsize = (iov_count + 1) * sizeof(xdr_vio);

		/* Allocate the gss_iov */
		if (unlikely(gvsize > MAXALLOCA)) {
			gss_iov = mem_alloc(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				iov_count);
		} else {
			gss_iov = alloca(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				iov_count);
		}

		/* Allocate the xdr_iov */
		if (unlikely(xvsize > MAXALLOCA)) {
			xdr_iov = mem_alloc(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				iov_count);
		} else {
			xdr_iov = alloca(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				iov_count);
		}

		memset(gss_iov, 0, gvsize);
		memset(xdr_iov, 0, xvsize);

		if (token_iov_count != 1) {
			/* We need to allocate a token buffer */
			buffer = mem_alloc(token_len);
			buffer_len = token_len;
			gss_iov[iov_count].type = GSS_IOV_BUFFER_TYPE_MIC_TOKEN;
			gss_iov[iov_count].buffer.length = token_len;
			gss_iov[iov_count].buffer.value = buffer;

			/* Now extract the MIC token into the buffer */
			if (!xdr_opaque_decode(xdrs, buffer, token_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() xdr_opaque_decode MIC failed",
					__func__);
				xdr_stat = FALSE;
				goto out;
			 }
		} else {
			if (!XDR_FILLBUFS(xdrs, token_start,
					  &xdr_iov[iov_count], token_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_FILLBUFS MIC token failed",
					__func__);
				xdr_stat = FALSE;
				goto out;
			}
			gss_iov[iov_count].type = GSS_IOV_BUFFER_TYPE_MIC_TOKEN;
			gss_iov[iov_count].buffer.length = token_len;
			gss_iov[iov_count].buffer.value =
				xdr_iov[iov_count].vio_head;
			/* Consume the MIC token */
			if (!XDR_SETPOS(xdrs, token_start + token_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_SETPOS failed %lu %lu %lu",
					__func__,
					(unsigned long) token_start,
					(unsigned long) token_len,
					(unsigned long) token_start +
							token_len);
				xdr_stat = FALSE;
				goto out;
			}
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, iov_count + 1, xdr_iov, iov_count + 1,
				 "just before XDR_FILLBUFS for data buffers");

		/* Now fill in the data buffers
		 * DATA buffers are completely filled (vio_base, vio_head,
		 *   vio_tail, vio_wrap, vio_length, and vio_type) on exit.
		 */
		if (!XDR_FILLBUFS(xdrs, data_start, &xdr_iov[0], data_len)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_FILLBUFS integrity data failed",
				__func__);
			xdr_stat = FALSE;
			goto out;
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, iov_count + 1, xdr_iov, iov_count + 1,
				 "just after XDR_FILLBUFS for data buffers");

		/* Now set up the gss_iov */
		for (i = 0; i < iov_count; i++) {
			/* Copy over a DATA buffer */
			gss_iov[i].type = GSS_IOV_BUFFER_TYPE_DATA;
			gss_iov[i].buffer.length = xdr_iov[i].vio_length;
			gss_iov[i].buffer.value = xdr_iov[i].vio_head;
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"buf %d type %d length %d value %p",
				i, gss_iov[i].type, gss_iov[i].buffer.length,
				gss_iov[i].buffer.value);
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, iov_count + 1, xdr_iov, iov_count + 1,
				 "just before gss_verify_mic_iov");

		maj_stat = gss_verify_mic_iov(&min_stat, ctx, &qop_state,
					      gss_iov, iov_count + 1);

		if (maj_stat != GSS_S_COMPLETE || qop_state != qop) {
			gss_log_error("gss_verify_mic_iov",
				      maj_stat, min_stat);
			xdr_stat = FALSE;
			goto out;
		}

                /* Now we have verified. The data is still in place so we can
                 * decode the actual request from the original xdrs, so position
                 * to data_start so decode can begin.
                 */
		if (!XDR_SETPOS(xdrs, data_start)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_SETPOS to veriefied data start failed",
				__func__);
			xdr_stat = FALSE;
			goto out;
		}
	} else if (svc == RPCSEC_GSS_SVC_PRIVACY) {
		/*
		 * first deal with the token length since xdr bytes are counted
		 * token_start and token_len refer to the entire wrapped package
		 */
		if (!XDR_GETUINT32(xdrs, &token_len)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s:%u ERROR size",
				__func__, __LINE__);
			return (FALSE);
		}

		token_start = XDR_GETPOS(xdrs);
		iov_count = XDR_IOVCOUNT(xdrs, token_start, token_len);

		if (iov_count < 0) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_IOVCOUNT privacy data failed",
				__func__);
			return (FALSE);
		}

		/* Determine the size of the gss_iov and xdr_iov.
		 * NOTE: we only need a single xdr_iov buffer.
		 * The gss_iov will always be 2: STREAM, DATA
		 */
		gvsize = 2 * sizeof(gss_iov_buffer_desc);
		xvsize = sizeof(xdr_vio);

		/* Allocate the gss_iov */
		if (unlikely(gvsize > MAXALLOCA)) {
			gss_iov = mem_alloc(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				iov_count);
		} else {
			gss_iov = alloca(gvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca gss_iov=%p size %llu count %d",
				gss_iov, (unsigned long long) gvsize,
				iov_count);
		}

		/* Allocate the xdr_iov */
		if (unlikely(xvsize > MAXALLOCA)) {
			xdr_iov = mem_alloc(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"mem_alloc xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				iov_count);
		} else {
			xdr_iov = alloca(xvsize);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"alloca xdr_iov=%p size %llu count %d",
				xdr_iov, (unsigned long long) xvsize,
				iov_count);
		}

		memset(gss_iov, 0, gvsize);
		memset(xdr_iov, 0, xvsize);

		gss_iov[0].type = GSS_IOV_BUFFER_TYPE_STREAM;
		gss_iov[1].type = GSS_IOV_BUFFER_TYPE_DATA;

		if (iov_count == 1) {
			/* We can unwrap in place */
			if (!XDR_FILLBUFS(xdrs, token_start,
					  &xdr_iov[0], token_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_FILLBUFS wrap token failed",
					__func__);
				xdr_stat = FALSE;
				goto out;
			}

			gss_iov[0].buffer.length = xdr_iov[0].vio_length;
			gss_iov[0].buffer.value = xdr_iov[0].vio_head;
		} else {
			/* We need to extract into a single buffer to unwrap */
			buffer = mem_alloc(token_len);
			buffer_len = token_len;
			gss_iov[0].buffer.length = token_len;
			gss_iov[0].buffer.value = buffer;

			/* Now extract the wrap token into the buffer */
			if (!xdr_opaque_decode(xdrs, buffer, token_len)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() xdr_opaque_decode wrap token failed",
					__func__);
				xdr_stat = FALSE;
				goto out;
			 }
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, 2, xdr_iov, 1,
				 "just before gss_unwrap_iov");

		/* Now we have the wrap token in the STREAM buffer */
		maj_stat = gss_unwrap_iov(&min_stat, ctx, &conf_state,
					  &qop_state, gss_iov, 2);

		if (maj_stat != GSS_S_COMPLETE || qop_state != qop) {
			gss_log_error("gss_unwrap_iov", maj_stat, min_stat);
			xdr_stat = FALSE;
			goto out;
		}

		/* Now show the gss_iov and xdr_iov */
		show_gss_xdr_iov(gss_iov, 2, xdr_iov, 1,
				 "just after gss_unwrap_iov");

		if (iov_count == 1) {
			/* We can decode in place, find the data_start by
			 * determining the offset within the STREAM
			 * that gss_unwrap_iov indicated via the DATA buffer
			 * pointer.
			 */
			data_start = token_start + 
				     gss_iov[1].buffer.value -
				     gss_iov[0].buffer.value;
			data_len = gss_iov[1].buffer.length;

			if (!XDR_SETPOS(xdrs, data_start)) {
				__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
					"%s() XDR_SETPOS to veriefied data start failed",
					__func__);
				xdr_stat = FALSE;
				goto out;
			}
		} else {
			/* We need to create an xdrmem from the DATA buffer */
			xdrmem_create(&tmpxdrs, gss_iov[1].buffer.value,
				      gss_iov[1].buffer.length, XDR_DECODE);
			usexdrs = &tmpxdrs;
		}
	}

	/* At this point, usexdrs has been set either to the original xdrs
	 * up front, or due to the need to unwrap a multi-buffer token, has
	 * been set to &tmpxdrs.
	 */

	/* If checksum is requested perform it. */
	if (checksum_func != NULL) {
		checksum_func(priv, usexdrs->x_data, xdr_size_inline(usexdrs));
	}

	/* Decode rpc_gss_data_t (sequence number + arguments). */
	xdr_stat = (XDR_GETUINT32(usexdrs, &seq_num)
		    && (*xdr_func) (usexdrs, xdr_ptr));

	if (usexdrs == &tmpxdrs) {
		/* If it's the tmpxdrs, then destroy the xdrmem we created. */
		XDR_DESTROY(&tmpxdrs);
	}

	/* Verify sequence number. */
	if (xdr_stat == TRUE && seq_num != seq) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() wrong sequence number in databody",
			__func__);
		return (FALSE);
	}

out:

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"check free gss_iov=%p size %llu",
		gss_iov, (unsigned long long) gvsize);

	if (unlikely(gvsize > MAXALLOCA)) {
		mem_free(gss_iov, gvsize);
	}

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"check free gss_iov=%p size %llu",
		xdr_iov, (unsigned long long) xvsize);

	if (unlikely(xvsize > MAXALLOCA)) {
		mem_free(xdr_iov, xvsize);
	}

	if (buffer != NULL) {
		mem_free(buffer, buffer_len);
	}

	return (xdr_stat);
}

#ifdef DEBUG
#include <ctype.h>

void
gss_log_status(char *m, OM_uint32 maj_stat, OM_uint32 min_stat)
{
	OM_uint32 min;
	gss_buffer_desc msg;
	int msg_ctx = 0;

	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "rpcsec_gss: %s: ", m);

	gss_display_status(&min, maj_stat, GSS_C_GSS_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s - ", (char *)msg.value);
	gss_release_buffer(&min, &msg);

	gss_display_status(&min, min_stat, GSS_C_MECH_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg);
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s\n", (char *)msg.value);
	gss_release_buffer(&min, &msg);
}

#define DUMP_BYTES_PER_GROUP (4)
#define DUMP_GROUPS_PER_LINE (4)
#define DUMP_BYTES_PER_LINE (DUMP_BYTES_PER_GROUP * DUMP_GROUPS_PER_LINE)

void
gss_log_hexdump(const u_char *buf, int len, int offset)
{
	char *buffer;
	uint8_t *datum = buf;
	int sized = len - offset;
	int buffered = (((sized / DUMP_BYTES_PER_LINE) + 1 /*partial line*/)
			* (12 /* heading */
			   + (((DUMP_BYTES_PER_GROUP * 2 /*%02X*/) + 1 /*' '*/)
			      * DUMP_GROUPS_PER_LINE)))
			+ 1 /*'\0'*/;
	int i = 0;
	int m = 0;

	if (sized == 0) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s()\n");
		return;
	}
	buffer = (char *)mem_alloc(buffered);

	while (sized > i) {
		int j = sized - i;
		int k = j < DUMP_BYTES_PER_LINE ? j : DUMP_BYTES_PER_LINE;
		int l = 0;
		int r = sprintf(&buffer[m], "\n%10d:", i);	/* heading */

		if (r < 0)
			goto quit;
		m += r;

		for (; l < k; l++) {
			if (l % DUMP_BYTES_PER_GROUP == 0)
				buffer[m++] = ' ';

			r = sprintf(&buffer[m], "%02X", datum[i++]);
			if (r < 0)
				goto quit;
			m += r;
		}
	}
quit:
	buffer[m] = '\0';	/* in case of error */
	__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
		"%s() %s\n",
		buffer);
	mem_free(buffer, buffered);
}

#else

void
gss_log_status(char *m, OM_uint32 maj_stat, OM_uint32 min_stat)
{
}

void
gss_log_hexdump(const u_char *buf, int len, int offset)
{
}

#endif
