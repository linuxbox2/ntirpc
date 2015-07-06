/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
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

/*
 * rpc_callmsg.c
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 */

#include <config.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>

#include <sys/select.h>

/* in glibc 2.14+ x86_64, memcpy no longer tries to handle overlapping areas,
 * see Fedora Bug 691336 (NOTABUG); we dont permit overlapping segments,
 * so memcpy may be a small win over memmove.
 */

/*
 * encode a call message, log error messages
 */
bool
xdr_call_encode(XDR *xdrs, struct rpc_msg *cmsg)
{
	struct opaque_auth *oa;
	int32_t *buf;

	if (cmsg->rm_call.cb_cred.oa_length > MAX_AUTH_BYTES) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR cb_cred.oa_length (%u) > %u",
			__func__, __LINE__,
			cmsg->rm_call.cb_cred.oa_length,
			MAX_AUTH_BYTES);
		return (false);
	}
	if (cmsg->rm_call.cb_verf.oa_length > MAX_AUTH_BYTES) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR cb_verf.oa_length (%u) > %u",
			__func__, __LINE__,
			cmsg->rm_call.cb_verf.oa_length,
			MAX_AUTH_BYTES);
		return (false);
	}
	buf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT
			+ RNDUP(cmsg->rm_call.cb_cred.oa_length)
			+ 2 * BYTES_PER_XDR_UNIT
			+ RNDUP(cmsg->rm_call.cb_verf.oa_length));
	if (buf != NULL) {
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u INLINE",
			__func__, __LINE__);
		IXDR_PUT_INT32(buf, cmsg->rm_xid);
		IXDR_PUT_ENUM(buf, cmsg->rm_direction);
		IXDR_PUT_INT32(buf, cmsg->rm_call.cb_rpcvers);
		if (cmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_rpcvers %u != %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_rpcvers,
				RPC_MSG_VERSION);
			return (false);
		}
		IXDR_PUT_INT32(buf, cmsg->rm_call.cb_prog);
		IXDR_PUT_INT32(buf, cmsg->rm_call.cb_vers);
		IXDR_PUT_INT32(buf, cmsg->rm_call.cb_proc);
		oa = &cmsg->rm_call.cb_cred;
		IXDR_PUT_ENUM(buf, oa->oa_flavor);
		IXDR_PUT_INT32(buf, oa->oa_length);
		if (oa->oa_length) {
			memcpy(buf, oa->oa_base, oa->oa_length);
			buf += RNDUP(oa->oa_length) / sizeof (int32_t);
		}
		oa = &cmsg->rm_call.cb_verf;
		IXDR_PUT_ENUM(buf, oa->oa_flavor);
		IXDR_PUT_INT32(buf, oa->oa_length);
		if (oa->oa_length) {
			memcpy(buf, oa->oa_base, oa->oa_length);
			/* no real need....
			buf += RNDUP(oa->oa_length) / sizeof (int32_t);
			*/
		}
	} else {
		/* nTI-RPC handles multiple buffers */
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u non-INLINE",
			__func__, __LINE__);
		if (!xdr_putuint32(xdrs, &(cmsg->rm_xid))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_xid %u",
				__func__, __LINE__,
				cmsg->rm_xid);
			return (false);
		}
		if (!xdr_putenum(xdrs, cmsg->rm_direction)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_direction %u",
				__func__, __LINE__,
				cmsg->rm_direction);
			return (false);
		}
		if (!xdr_putuint32(xdrs, &(cmsg->rm_call.cb_rpcvers))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_rpcvers %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_rpcvers);
			return (false);
		}
		if (cmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_rpcvers %u != %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_rpcvers,
				RPC_MSG_VERSION);
			return (false);
		}
		if (!xdr_putuint32(xdrs, &(cmsg->rm_call.cb_prog))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_prog %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_prog);
			return (false);
		}
		if (!xdr_putuint32(xdrs, &(cmsg->rm_call.cb_vers))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_vers %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_vers);
			return (false);
		}
		if (!xdr_putuint32(xdrs, &(cmsg->rm_call.cb_proc))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_proc %u",
				__func__, __LINE__,
				cmsg->rm_call.cb_proc);
			return (false);
		}
		if (!inline_auth_encode(xdrs, &(cmsg->rm_call.cb_cred))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			return (false);
		}
		if (!inline_auth_encode(xdrs, &(cmsg->rm_call.cb_verf))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			return (false);
		}
	}
	return (true);
}

/*
 * decode a call message, log error messages
 *
 * param[IN]	buf	3 more inline
 */
bool
xdr_call_decode(XDR *xdrs, struct rpc_msg *cmsg, int32_t *buf)
{
	if (buf != NULL) {
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u INLINE",
			__func__, __LINE__);
		cmsg->rm_call.cb_rpcvers = IXDR_GET_U_INT32(buf);
	} else {
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u non-INLINE",
			__func__, __LINE__);
		if (!xdr_getuint32(xdrs, &cmsg->rm_call.cb_rpcvers)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_call.cb_rpcvers",
				__func__, __LINE__);
			return (false);
		}
	}
	if (cmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR rm_call.cb_rpcvers %u != %u",
			__func__, __LINE__,
			cmsg->rm_call.cb_rpcvers,
			RPC_MSG_VERSION);
		return (false);
	}

	if (buf != NULL) {
		cmsg->rm_call.cb_prog = IXDR_GET_U_INT32(buf);
		cmsg->rm_call.cb_vers = IXDR_GET_U_INT32(buf);
	} else if (!xdr_getuint32(xdrs, &(cmsg->rm_call.cb_prog))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR rm_call.cb_prog",
			__func__, __LINE__);
		return (false);
	} else if (!xdr_getuint32(xdrs, &(cmsg->rm_call.cb_vers))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR rm_call.cb_vers",
			__func__, __LINE__);
		return (false);
	}

	buf = XDR_INLINE(xdrs, 3 * BYTES_PER_XDR_UNIT);
	if (buf != NULL) {
		cmsg->rm_call.cb_proc = IXDR_GET_U_INT32(buf);
		if (!inline_auth_decode(xdrs, &(cmsg->rm_call.cb_cred), buf)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			return (false);
		}
	} else if (!xdr_getuint32(xdrs, &(cmsg->rm_call.cb_proc))) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR rm_call.cb_proc",
			__func__, __LINE__);
		return (false);
	} else if (!inline_auth_decode(xdrs, &(cmsg->rm_call.cb_cred), NULL)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return (false);
	}

	if (!inline_auth_decode(xdrs, &(cmsg->rm_call.cb_verf), NULL)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return (false);
	}
	return (true);
}

/*
 * XDR a call message, log error messages
 */
bool
xdr_ncallmsg(XDR *xdrs, struct rpc_msg *cmsg)
{
	assert(xdrs != NULL);
	assert(cmsg != NULL);

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		if (cmsg->rm_direction != CALL) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR cmsg->rm_direction %u != %u",
				__func__, __LINE__,
				cmsg->rm_direction,
				CALL);
			return (false);
		}
		return (xdr_call_encode(xdrs, cmsg));
	case XDR_DECODE:
		if (xdr_dplx_decode(xdrs, cmsg)) {
			if (cmsg->rm_direction != CALL) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR cmsg->rm_direction %u != %u",
					__func__, __LINE__,
					cmsg->rm_direction,
					CALL);
				return (false);
			}
			return (true);
		}
		break;
	case XDR_FREE:
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u xdrs->x_op XDR_FREE",
			__func__, __LINE__);
		return (true);
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR xdrs->x_op (%u)",
			__func__, __LINE__,
			xdrs->x_op);
		break;
	}

	return (false);
}					/* xdr_ncallmsg */
