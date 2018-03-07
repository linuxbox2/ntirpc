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
 */

/*
 * rpc_dplx_msg.c
 *
 * Based upon/copied from:
 * rpc_callmsg.c
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 */

#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <reentrant.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>

static const struct xdr_discrim reply_dscrm[3] = {
	{(int)MSG_ACCEPTED, (xdrproc_t) xdr_naccepted_reply},
	{(int)MSG_DENIED, (xdrproc_t) xdr_nrejected_reply},
	{__dontcare__, NULL_xdrproc_t}
};

/* in glibc 2.14+ x86_64, memcpy no longer tries to handle overlapping areas,
 * see Fedora Bug 691336 (NOTABUG); we dont permit overlapping segments,
 * so memcpy may be a small win over memmove.
 */

/*
 * encode a reply message, log error messages
 */
bool
xdr_reply_encode(XDR *xdrs, struct rpc_msg *dmsg)
{
	struct opaque_auth *oa;
	int32_t *buf;

	switch (dmsg->rm_reply.rp_stat) {
	case MSG_ACCEPTED:
	{
		struct accepted_reply *ar = (struct accepted_reply *)
						&(dmsg->rm_reply.ru);

		oa = &ar->ar_verf;
		if (oa->oa_length > MAX_AUTH_BYTES) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR ar_verf.oa_length (%u) > %u",
				__func__, __LINE__,
				oa->oa_length,
				MAX_AUTH_BYTES);
			return (false);
		}
		buf = xdr_inline_encode(xdrs, 6 * BYTES_PER_XDR_UNIT
						+ RNDUP(oa->oa_length));

		if (buf != NULL) {
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u ACCEPTED INLINE",
				__func__, __LINE__);
			IXDR_PUT_INT32(buf, dmsg->rm_xid);
			IXDR_PUT_ENUM(buf, dmsg->rm_direction);
			IXDR_PUT_ENUM(buf, dmsg->rm_reply.rp_stat);
			IXDR_PUT_ENUM(buf, oa->oa_flavor);
			IXDR_PUT_INT32(buf, oa->oa_length);
			if (oa->oa_length) {
				memcpy(buf, oa->oa_body, oa->oa_length);
				buf += RNDUP(oa->oa_length) / sizeof(int32_t);
			}

			IXDR_PUT_ENUM(buf, ar->ar_stat);
			switch (ar->ar_stat) {
			case SUCCESS:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s:%u SUCCESS",
					__func__, __LINE__);
				return (true);

			case PROG_MISMATCH:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s:%u MISMATCH",
					__func__, __LINE__);
				buf = xdr_inline_encode(xdrs,
							2 * BYTES_PER_XDR_UNIT);
				if (buf != NULL) {
					IXDR_PUT_ENUM(buf, ar->ar_vers.low);
					IXDR_PUT_ENUM(buf, ar->ar_vers.high);
				} else if (!xdr_putuint32(xdrs,
							  ar->ar_vers.low)) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s:%u ERROR ar_vers.low %u",
						__func__, __LINE__,
						ar->ar_vers.low);
					return (false);
				} else if (!xdr_putuint32(xdrs,
							  ar->ar_vers.high)) {
					__warnx(TIRPC_DEBUG_FLAG_ERROR,
						"%s:%u ERROR ar_vers.high %u",
						__func__, __LINE__,
						ar->ar_vers.high);
					return (false);
				}
				/* fallthru */
			case GARBAGE_ARGS:
			case SYSTEM_ERR:
			case PROC_UNAVAIL:
			case PROG_UNAVAIL:
				break;
			};
			return (true);
		}

		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u ACCEPTED non-INLINE",
			__func__, __LINE__);
		if (!xdr_putuint32(xdrs, dmsg->rm_xid)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_xid %u",
				__func__, __LINE__,
				dmsg->rm_xid);
			return (false);
		}
		if (!xdr_putenum(xdrs, dmsg->rm_direction)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_direction %u",
				__func__, __LINE__,
				dmsg->rm_direction);
			return (false);
		}
		if (!inline_xdr_union(xdrs,
				      (enum_t *) &(dmsg->rm_reply.rp_stat),
				      (void *)&(dmsg->rm_reply.ru),
				      reply_dscrm, NULL_xdrproc_t)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR inline_xdr_union",
				__func__, __LINE__);
			return (false);
		}
		return (true);
	}
	case MSG_DENIED:
	{
		struct rejected_reply *rr = (struct rejected_reply *)
						&(dmsg->rm_reply.ru);

		buf = xdr_inline_encode(xdrs, 3 * BYTES_PER_XDR_UNIT);
		if (buf != NULL) {
			IXDR_PUT_INT32(buf, dmsg->rm_xid);
			IXDR_PUT_ENUM(buf, dmsg->rm_direction);
			IXDR_PUT_ENUM(buf, dmsg->rm_reply.rp_stat);
		} else if (!xdr_putuint32(xdrs, dmsg->rm_xid)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_xid %u",
				__func__, __LINE__,
				dmsg->rm_xid);
			return (false);
		} else if (!xdr_putenum(xdrs, dmsg->rm_direction)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_direction %u",
				__func__, __LINE__,
				dmsg->rm_direction);
			return (false);
		} else if (!xdr_putenum(xdrs, dmsg->rm_reply.rp_stat)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rp_stat %u",
				__func__, __LINE__,
				dmsg->rm_reply.rp_stat);
			return (false);
		}

		switch (rr->rj_stat) {
		case RPC_MISMATCH:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u DENIED MISMATCH",
				__func__, __LINE__);
			buf = xdr_inline_encode(xdrs, 3 * BYTES_PER_XDR_UNIT);

			if (buf != NULL) {
				IXDR_PUT_ENUM(buf, rr->rj_stat);
				IXDR_PUT_U_INT32(buf, rr->rj_vers.low);
				IXDR_PUT_U_INT32(buf, rr->rj_vers.high);
			} else if (!xdr_putenum(xdrs, rr->rj_stat)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_stat %u",
					__func__, __LINE__,
					rr->rj_stat);
				return (false);
			} else if (!xdr_putuint32(xdrs, rr->rj_vers.low)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_vers.low %u",
					__func__, __LINE__,
					rr->rj_vers.low);
				return (false);
			} else if (!xdr_putuint32(xdrs, rr->rj_vers.high)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_vers.high %u",
					__func__, __LINE__,
					rr->rj_vers.high);
				return (false);
			}
			return (true); /* bugfix */
		case AUTH_ERROR:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u DENIED AUTH",
				__func__, __LINE__);
			buf = xdr_inline_encode(xdrs, 2 * BYTES_PER_XDR_UNIT);

			if (buf != NULL) {
				IXDR_PUT_ENUM(buf, rr->rj_stat);
				IXDR_PUT_ENUM(buf, rr->rj_why);
			} else if (!xdr_putenum(xdrs, rr->rj_stat)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_stat %u",
					__func__, __LINE__,
					rr->rj_stat);
				return (false);
			} else if (!xdr_putenum(xdrs, rr->rj_why)) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_why %u",
					__func__, __LINE__,
					rr->rj_why);
				return (false);
			}
			return (true); /* bugfix */
		default:
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rr->rj_stat (%u)",
				__func__, __LINE__,
				rr->rj_stat);
			break;
		};
		break;
	}
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR dmsg->rm_reply.rp_stat (%u)",
			__func__, __LINE__,
			dmsg->rm_reply.rp_stat);
		break;
	};

	return (false);
}			/* XDR_ENCODE */

/*
 * decode a reply message
 *
 * param[IN]	buf	3 more inline
 */
bool
xdr_reply_decode(XDR *xdrs, struct rpc_msg *dmsg, int32_t *buf)
{
	if (buf != NULL) {
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u INLINE",
			__func__, __LINE__);
		dmsg->rm_reply.rp_stat = IXDR_GET_ENUM(buf, enum_t);
	} else {
		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u non-INLINE",
			__func__, __LINE__);
		if (!xdr_getenum(xdrs, (enum_t *)&(dmsg->rm_reply.rp_stat))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_reply.rp_stat",
				__func__, __LINE__);
			return (false);
		}
	}

	switch (dmsg->rm_reply.rp_stat) {
	case MSG_ACCEPTED:
	{
		struct accepted_reply *ar = (struct accepted_reply *)
						&(dmsg->rm_reply.ru);

		if (!xdr_opaque_auth_decode(xdrs, &ar->ar_verf, buf)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR (return)",
				__func__, __LINE__);
			return (false);
		}

		if (!xdr_getenum(xdrs, (enum_t *)&(ar->ar_stat))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR ar_stat",
				__func__, __LINE__);
			return (false);
		}

		switch (ar->ar_stat) {
		case SUCCESS:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u SUCCESS",
				__func__, __LINE__);
			return (true);

		case PROG_MISMATCH:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u MISMATCH",
				__func__, __LINE__);
			if (!xdr_getuint32(xdrs, &(ar->ar_vers.low))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR ar_vers.low",
					__func__, __LINE__);
				return (false);
			}
			if (!xdr_getuint32(xdrs, &(ar->ar_vers.high))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR ar_vers.high",
					__func__, __LINE__);
				return (false);
			}

		case GARBAGE_ARGS:
		case SYSTEM_ERR:
		case PROC_UNAVAIL:
		case PROG_UNAVAIL:
			/* true */
			break;
		default:
			break;
		};	/* ar_stat */
		return (true);
	}	/* MSG_ACCEPTED */
	case MSG_DENIED:
	{
		/* XXX branch not verified */
		struct rejected_reply *rr = (struct rejected_reply *)
						&(dmsg->rm_reply.ru);

		__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
			"%s:%u MSG_DENIED not verified",
			__func__, __LINE__);

		if (buf != NULL) {
			rr->rj_stat = IXDR_GET_ENUM(buf, enum_t);
		} else if (!xdr_getenum(xdrs, (enum_t *)&(rr->rj_stat))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rj_stat",
				__func__, __LINE__);
			return (false);
		}

		switch (rr->rj_stat) {
		case RPC_MISMATCH:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u DENIED MISMATCH",
				__func__, __LINE__);
			if (buf != NULL) {
				rr->rj_vers.low = IXDR_GET_U_INT32(buf);
			} else if (!xdr_getuint32(xdrs, &(rr->rj_vers.low))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_vers.low",
					__func__, __LINE__);
				return (false);
			}
			if (!xdr_getuint32(xdrs, &(rr->rj_vers.high))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_vers.high",
					__func__, __LINE__);
				return (false);
			}
			break;

		case AUTH_ERROR:
			__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
				"%s:%u DENIED AUTH",
				__func__, __LINE__);
			if (buf != NULL) {
				rr->rj_why = IXDR_GET_ENUM(buf, enum_t);
			} else if (!xdr_getenum(xdrs,
					(enum_t *)&(rr->rj_why))) {
				__warnx(TIRPC_DEBUG_FLAG_ERROR,
					"%s:%u ERROR rj_why",
					__func__, __LINE__);
				return (false);
			}
			break;
		};
		return (true);
	}	/* MSG_DENIED */
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR dmsg->rm_reply.rp_stat %u",
			__func__, __LINE__,
			dmsg->rm_reply.rp_stat);
		break;
	};	/* rm_reply.rp_stat */

	return (false);
}

/*
 * decode a duplex message, log error messages
 */
bool
xdr_dplx_decode(XDR *xdrs, struct rpc_msg *dmsg)
{
	int32_t *buf;

	/*
	 * NOTE: 5 here, 3 more in each _decode
	 */
	buf = xdr_inline_decode(xdrs, 5 * BYTES_PER_XDR_UNIT);
	if (buf != NULL) {
		dmsg->rm_xid = IXDR_GET_U_INT32(buf);
		dmsg->rm_direction = IXDR_GET_ENUM(buf, enum msg_type);
	} else {
		if (!xdr_getuint32(xdrs, &(dmsg->rm_xid))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_xid",
				__func__, __LINE__);
			return (false);
		}
		if (!xdr_getenum(xdrs, (enum_t *)&(dmsg->rm_direction))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR rm_direction",
				__func__, __LINE__);
			return (false);
		}
	}

	switch (dmsg->rm_direction) {
	case CALL:
		return (xdr_call_decode(xdrs, dmsg, buf));
	case REPLY:
		return (xdr_reply_decode(xdrs, dmsg, buf));
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR dmsg->rm_direction (%u)",
			__func__, __LINE__,
			dmsg->rm_direction);
		break;
	};

	return (false);
}

/*
 * XDR a duplex message
 */
bool
xdr_dplx_msg(XDR *xdrs, struct rpc_msg *dmsg)
{
	assert(xdrs != NULL);
	assert(dmsg != NULL);

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		switch (dmsg->rm_direction) {
		case CALL:
			return (xdr_call_encode(xdrs, dmsg));
		case REPLY:
			return (xdr_reply_encode(xdrs, dmsg));
		default:
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR dmsg->rm_direction (%u)",
				__func__, __LINE__,
				dmsg->rm_direction);
			break;
		};
		break;
	case XDR_DECODE:
		return (xdr_dplx_decode(xdrs, dmsg));
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
	};

	return (false);
}					/* xdr_dplx_msg */
