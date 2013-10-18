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
 * rpc_dplx_msg.c
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 */

#include <config.h>
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

/*
 * XDR a duplex call message
 */
bool
xdr_dplx_msg(XDR *xdrs, struct rpc_msg *dmsg)
{
	int32_t *buf;
	struct opaque_auth *oa;

	assert(xdrs != NULL);
	assert(dmsg != NULL);

	if (xdrs->x_op == XDR_ENCODE) {
		if (dmsg->rm_call.cb_cred.oa_length > MAX_AUTH_BYTES)
			return (false);
		if (dmsg->rm_call.cb_verf.oa_length > MAX_AUTH_BYTES)
			return (false);
		buf =
		    XDR_INLINE(xdrs,
			       8 * BYTES_PER_XDR_UNIT +
			       RNDUP(dmsg->rm_call.cb_cred.oa_length)
			       + 2 * BYTES_PER_XDR_UNIT +
			       RNDUP(dmsg->rm_call.cb_verf.oa_length));
		if (buf != NULL) {
			IXDR_PUT_INT32(buf, dmsg->rm_xid);
			IXDR_PUT_ENUM(buf, dmsg->rm_direction);
			switch (dmsg->rm_direction) {
			case CALL:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s 1: XDR_ENCODE INLINE CALL",
					__func__);
				IXDR_PUT_INT32(buf, dmsg->rm_call.cb_rpcvers);
				if (dmsg->rm_call.cb_rpcvers !=
				    RPC_MSG_VERSION) {
					return (false);
				}
				IXDR_PUT_INT32(buf, dmsg->rm_call.cb_prog);
				IXDR_PUT_INT32(buf, dmsg->rm_call.cb_vers);
				IXDR_PUT_INT32(buf, dmsg->rm_call.cb_proc);
				oa = &dmsg->rm_call.cb_cred;
				IXDR_PUT_ENUM(buf, oa->oa_flavor);
				IXDR_PUT_INT32(buf, oa->oa_length);
				if (oa->oa_length) {
					memmove(buf, oa->oa_base,
						oa->oa_length);
					buf +=
					    RNDUP(oa->oa_length) /
					    sizeof(int32_t);
				}
				oa = &dmsg->rm_call.cb_verf;
				IXDR_PUT_ENUM(buf, oa->oa_flavor);
				IXDR_PUT_INT32(buf, oa->oa_length);
				if (oa->oa_length) {
					memmove(buf, oa->oa_base,
						oa->oa_length);
					/* no real need.... XXX next line
					 * uncommented (matt) */
					buf +=
					    RNDUP(oa->oa_length) /
					    sizeof(int32_t);
				}
				return (true);
				break;
			case REPLY:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s 2: INLINE un impl. XDR_ENCODE REPLY",
					__func__);
				return (inline_xdr_union
					(xdrs,
					 (enum_t *) &(dmsg->rm_reply.rp_stat),
					 (caddr_t) (void *)&(dmsg->rm_reply.ru),
					 reply_dscrm, NULL_xdrproc_t));
				break;
			default:
				/* unlikely */
				return (false);
			}
		} else {
			/* ! inline */
			if (inline_xdr_u_int32_t(xdrs, &(dmsg->rm_xid))
			    && inline_xdr_enum(xdrs,
					       (enum_t *) &(dmsg->
							     rm_direction))) {
				switch (dmsg->rm_direction) {
				case CALL:
					__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
						"%s 3: non-INLINE inline XDR_ENCODE CALL",
						__func__);
					if (inline_xdr_u_int32_t
					    (xdrs, &(dmsg->rm_call.cb_rpcvers))
					    && (dmsg->rm_call.cb_rpcvers ==
						RPC_MSG_VERSION)
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_prog))
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_vers))
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_proc))
					    && inline_xdr_opaque_auth(
						    xdrs,
						    &(dmsg->rm_call.cb_cred)))
						return (inline_xdr_opaque_auth
							(xdrs,
							 &(dmsg->rm_call.
							   cb_verf)));
					break;
				case REPLY:
					__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
						"%s 4: non INLINE need inline impl. "
						"XDR_ENCODE REPLY", __func__);
					return (inline_xdr_union
						(xdrs,
						 (enum_t *) &(dmsg->rm_reply.
							       rp_stat),
						 (caddr_t) (void *)
						 &(dmsg->rm_reply.ru),
						 reply_dscrm, NULL_xdrproc_t));
				default:
					/* unlikely */
					return (false);
				}
			}
		}
	}			/* XDR_ENCODE */
	if (xdrs->x_op == XDR_DECODE) {
		buf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT);
		if (buf != NULL) {
			dmsg->rm_xid = IXDR_GET_U_INT32(buf);
			dmsg->rm_direction = IXDR_GET_ENUM(buf, enum msg_type);
			switch (dmsg->rm_direction) {
			case CALL:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s 5: XDR_DECODE INLINE CALL",
					__func__);
				dmsg->rm_call.cb_rpcvers =
				    IXDR_GET_U_INT32(buf);
				if (dmsg->rm_call.cb_rpcvers !=
				    RPC_MSG_VERSION) {
					return (false);
				}
				dmsg->rm_call.cb_prog = IXDR_GET_U_INT32(buf);
				dmsg->rm_call.cb_vers = IXDR_GET_U_INT32(buf);
				dmsg->rm_call.cb_proc = IXDR_GET_U_INT32(buf);
				oa = &dmsg->rm_call.cb_cred;
				oa->oa_flavor = IXDR_GET_ENUM(buf, enum_t);
				oa->oa_length = (u_int) IXDR_GET_U_INT32(buf);
				if (oa->oa_length) {
					if (oa->oa_length > MAX_AUTH_BYTES)
						return (false);
					if (oa->oa_base == NULL) {
						oa->oa_base = (caddr_t)
						    mem_alloc(oa->oa_length);
						if (oa->oa_base == NULL)
							return (false);
					}
					buf =
					    XDR_INLINE(xdrs,
						       RNDUP(oa->oa_length));
					if (buf == NULL) {
						if (inline_xdr_opaque
						    (xdrs, oa->oa_base,
						     oa->oa_length) == false) {
							return (false);
						}
					} else {
						memmove(oa->oa_base, buf,
							oa->oa_length);
						/* no real need...
						 * XXX uncommented (matt) */
						buf +=
						    RNDUP(oa->oa_length) /
						    sizeof(int32_t);
					}
				}
				oa = &dmsg->rm_call.cb_verf;
				buf = XDR_INLINE(xdrs, 2 * BYTES_PER_XDR_UNIT);
				if (buf == NULL) {
					if (inline_xdr_enum
					    (xdrs, &oa->oa_flavor) == false
					    || inline_xdr_u_int(xdrs,
								&oa->oa_length)
					    == false) {
						return (false);
					}
				} else {
					oa->oa_flavor =
					    IXDR_GET_ENUM(buf, enum_t);
					oa->oa_length =
					    (u_int) IXDR_GET_U_INT32(buf);
				}
				if (oa->oa_length) {
					if (oa->oa_length > MAX_AUTH_BYTES)
						return (false);
					if (oa->oa_base == NULL) {
						oa->oa_base = (caddr_t)
						    mem_alloc(oa->oa_length);
						if (oa->oa_base == NULL)
							return (false);
					}
					buf =
					    XDR_INLINE(xdrs,
						       RNDUP(oa->oa_length));
					if (buf == NULL) {
						if (inline_xdr_opaque
						    (xdrs, oa->oa_base,
						     oa->oa_length) == false) {
							return (false);
						}
					} else {
						memmove(oa->oa_base, buf,
							oa->oa_length);
						/* no real need... */
						buf +=
						    RNDUP(oa->oa_length) /
						    sizeof(int32_t);
					}
				}
				return (true);
				break;
			case REPLY:
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s 6: INLINE XDR_ENCODE REPLY (not properly "
					"INLINE, not reached?)", __func__);
				return (inline_xdr_union
					(xdrs,
					 (enum_t *) &(dmsg->rm_reply.rp_stat),
					 (caddr_t) (void *)&(dmsg->rm_reply.ru),
					 reply_dscrm, NULL_xdrproc_t));

				break;
			default:
				/* unlikely */
				__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
					"%s: dmsg->rm_xid %u", __func__,
					dmsg->rm_xid);
				return (false);
			}
		} else {
			/* ! inline */
			if (inline_xdr_u_int32_t(xdrs, &(dmsg->rm_xid))
			    && inline_xdr_enum(xdrs,
					       (enum_t *) &(dmsg->
							     rm_direction))) {
				switch (dmsg->rm_direction) {
				case CALL:
					__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
						"%s 7: non-inline CALL",
						__func__);
					if (inline_xdr_u_int32_t
					    (xdrs, &(dmsg->rm_call.cb_rpcvers))
					    && (dmsg->rm_call.cb_rpcvers ==
						RPC_MSG_VERSION)
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_prog))
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_vers))
					    && inline_xdr_u_int32_t(xdrs,
								    &(dmsg->
								      rm_call.
								      cb_proc))
					    && inline_xdr_opaque_auth(
						    xdrs,
						    &(dmsg->rm_call.cb_cred)))
						return (inline_xdr_opaque_auth
							(xdrs,
							 &(dmsg->rm_call.
							   cb_verf)));
					break;
				case REPLY:
					__warnx(TIRPC_DEBUG_FLAG_RPC_MSG,
						"%s 8: non-INLINE REPLY",
						__func__);
					if (!inline_xdr_enum
					    (xdrs,
					     (enum_t *) &(dmsg->rm_reply.
							  rp_stat)))
						return (false);
					switch (dmsg->rm_reply.rp_stat) {
					case MSG_ACCEPTED:
						{
							struct accepted_reply
							*ar =
							    (struct
							     accepted_reply *)
							    &(dmsg->rm_reply.
							      ru);
							if (!inline_xdr_opaque_auth(xdrs, &(ar->ar_verf)))
								return (false);
							if (!inline_xdr_enum
							    (xdrs,
							     (enum_t *) & (ar->
									   ar_stat)))
								return (false);
							switch (ar->ar_stat) {
							case SUCCESS:
								return ((*
									 (ar->
									  ar_results.
									  proc))
									(xdrs,
									 &(ar->
									   ar_results.
									   where)));

							case PROG_MISMATCH:
								if (!inline_xdr_u_int32_t(xdrs, &(ar->ar_vers.low)))
									return
									    (false);
								return
								    (inline_xdr_u_int32_t
								     (xdrs,
								      &(ar->
									ar_vers.
									high)));

							case GARBAGE_ARGS:
							case SYSTEM_ERR:
							case PROC_UNAVAIL:
							case PROG_UNAVAIL:
								/* true */
								break;
							default:
								break;
							}	/* ar_stat */
							return (true);
						}	/* MSG_ACCEPTED */
						break;
					case MSG_DENIED:
						{
							/* XXX branch not verified */
							__warnx
							    (TIRPC_DEBUG_FLAG_RPC_MSG,
							     "non-inline MSG_DENIED not verified");
							struct rejected_reply
							*rr =
							    (struct
							     rejected_reply *)
							    &(dmsg->rm_reply.
							      ru);
							if (!inline_xdr_enum
							    (xdrs,
							     (enum_t *) & (rr->
									   rj_stat)))
								return (false);
							switch (rr->rj_stat) {

							case RPC_MISMATCH:
								if (!inline_xdr_u_int32_t(xdrs, &(rr->rj_vers.low)))
									return
									    (false);
								return
								    (inline_xdr_u_int32_t
								     (xdrs,
								      &(rr->
									rj_vers.
									high)));

							case AUTH_ERROR:
								return
								    (inline_xdr_enum
								     (xdrs,
								      (enum_t *)
								      &(rr->
									rj_why)));
							}
							return (true);
						}
						break;
					default:
						return (false);
						break;
					}	/* rm_reply.rp_stat */
				default:
					/* unlikely */
					return (false);
				}
			}
		}
	}			/* XDR_DECODE */
	return (false);
}				/* new */
