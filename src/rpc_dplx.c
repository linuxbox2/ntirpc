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
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>

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
                IXDR_PUT_INT32(buf, dmsg->rm_xid);
                IXDR_PUT_ENUM(buf, dmsg->rm_direction);
                switch (dmsg->rm_direction) {
                case CALL:
                    __warnx("%s 1: XDR_ENCODE INLINE CALL", __func__);
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
                    __warnx("%s 2: INLINE un impl. XDR_ENCODE REPLY", __func__);
                    return (inline_xdr_union(
                                xdrs, (enum_t *)&(dmsg->rm_reply.rp_stat),
                                (caddr_t)(void *)&(dmsg->rm_reply.ru),
                                reply_dscrm,
                                NULL_xdrproc_t));
                    break;
                default:
                    /* unlikely */
                    return (FALSE);
                }
            } else {
                /* ! inline */
                if (
                    inline_xdr_u_int32_t(xdrs, &(dmsg->rm_xid)) &&
                    inline_xdr_enum(xdrs, (enum_t *)&(dmsg->rm_direction))) {
                    switch (dmsg->rm_direction) {
                    case CALL:
                        __warnx("%s 3: non-INLINE inline XDR_ENCODE CALL",
                                __func__);
                    if (
                        inline_xdr_u_int32_t(
                            xdrs, &(dmsg->rm_call.cb_rpcvers)) &&
                        (dmsg->rm_call.cb_rpcvers == RPC_MSG_VERSION) &&
                        inline_xdr_u_int32_t(
                            xdrs, &(dmsg->rm_call.cb_prog)) &&
                        inline_xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_vers)) &&
                        inline_xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_proc)) &&
                        inline_xdr_opaque_auth(
                            xdrs, &(dmsg->rm_call.cb_cred)) )
                        return (
                            inline_xdr_opaque_auth(
                                xdrs, &(dmsg->rm_call.cb_verf)));
                    break;
                case REPLY:
                    __warnx("%s 4: non INLINE need inline impl. "
                            "XDR_ENCODE REPLY",
                            __func__);
                    return (
                        inline_xdr_union(
                            xdrs,
                            (enum_t *)&(dmsg->rm_reply.rp_stat),
                            (caddr_t)(void *)&(dmsg->rm_reply.ru),
                            reply_dscrm,
                            NULL_xdrproc_t));
                    default:
                        /* unlikely */
                    return (FALSE);
                    }
                }
            }
	} /* XDR_ENCODE */
	if (xdrs->x_op == XDR_DECODE) {
            buf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT);
            if (buf != NULL) {
                dmsg->rm_xid = IXDR_GET_U_INT32(buf);
                dmsg->rm_direction = IXDR_GET_ENUM(buf, enum msg_type);
                switch (dmsg->rm_direction) {
                case CALL:
                    __warnx("%s 5: XDR_DECODE INLINE CALL", __func__);
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
                            if (inline_xdr_opaque(
                                    xdrs, oa->oa_base,
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
                        if (inline_xdr_enum(xdrs, &oa->oa_flavor) == FALSE ||
                            inline_xdr_u_int(xdrs, &oa->oa_length) == FALSE) {
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
                            if (inline_xdr_opaque(xdrs, oa->oa_base,
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
                    __warnx("%s 6: INLINE XDR_ENCODE REPLY (not properly "
                            "INLINE, not reached?)", __func__);
                    return (inline_xdr_union(
                                xdrs,
                                (enum_t *)&(dmsg->rm_reply.rp_stat),
                                (caddr_t)(void *)&(dmsg->rm_reply.ru),
                                reply_dscrm,
                                NULL_xdrproc_t));

                    break;
                default:
                    /* unlikely */
                    return (FALSE);
                }
            } else {
                /* ! inline */
                if (
                    inline_xdr_u_int32_t(xdrs, &(dmsg->rm_xid)) &&
                    inline_xdr_enum(xdrs, (enum_t *)&(dmsg->rm_direction))) {
                    switch (dmsg->rm_direction) {
                    case CALL:
                        __warnx("%s 7: non-inline CALL", __func__);
                        if (
                            inline_xdr_u_int32_t(
                                xdrs, &(dmsg->rm_call.cb_rpcvers)) &&
                            (dmsg->rm_call.cb_rpcvers == RPC_MSG_VERSION) &&
                            inline_xdr_u_int32_t(
                                xdrs, &(dmsg->rm_call.cb_prog)) &&
                            inline_xdr_u_int32_t(
                                xdrs, &(dmsg->rm_call.cb_vers)) &&
                            inline_xdr_u_int32_t(
                                xdrs, &(dmsg->rm_call.cb_proc)) &&
                            inline_xdr_opaque_auth(
                                xdrs, &(dmsg->rm_call.cb_cred)) )
                            return (
                                inline_xdr_opaque_auth(
                                    xdrs, &(dmsg->rm_call.cb_verf)));
                        break;
                    case REPLY:
                        __warnx("%s 8: non-INLINE REPLY", __func__);

                            if (! inline_xdr_enum(
                                    xdrs, (enum_t *) &(dmsg->rm_reply.rp_stat)))
                                return (FALSE);
                            switch (dmsg->rm_reply.rp_stat) {
                            case MSG_ACCEPTED:
                            {
                                struct accepted_reply *ar =
                                    (struct accepted_reply *)
                                    &(dmsg->rm_reply.ru);
                                if (! inline_xdr_opaque_auth(
                                        xdrs, &(ar->ar_verf)))
                                    return (FALSE);
                                if (! inline_xdr_enum(
                                        xdrs, (enum_t *)&(ar->ar_stat)))
                                    return (FALSE);
                                switch (ar->ar_stat) {
                                case SUCCESS:
                                    return ((*(ar->ar_results.proc))
                                            (xdrs, &(ar->ar_results.where)));

                                case PROG_MISMATCH:
                                    if (! inline_xdr_u_int32_t(
                                            xdrs, &(ar->ar_vers.low)))
                                        return (FALSE);
                                    return (inline_xdr_u_int32_t(
                                                xdrs, &(ar->ar_vers.high)));

                                case GARBAGE_ARGS:
                                case SYSTEM_ERR:
                                case PROC_UNAVAIL:
                                case PROG_UNAVAIL:
                                    /* TRUE */
                                    break;
                                default:
                                    break;
                                } /* ar_stat */
                                return (TRUE);
                            } /* MSG_ACCEPTED */
                            break;
                            case MSG_DENIED:
                            {
                                /* XXX branch not verified */
                                __warnx("non-inline MSG_DENIED not verified");
                                struct rejected_reply *rr =
                                    (struct rejected_reply *)
                                    &(dmsg->rm_reply.ru);
                                if (! inline_xdr_enum(
                                        xdrs, (enum_t *)&(rr->rj_stat)))
                                    return (FALSE);
                                switch (rr->rj_stat) {

                                case RPC_MISMATCH:
                                    if (! inline_xdr_u_int32_t(
                                            xdrs, &(rr->rj_vers.low)))
                                        return (FALSE);
                                    return (inline_xdr_u_int32_t(
                                                xdrs, &(rr->rj_vers.high)));

                                case AUTH_ERROR:
                                    return (inline_xdr_enum(
                                                xdrs, (enum_t *)&(rr->rj_why)));
                                }
                                return (TRUE);
                            }
                            break;
                            default:
                                return (FALSE);
                                break;
                            } /* rm_reply.rp_stat */
                    default:
                        /* unlikely */
                        return (FALSE);
                    }
                }
            }
	} /* XDR_DECODE */
            return (FALSE);       
} /* new */


/* 2-stage decode */

bool_t
xdr_dplx_msg_decode_start(xdrs, dmsg)
	XDR *xdrs;
	struct rpc_msg *dmsg;
{
	int32_t *buf;

	assert(xdrs != NULL);
	assert(dmsg != NULL);

        assert(xdrs->x_op == XDR_DECODE);

        /* begin call consumes only the header */

        buf = dmsg->rm_ibuf = XDR_INLINE(xdrs, 8 * BYTES_PER_XDR_UNIT);

        if (buf != NULL) {
            dmsg->rm_xid = IXDR_GET_U_INT32(buf);
            dmsg->rm_direction = IXDR_GET_ENUM(buf, enum msg_type);
            switch (dmsg->rm_direction) {
            case CALL:
                dmsg->rm_call.cb_rpcvers = IXDR_GET_U_INT32(buf);
                if (dmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION)
                    return (FALSE);
                /* get program and version information early */
                dmsg->rm_call.cb_prog = IXDR_GET_U_INT32(buf);
                dmsg->rm_call.cb_vers = IXDR_GET_U_INT32(buf);
                dmsg->rm_call.cb_proc = IXDR_GET_U_INT32(buf);
                return (TRUE);
                break;
            case REPLY:
                /* in continuation */
                return (TRUE);
                break;
            default:
                /* unlikely */
                return (FALSE);
            }
        } else { /* ! buf, couldn't INLINE */ 
            if (! (inline_xdr_u_int32_t(xdrs, &(dmsg->rm_xid)) && 
                   inline_xdr_enum(xdrs, (enum_t *) &(dmsg->rm_direction))))
                return (FALSE);
            switch (dmsg->rm_direction) {
            case CALL:
                if (! ((inline_xdr_u_int32_t(
                            xdrs, &(dmsg->rm_call.cb_rpcvers))) &&
                       (dmsg->rm_call.cb_rpcvers != RPC_MSG_VERSION)))
                    return (FALSE);
                /* get program and version information early */
                if (! ((inline_xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_prog))) &&
                       (inline_xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_vers))) &&
                       (inline_xdr_u_int32_t(xdrs, &(dmsg->rm_call.cb_proc)))))
                    return (FALSE);
                return (TRUE);
                break;
            case REPLY:
                /* in continuation */
                return (TRUE);
                break;
            default:
                /* unlikely */
                return (FALSE);
            }
        } /* ! inline */

        return (FALSE);
}

bool_t
xdr_dplx_msg_decode_continue(xdrs, dmsg)
	XDR *xdrs;
	struct rpc_msg *dmsg;
{
	int32_t *buf;
	struct opaque_auth *oa;

	assert(xdrs != NULL);
	assert(dmsg != NULL);

        assert(xdrs->x_op == XDR_DECODE);

        buf = dmsg->rm_ibuf;

        if (buf != NULL) {
            /* continue where we left off */
            switch (dmsg->rm_direction) {
            case CALL:
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
                        if (inline_xdr_opaque(
                                xdrs, oa->oa_base,
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
                    if (inline_xdr_enum(xdrs, &oa->oa_flavor) == FALSE ||
                        inline_xdr_u_int(xdrs, &oa->oa_length) == FALSE) {
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
                        if (inline_xdr_opaque(xdrs, oa->oa_base,
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
                __warnx("%s 6: INLINE XDR_ENCODE REPLY (not properly "
                        "INLINE, not reached?)", __func__);
                return (inline_xdr_union(
                            xdrs,
                            (enum_t *)&(dmsg->rm_reply.rp_stat),
                            (caddr_t)(void *)&(dmsg->rm_reply.ru),
                            reply_dscrm,
                            NULL_xdrproc_t));
                
                break;
            default:
                /* unlikely */
                return (FALSE);
            }
        } else {
            /* ! inline */
            switch (dmsg->rm_direction) {
            case CALL:
                __warnx("%s 7: non-inline CALL", __func__);
                if (inline_xdr_opaque_auth(
                        xdrs, &(dmsg->rm_call.cb_cred)) )
                    return (
                        inline_xdr_opaque_auth(
                            xdrs, &(dmsg->rm_call.cb_verf)));
                break;
            case REPLY:
                __warnx("%s 8: non-INLINE REPLY", __func__);
                if (! inline_xdr_enum(
                        xdrs, (enum_t *) &(dmsg->rm_reply.rp_stat)))
                    return (FALSE);
                switch (dmsg->rm_reply.rp_stat) {
                case MSG_ACCEPTED:
                {
                    struct accepted_reply *ar =
                        (struct accepted_reply *)
                        &(dmsg->rm_reply.ru);
                    if (! inline_xdr_opaque_auth(
                            xdrs, &(ar->ar_verf)))
                        return (FALSE);
                    if (! inline_xdr_enum(
                            xdrs, (enum_t *)&(ar->ar_stat)))
                        return (FALSE);
                    switch (ar->ar_stat) {
                    case SUCCESS:
                        return ((*(ar->ar_results.proc))
                                (xdrs, &(ar->ar_results.where)));

                    case PROG_MISMATCH:
                        if (! inline_xdr_u_int32_t(
                                xdrs, &(ar->ar_vers.low)))
                            return (FALSE);
                        return (inline_xdr_u_int32_t(
                                    xdrs, &(ar->ar_vers.high)));
    
                    case GARBAGE_ARGS:
                    case SYSTEM_ERR:
                    case PROC_UNAVAIL:
                    case PROG_UNAVAIL:
                        /* TRUE */
                        break;
                    default:
                        break;
                    } /* ar_stat */
                    return (TRUE);
                } /* MSG_ACCEPTED */
                break;
                case MSG_DENIED:
                {
                    /* XXX branch not verified */
                    __warnx("non-inline MSG_DENIED not verified");
                    struct rejected_reply *rr =
                        (struct rejected_reply *)
                        &(dmsg->rm_reply.ru);
                    if (! inline_xdr_enum(
                            xdrs, (enum_t *)&(rr->rj_stat)))
                        return (FALSE);
                    switch (rr->rj_stat) {

                    case RPC_MISMATCH:
                        if (! inline_xdr_u_int32_t(
                                xdrs, &(rr->rj_vers.low)))
                            return (FALSE);
                        return (inline_xdr_u_int32_t(
                                    xdrs, &(rr->rj_vers.high)));

                    case AUTH_ERROR:
                        return (inline_xdr_enum(
                                    xdrs, (enum_t *)&(rr->rj_why)));
                    }
                    return (TRUE);
                }
                break;
                default:
                    return (FALSE);
                    break;
                } /* rm_reply.rp_stat */
            default:
                /* unlikely */
                return (FALSE);
            }
        }

        return (FALSE);
}
