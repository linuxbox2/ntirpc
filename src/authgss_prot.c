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

/* additional space needed for encoding */
#define RPC_SLACK_SPACE 1024
#define AUTHGSS_MAX_TOKEN_SIZE 24576 /* default MS PAC is 12000 bytes */

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

bool
xdr_rpc_gss_wrap(XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr,
		 gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc, u_int seq)
{
	gss_buffer_desc databuf, wrapbuf;
	OM_uint32 maj_stat, min_stat;
	int start, end, conf_state;
	bool xdr_stat;
	u_int databuflen, maxwrapsz;

	/* Write dummy for databody length. */
	start = XDR_GETPOS(xdrs);
	databuflen = 0xaaaaaaaa;	/* should always overwrite */
	if (!XDR_PUTUINT32(xdrs, databuflen))
		return (FALSE);

	memset(&databuf, 0, sizeof(databuf));
	memset(&wrapbuf, 0, sizeof(wrapbuf));

	/* Marshal rpc_gss_data_t (sequence number + arguments). */
	if (!XDR_PUTUINT32(xdrs, seq) || !(*xdr_func) (xdrs, xdr_ptr))
		return (FALSE);
	end = XDR_GETPOS(xdrs);

	/* Set databuf to marshalled rpc_gss_data_t. */
	databuflen = end - start - 4;
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

	xdr_stat = FALSE;

	if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
		/* Marshal databody_integ length. */
		if (!XDR_SETPOS(xdrs, start)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_SETPOS #2 failed",
				__func__);
			return (FALSE);
		}
		if (!XDR_PUTUINT32(xdrs, databuflen))
			return (FALSE);

		/* Checksum rpc_gss_data_t. */
		maj_stat = gss_get_mic(&min_stat, ctx, qop, &databuf, &wrapbuf);
		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() gss_get_mic failed",
				__func__);
			return (FALSE);
		}
		/* Marshal checksum. */
		if (!XDR_SETPOS(xdrs, end)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() XDR_SETPOS #3 failed",
				__func__);
			gss_release_buffer(&min_stat, &wrapbuf);
			return (FALSE);
		}
		maxwrapsz = (u_int) (wrapbuf.length + RPC_SLACK_SPACE);
		xdr_stat = xdr_rpc_gss_encode(xdrs, &wrapbuf, maxwrapsz);
		gss_release_buffer(&min_stat, &wrapbuf);
	} else if (svc == RPCSEC_GSS_SVC_PRIVACY) {
		/* Encrypt rpc_gss_data_t. */
		maj_stat =
		    gss_wrap(&min_stat, ctx, TRUE, qop, &databuf, &conf_state,
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
	}
	if (!xdr_stat) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS, "%s() failed", __func__);
	}
	return (xdr_stat);
}

bool
xdr_rpc_gss_unwrap(XDR *xdrs, xdrproc_t xdr_func, void *xdr_ptr,
		   gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc,
		   u_int seq)
{
	XDR tmpxdrs;
	gss_buffer_desc databuf, wrapbuf;
	OM_uint32 maj_stat, min_stat;
	u_int qop_state;
	int conf_state;
	uint32_t seq_num;
	bool xdr_stat;

	if (xdr_func == (xdrproc_t) xdr_void || xdr_ptr == NULL)
		return (TRUE);

	memset(&databuf, 0, sizeof(databuf));
	memset(&wrapbuf, 0, sizeof(wrapbuf));

	if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
		/* Decode databody_integ. */
		if (!xdr_rpc_gss_decode(xdrs, &databuf)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() xdr_rpc_gss_decode databody_integ failed",
				__func__);
			return (FALSE);
		}
		/* Decode checksum. */
		if (!xdr_rpc_gss_decode(xdrs, &wrapbuf)) {
			gss_release_buffer(&min_stat, &databuf);
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() xdr_rpc_gss_decode checksum failed",
				__func__);
			return (FALSE);
		}
		/* Verify checksum and QOP. */
		maj_stat =
		    gss_verify_mic(&min_stat, ctx, &databuf, &wrapbuf,
				   &qop_state);
		gss_release_buffer(&min_stat, &wrapbuf);

		if (maj_stat != GSS_S_COMPLETE || qop_state != qop) {
			gss_release_buffer(&min_stat, &databuf);
			gss_log_status("gss_verify_mic", maj_stat, min_stat);
			return (FALSE);
		}
	} else if (svc == RPCSEC_GSS_SVC_PRIVACY) {
		/* Decode databody_priv. */
		if (!xdr_rpc_gss_decode(xdrs, &wrapbuf)) {
			__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
				"%s() xdr_rpc_gss_decode databody_priv failed",
				__func__);
			return (FALSE);
		}
		/* Decrypt databody. */
		maj_stat =
		    gss_unwrap(&min_stat, ctx, &wrapbuf, &databuf, &conf_state,
			       &qop_state);

		gss_release_buffer(&min_stat, &wrapbuf);

		/* Verify encryption and QOP. */
		if (maj_stat != GSS_S_COMPLETE || qop_state != qop
		    || conf_state != TRUE) {
			gss_release_buffer(&min_stat, &databuf);
			gss_log_status("gss_unwrap", maj_stat, min_stat);
			return (FALSE);
		}
	}
	/* Decode rpc_gss_data_t (sequence number + arguments). */
	xdrmem_create(&tmpxdrs, databuf.value, databuf.length, XDR_DECODE);
	xdr_stat = (XDR_GETUINT32(&tmpxdrs, &seq_num)
		    && (*xdr_func) (&tmpxdrs, xdr_ptr));
	XDR_DESTROY(&tmpxdrs);
	gss_release_buffer(&min_stat, &databuf);

	/* Verify sequence number. */
	if (xdr_stat == TRUE && seq_num != seq) {
		__warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
			"%s() wrong sequence number in databody",
			__func__);
		return (FALSE);
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
