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
 * rpc_dplx.c
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>

#include <sys/select.h>

static const struct xdr_discrim reply_dscrm[3] = {
	{ (int)MSG_ACCEPTED, (xdrproc_t)xdr_accepted_reply },
	{ (int)MSG_DENIED, (xdrproc_t)xdr_rejected_reply },
	{ __dontcare__, NULL_xdrproc_t } };

/*
 * XDR a duplex call message
 */
bool_t
xdr_dplx_msg(xdrs, dmsg)
	XDR *xdrs;
	struct rpc_msg *dmsg;
{
	int32_t *buf;
	struct opaque_auth *oa;
        bool_t rslt;

	assert(xdrs != NULL);
	assert(dmsg != NULL);

	if (xdrs->x_op == XDR_ENCODE) {
            if (dmsg->rm_call.cb_cred.oa_length > MAX_AUTH_BYTES) {
                return (FALSE);
            }
            if (dmsg->rm_call.cb_verf.oa_length > MAX_AUTH_BYTES) {
                return (FALSE);
            }
            buf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT
                             + RNDUP(dmsg->rm_call.cb_cred.oa_length)
                             + 2 * BYTES_PER_XDR_UNIT
                             + RNDUP(dmsg->rm_call.cb_verf.oa_length));
            if (buf != NULL) {
                __warnx("%s XDR_ENCODE inline case", __func__);
                IXDR_PUT_INT32(buf, dmsg->rm_xid);
                IXDR_PUT_ENUM(buf, dmsg->rm_direction);
                switch (dmsg->rm_direction) {
                case CALL:
                    IXDR_PUT_INT32(buf, dmsg->rm_call.cb_rpcvers);
                    if (dmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION) {
                        return (FALSE);
                    }
                    IXDR_PUT_INT32(buf, dmsg->rm_call.cb_prog);
                    IXDR_PUT_INT32(buf, dmsg->rm_call.cb_vers);
                    IXDR_PUT_INT32(buf, dmsg->rm_call.cb_proc);
                    oa = &dmsg->rm_call.cb_cred;
                    IXDR_PUT_ENUM(buf, oa->oa_flavor);
                    IXDR_PUT_INT32(buf, oa->oa_length);
                    if (oa->oa_length) {
                        memmove(buf, oa->oa_base, oa->oa_length);
                        buf += RNDUP(oa->oa_length) / sizeof (int32_t);
                    }
                    oa = &dmsg->rm_call.cb_verf;
                    IXDR_PUT_ENUM(buf, oa->oa_flavor);
                    IXDR_PUT_INT32(buf, oa->oa_length);
                    if (oa->oa_length) {
                        memmove(buf, oa->oa_base, oa->oa_length);
                        /* no real need.... XXX next line uncommented (matt) */
                        buf += RNDUP(oa->oa_length) / sizeof (int32_t);
                    }
                    return (TRUE);
                    break;
                case REPLY:
                    __warnx("%s unexpected (duplex) REPLY", __func__);
                    return (xdr_union(xdrs,
                                      (enum_t *)&(dmsg->rm_reply.rp_stat),
                                      (caddr_t)(void *)&(dmsg->rm_reply.ru),
                                      reply_dscrm,
                                      NULL_xdrproc_t));
                    break;
                default:
                    /* unlikely */
                    return (FALSE);
                }
            } /* buf */
	} /* XDR_ENCODE */
	if (xdrs->x_op == XDR_DECODE) {
            buf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT);
            if (buf != NULL) {
                __warnx("%s XDR_DECODE inline case", __func__);
                dmsg->rm_xid = IXDR_GET_U_INT32(buf);
                dmsg->rm_direction = IXDR_GET_ENUM(buf, enum msg_type);
                switch (dmsg->rm_direction) {
                case CALL:
                    dmsg->rm_call.cb_rpcvers = IXDR_GET_U_INT32(buf);
                    if (dmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION) {
                        return (FALSE);
                    }
                    dmsg->rm_call.cb_prog = IXDR_GET_U_INT32(buf);
                    dmsg->rm_call.cb_vers = IXDR_GET_U_INT32(buf);
                    dmsg->rm_call.cb_proc = IXDR_GET_U_INT32(buf);
                    oa = &dmsg->rm_call.cb_cred;
                    oa->oa_flavor = IXDR_GET_ENUM(buf, enum_t);
                    oa->oa_length = (u_int)IXDR_GET_U_INT32(buf);
                    if (oa->oa_length) {
                        if (oa->oa_length > MAX_AUTH_BYTES) {
                            return (FALSE);
                        }
                        if (oa->oa_base == NULL) {
                            oa->oa_base = (caddr_t)
                                mem_alloc(oa->oa_length);
                            if (oa->oa_base == NULL)
                                return (FALSE);
                        }
                        buf = XDR_INLINE(xdrs, RNDUP(oa->oa_length));
                        if (buf == NULL) {
                            if (xdr_opaque(xdrs, oa->oa_base,
                                           oa->oa_length) == FALSE) {
                                return (FALSE);
                            }
                        } else {
                            memmove(oa->oa_base, buf,
                                    oa->oa_length);
                            /* no real need... XXX uncommented (matt) */
                            buf += RNDUP(oa->oa_length) / sizeof (int32_t);
                        }
                    }
                    oa = &dmsg->rm_call.cb_verf;
                    buf = XDR_INLINE(xdrs, 2 * BYTES_PER_XDR_UNIT);
                    if (buf == NULL) {
                        if (xdr_enum(xdrs, &oa->oa_flavor) == FALSE ||
                            xdr_u_int(xdrs, &oa->oa_length) == FALSE) {
                            return (FALSE);
                        }
                    } else {
                        oa->oa_flavor = IXDR_GET_ENUM(buf, enum_t);
                        oa->oa_length = (u_int)IXDR_GET_U_INT32(buf);
                    }
                    if (oa->oa_length) {
                        if (oa->oa_length > MAX_AUTH_BYTES) {
                            return (FALSE);
                        }
                        if (oa->oa_base == NULL) {
                            oa->oa_base = (caddr_t)
                                mem_alloc(oa->oa_length);
                            if (oa->oa_base == NULL)
                                return (FALSE);
                        }
                        buf = XDR_INLINE(xdrs, RNDUP(oa->oa_length));
                        if (buf == NULL) {
                            if (xdr_opaque(xdrs, oa->oa_base,
                                           oa->oa_length) == FALSE) {
                                return (FALSE);
                            }
                        } else {
                            memmove(oa->oa_base, buf,
                                    oa->oa_length);
                            /* no real need... */
                            buf += RNDUP(oa->oa_length) / sizeof (int32_t);
                        }
                    }
                    return (TRUE);
                    break;
                case REPLY:
                    __warnx("%s non-inline (duplex) REPLY 1", __func__);
                    return (xdr_union(xdrs,
                                      (enum_t *)&(dmsg->rm_reply.rp_stat),
                                      (caddr_t)(void *)&(dmsg->rm_reply.ru),
                                      reply_dscrm,
                                      NULL_xdrproc_t));

                    break;
                default:
                    /* unlikely */
                    return (FALSE);
                }
            } /* buf */
	} /* XDR_DECODE */
	if (
	    xdr_u_int32_t(xdrs, &(dmsg->rm_xid)) &&
	    xdr_enum(xdrs, (enum_t *)&(dmsg->rm_direction))) {
                switch (dmsg->rm_direction) {
                case CALL:
                    if (
                        xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_rpcvers)) &&
                        (dmsg->rm_call.cb_rpcvers == RPC_MSG_VERSION) &&
                        xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_prog)) &&
                        xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_vers)) &&
                        xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_proc)) &&
                        xdr_opaque_auth(xdrs, &(dmsg->rm_call.cb_cred)) )
                        return (
                            xdr_opaque_auth(xdrs,
                                            &(dmsg->rm_call.cb_verf)));
                    break;
                case REPLY:
                    __warnx("%s non-inline (duplex) REPLY 2", __func__);
                    return (
                        xdr_union(xdrs,
                                  (enum_t *)&(dmsg->rm_reply.rp_stat),
                                  (caddr_t)(void *)&(dmsg->rm_reply.ru),
                                  reply_dscrm,
                                  NULL_xdrproc_t));
                default:
                    /* unlikely */
                    return (FALSE);
                }
            }
            return (FALSE);       
}
