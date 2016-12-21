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

#include <config.h>
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
xdr_rpc_gss_buf(XDR *xdrs, gss_buffer_t buf, u_int maxsize)
{
	bool xdr_stat;
	u_int tmplen = 0;

	if (xdrs->x_op != XDR_DECODE) {
		if (buf->length > UINT_MAX)
			return FALSE;
		else
			tmplen = buf->length;
	}
	xdr_stat =
	    inline_xdr_opaques(xdrs, buf->value, &tmplen, maxsize);

	if (xdr_stat && xdrs->x_op == XDR_DECODE)
		buf->length = tmplen;

	log_debug("xdr_rpc_gss_buf: %s %s (%p:%d)",
		  (xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		  (xdr_stat == TRUE) ? "success" : "failure", buf->value,
		  buf->length);

	return xdr_stat;
}

bool
xdr_rpc_gss_cred(XDR *xdrs, struct rpc_gss_cred *p)
{
	bool xdr_stat;

	xdr_stat = (inline_xdr_u_int(xdrs, &p->gc_v)
		    && inline_xdr_enum(xdrs, (enum_t *) &p->gc_proc)
		    && inline_xdr_u_int(xdrs, &p->gc_seq)
		    && inline_xdr_enum(xdrs, (enum_t *) &p->gc_svc)
		    && xdr_rpc_gss_buf(xdrs, &p->gc_ctx, MAX_AUTH_BYTES));

	log_debug("xdr_rpc_gss_cred: %s %s "
		  "(v %d, proc %d, seq %d, svc %d, ctx %p:%d)",
		  (xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		  (xdr_stat == TRUE) ? "success" : "failure", p->gc_v,
		  p->gc_proc, p->gc_seq, p->gc_svc, p->gc_ctx.value,
		  p->gc_ctx.length);

	return (xdr_stat);
}

bool
xdr_rpc_gss_init_args(XDR *xdrs, gss_buffer_desc *p)
{
	bool xdr_stat;
	u_int maxlen = AUTHGSS_MAX_TOKEN_SIZE;

	xdr_stat = xdr_rpc_gss_buf(xdrs, p, maxlen);

	log_debug("xdr_rpc_gss_init_args: %s %s (token %p:%d)",
		  (xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		  (xdr_stat == TRUE) ? "success" : "failure", p->value,
		  p->length);

	return (xdr_stat);
}

bool
xdr_rpc_gss_init_res(XDR *xdrs, struct rpc_gss_init_res *p)
{
	bool xdr_stat;

	u_int ctx_maxlen = (u_int) (p->gr_ctx.length + RPC_SLACK_SPACE);
	u_int tok_maxlen = (u_int) (p->gr_token.length + RPC_SLACK_SPACE);

	xdr_stat = (xdr_rpc_gss_buf(xdrs, &p->gr_ctx, ctx_maxlen)
		    && inline_xdr_u_int(xdrs, &p->gr_major)
		    && inline_xdr_u_int(xdrs, &p->gr_minor)
		    && inline_xdr_u_int(xdrs, &p->gr_win)
		    && xdr_rpc_gss_buf(xdrs, &p->gr_token, tok_maxlen));

	log_debug("xdr_rpc_gss_init_res %s %s "
		  "(ctx %p:%d, maj %d, min %d, win %d, token %p:%d)",
		  (xdrs->x_op == XDR_ENCODE) ? "encode" : "decode",
		  (xdr_stat == TRUE) ? "success" : "failure", p->gr_ctx.value,
		  p->gr_ctx.length, p->gr_major, p->gr_minor, p->gr_win,
		  p->gr_token.value, p->gr_token.length);

	return (xdr_stat);
}

bool
xdr_rpc_gss_wrap_data(XDR *xdrs, xdrproc_t xdr_func, caddr_t xdr_ptr,
		      gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc,
		      u_int seq)
{
	gss_buffer_desc databuf, wrapbuf;
	OM_uint32 maj_stat, min_stat;
	int start, end, conf_state;
	bool xdr_stat;
	u_int databuflen, maxwrapsz;

	/* Write dummy for databody length. */
	start = XDR_GETPOS(xdrs);
	databuflen = 0xaaaaaaaa;	/* should always overwrite */
	if (!inline_xdr_u_int(xdrs, &databuflen))
		return (FALSE);

	memset(&databuf, 0, sizeof(databuf));
	memset(&wrapbuf, 0, sizeof(wrapbuf));

	/* Marshal rpc_gss_data_t (sequence number + arguments). */
	if (!inline_xdr_u_int(xdrs, &seq) || !(*xdr_func) (xdrs, xdr_ptr))
		return (FALSE);
	end = XDR_GETPOS(xdrs);

	/* Set databuf to marshalled rpc_gss_data_t. */
	databuflen = end - start - 4;
	if (!XDR_SETPOS(xdrs, start+4)) {
		log_debug("xdr_setpos#1 failed");
		return (FALSE);
	}
	databuf.value = XDR_INLINE(xdrs, databuflen);
	databuf.length = databuflen;

	if (!databuf.value) {
		log_debug("XDR_INLINE failed");
		return (FALSE);
	}

	xdr_stat = FALSE;

	if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
		/* Marshal databody_integ length. */
		if (!XDR_SETPOS(xdrs, start)) {
			log_debug("xdr_setpos#2 failed");
			return (FALSE);
		}
		if (!inline_xdr_u_int(xdrs, &databuflen))
			return (FALSE);

		/* Checksum rpc_gss_data_t. */
		maj_stat = gss_get_mic(&min_stat, ctx, qop, &databuf, &wrapbuf);
		if (maj_stat != GSS_S_COMPLETE) {
			log_debug("gss_get_mic failed");
			return (FALSE);
		}
		/* Marshal checksum. */
		if (!XDR_SETPOS(xdrs, end)) {
			log_debug("xdr_setpos#3 failed");
			gss_release_buffer(&min_stat, &wrapbuf);
			return (FALSE);
		}
		maxwrapsz = (u_int) (wrapbuf.length + RPC_SLACK_SPACE);
		xdr_stat = xdr_rpc_gss_buf(xdrs, &wrapbuf, maxwrapsz);
		gss_release_buffer(&min_stat, &wrapbuf);
	} else if (svc == RPCSEC_GSS_SVC_PRIVACY) {
		/* Encrypt rpc_gss_data_t. */
		maj_stat =
		    gss_wrap(&min_stat, ctx, TRUE, qop, &databuf, &conf_state,
			     &wrapbuf);
		if (maj_stat != GSS_S_COMPLETE) {
			log_status("gss_wrap", maj_stat, min_stat);
			return (FALSE);
		}
		/* Marshal databody_priv. */
		if (!XDR_SETPOS(xdrs, start)) {
			log_debug("xdr_setpos#4 failed");
			gss_release_buffer(&min_stat, &wrapbuf);
			return (FALSE);
		}
		maxwrapsz = (u_int) (wrapbuf.length + RPC_SLACK_SPACE);
		xdr_stat = xdr_rpc_gss_buf(xdrs, &wrapbuf, maxwrapsz);
		gss_release_buffer(&min_stat, &wrapbuf);
	}
	if (!xdr_stat) {
		log_debug("xdr_rpc_gss_wrap_data failed");
	}
	return (xdr_stat);
}

bool
xdr_rpc_gss_unwrap_data(XDR *xdrs, xdrproc_t xdr_func, caddr_t xdr_ptr,
			gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc,
			u_int seq)
{
	XDR tmpxdrs;
	gss_buffer_desc databuf, wrapbuf;
	OM_uint32 maj_stat, min_stat;
	u_int seq_num, qop_state;
	int conf_state;
	bool xdr_stat;

	if (xdr_func == (xdrproc_t) xdr_void || xdr_ptr == NULL)
		return (TRUE);

	memset(&databuf, 0, sizeof(databuf));
	memset(&wrapbuf, 0, sizeof(wrapbuf));

	if (svc == RPCSEC_GSS_SVC_INTEGRITY) {
		/* Decode databody_integ. */
		if (!xdr_rpc_gss_buf(xdrs, &databuf, (u_int) -1)) {
			log_debug("xdr decode databody_integ failed");
			return (FALSE);
		}
		/* Decode checksum. */
		if (!xdr_rpc_gss_buf(xdrs, &wrapbuf, (u_int) -1)) {
			gss_release_buffer(&min_stat, &databuf);
			log_debug("xdr decode checksum failed");
			return (FALSE);
		}
		/* Verify checksum and QOP. */
		maj_stat =
		    gss_verify_mic(&min_stat, ctx, &databuf, &wrapbuf,
				   &qop_state);
		gss_release_buffer(&min_stat, &wrapbuf);

		if (maj_stat != GSS_S_COMPLETE || qop_state != qop) {
			gss_release_buffer(&min_stat, &databuf);
			log_status("gss_verify_mic", maj_stat, min_stat);
			return (FALSE);
		}
	} else if (svc == RPCSEC_GSS_SVC_PRIVACY) {
		/* Decode databody_priv. */
		if (!xdr_rpc_gss_buf(xdrs, &wrapbuf, (u_int) -1)) {
			log_debug("xdr decode databody_priv failed");
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
			log_status("gss_unwrap", maj_stat, min_stat);
			return (FALSE);
		}
	}
	/* Decode rpc_gss_data_t (sequence number + arguments). */
	xdrmem_create(&tmpxdrs, databuf.value, databuf.length, XDR_DECODE);
	xdr_stat = (xdr_u_int(&tmpxdrs, &seq_num)
		    && (*xdr_func) (&tmpxdrs, xdr_ptr));
	XDR_DESTROY(&tmpxdrs);
	gss_release_buffer(&min_stat, &databuf);

	/* Verify sequence number. */
	if (xdr_stat == TRUE && seq_num != seq) {
		log_debug("wrong sequence number in databody");
		return (FALSE);
	}
	return (xdr_stat);
}

bool
xdr_rpc_gss_data(XDR *xdrs, xdrproc_t xdr_func, caddr_t xdr_ptr,
		 gss_ctx_id_t ctx, gss_qop_t qop, rpc_gss_svc_t svc,
		 u_int seq)
{
	switch (xdrs->x_op) {

	case XDR_ENCODE:
		return (xdr_rpc_gss_wrap_data
			(xdrs, xdr_func, xdr_ptr, ctx, qop, svc, seq));
	case XDR_DECODE:
		return (xdr_rpc_gss_unwrap_data
			(xdrs, xdr_func, xdr_ptr, ctx, qop, svc, seq));
	case XDR_FREE:
		return (TRUE);
	}
	return (FALSE);
}

#ifdef DEBUG
#include <ctype.h>

#define log_debug __warnx

#if 0
void log_debug(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "rpcsec_gss: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}
#endif

void
log_status(char *m, OM_uint32 maj_stat, OM_uint32 min_stat)
{
	OM_uint32 min;
	gss_buffer_desc msg;
	int msg_ctx = 0;

	__warnx("rpcsec_gss: %s: ", m);

	gss_display_status(&min, maj_stat, GSS_C_GSS_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg);
	__warnx("%s - ", (char *)msg.value);
	gss_release_buffer(&min, &msg);

	gss_display_status(&min, min_stat, GSS_C_MECH_CODE, GSS_C_NULL_OID,
			   &msg_ctx, &msg);
	__warnx("%s\n", (char *)msg.value);
	gss_release_buffer(&min, &msg);
}

void
gss_log_hexdump(const u_char *buf, int len, int offset)
{
	u_int i, j, jm;
	int c;

	__warnx("\n");
	for (i = 0; i < len; i += 0x10) {
		__warnx("  %04x: ", (u_int) (i + offset));
		jm = len - i;
		jm = jm > 16 ? 16 : jm;

		for (j = 0; j < jm; j++) {
			if ((j % 2) == 1)
				__warnx("%02x ", (u_int) buf[i + j]);
			else
				__warnx("%02x", (u_int) buf[i + j]);
		}
		for (; j < 16; j++) {
			if ((j % 2) == 1)
				printf("   ");
			else
				__warnx("  ");
		}
		__warnx(" ");

		for (j = 0; j < jm; j++) {
			c = buf[i + j];
			c = isprint(c) ? c : '.';
			__warnx("%c", c);
		}
		__warnx("\n");
	}
}

#else

void
log_debug(const char *fmt, ...)
{
}

void
log_status(char *m, OM_uint32 maj_stat, OM_uint32 min_stat)
{
}

void loggss__hexdump(const u_char *buf, int len, int offset)
{
}

#endif
