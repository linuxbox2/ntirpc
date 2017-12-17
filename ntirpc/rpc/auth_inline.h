/* $NetBSD: auth.h,v 1.15 2000/06/02 22:57:55 fvdl Exp $ */

/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2012-2017 Red Hat, Inc. and/or its affiliates.
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
 * from: @(#)auth.h 1.17 88/02/08 SMI
 * from: @(#)auth.h 2.3 88/08/07 4.0 RPCSRC
 * from: @(#)auth.h 1.43  98/02/02 SMI
 * $FreeBSD: src/include/rpc/auth.h,v 1.20 2003/01/01 18:48:42 schweikh Exp $
 */

/*
 * auth.h, Authentication interface.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * The data structures are completely opaque to the client.  The client
 * is required to pass an AUTH * to routines that create rpc
 * "sessions".
 */

#ifndef _TIRPC_AUTH_INLINE_H
#define _TIRPC_AUTH_INLINE_H

#include <rpc/xdr_inline.h>
#include <rpc/auth.h>

/*
 * encode auth opaque
 */
static inline bool
xdr_opaque_auth_encode_it(XDR *xdrs, struct opaque_auth *oa)
{
	if (oa->oa_length > MAX_AUTH_BYTES) {
		/* duplicate test, usually done earlier by caller */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_length (%u) > %u",
			__func__, __LINE__,
			oa->oa_length,
			MAX_AUTH_BYTES);
		return (false);
	}

	return (xdr_opaque_encode(xdrs, oa->oa_body, oa->oa_length));
}

/*
 * encode an auth message
 */
static inline bool
xdr_opaque_auth_encode(XDR *xdrs, struct opaque_auth *oa)
{
	/*
	 * XDR_INLINE is just as likely to do a function call,
	 * so don't bother with it here.
	 */
	if (!xdr_putenum(xdrs, oa->oa_flavor)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_flavor",
			__func__, __LINE__);
		return (false);
	}
	if (!xdr_putuint32(xdrs, oa->oa_length)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_length",
			__func__, __LINE__);
		return (false);
	}

	if (oa->oa_length) {
		/* only call and alloc for > 0 length */
		return (xdr_opaque_auth_encode_it(xdrs, oa));
	}
	return (true);	/* 0 length succeeds */
}

/*
 * decode auth opaque
 */
static inline bool
xdr_opaque_auth_decode_it(XDR *xdrs, struct opaque_auth *oa)
{
	if (oa->oa_length > MAX_AUTH_BYTES) {
		/* duplicate test, usually done earlier by caller */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_length (%u) > %u",
			__func__, __LINE__,
			oa->oa_length,
			MAX_AUTH_BYTES);
		return (false);
	}

	return (xdr_opaque_decode(xdrs, oa->oa_body, oa->oa_length));
}

/*
 * decode an auth message
 *
 * param[IN]	buf	2 more inline
 */
static inline bool
xdr_opaque_auth_decode(XDR *xdrs, struct opaque_auth *oa, int32_t *buf)
{
	if (buf != NULL) {
		oa->oa_flavor = IXDR_GET_ENUM(buf, enum_t);
		oa->oa_length = (u_int) IXDR_GET_U_INT32(buf);
	} else if (!xdr_getenum(xdrs, (enum_t *)&oa->oa_flavor)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_flavor",
			__func__, __LINE__);
		return (false);
	} else if (!xdr_getuint32(xdrs, &oa->oa_length)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR oa_length",
			__func__, __LINE__);
		return (false);
	}

	if (oa->oa_length) {
		/* only call and alloc for > 0 length */
		return xdr_opaque_auth_decode_it(xdrs, oa);
	}
	return (true);	/* 0 length succeeds */
}

/*
 * XDR an auth message
 */
static inline bool
xdr_opaque_auth(XDR *xdrs, struct opaque_auth *oa)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (xdr_opaque_auth_encode(xdrs, oa));
	case XDR_DECODE:
		return (xdr_opaque_auth_decode(xdrs, oa, NULL));
	case XDR_FREE:
		return (true);
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR xdrs->x_op (%u)",
			__func__, __LINE__,
			xdrs->x_op);
		break;
	}			/* x_op */

	return (false);
}

#endif				/* !_TIRPC_AUTH_INLINE_H */
