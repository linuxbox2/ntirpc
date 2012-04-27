/*	$NetBSD: auth.h,v 1.15 2000/06/02 22:57:55 fvdl Exp $	*/

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
 *
 *	from: @(#)auth.h 1.17 88/02/08 SMI
 *	from: @(#)auth.h	2.3 88/08/07 4.0 RPCSRC
 *	from: @(#)auth.h	1.43 	98/02/02 SMI
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
#define _TIRPC_AUTH__INLINE_H

#include <rpc/xdr_inline.h>
#include <rpc/auth.h>

/*
 * XDR xdr_opaque_auth
 */
static inline bool_t 
inline_xdr_opaque_auth(XDR *xdrs, struct opaque_auth *oa)
{
    if (! (inline_xdr_enum(xdrs, &oa->oa_flavor) &&
           (inline_xdr_u_int(xdrs, &oa->oa_length))))
        return (FALSE);

    switch (xdrs->x_op) {
    case XDR_DECODE:
        if (oa->oa_length) {
            if (oa->oa_length > MAX_AUTH_BYTES)
                return (FALSE);
            if (oa->oa_base == NULL) {
                oa->oa_base = (caddr_t) mem_alloc(oa->oa_length);
                if (oa->oa_base == NULL)
                    return (FALSE);
            }
            return (inline_xdr_opaque(xdrs, oa->oa_base, oa->oa_length));
        }
        return (TRUE); /* 0 length succeeds */
        break;
    case XDR_ENCODE:
        if (oa->oa_length) {
            if (oa->oa_length > MAX_AUTH_BYTES)
                return (FALSE);
            return (inline_xdr_opaque(xdrs, oa->oa_base, oa->oa_length));
        }
        return (TRUE); /* 0 length succeeds */
        break;
    case XDR_FREE:
        if (oa->oa_base != NULL) {
            mem_free(oa->oa_base, oa->oa_length);
            oa->oa_base = NULL;
        }
        return (TRUE);
        break;
    default:
        break;
    } /* x_op */

    return (FALSE);
}

#endif /* !_TIRPC_AUTH_INLINE_H */
