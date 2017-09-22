/*
 * Copyright (c) 1984, 2009, Sun Microsystems, Inc.
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
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
 * from: $NetBSD: auth.h,v 1.15 2000/06/02 22:57:55 fvdl Exp $
 * from: $FreeBSD: src/include/rpc/auth.h,v 1.20 2003/01/01 18:48:42 schweikh Exp $
 */

#ifndef _TIRPC_AUTH_STAT_H_
#define _TIRPC_AUTH_STAT_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Status returned from authentication check
 */
enum auth_stat {
	AUTH_OK = 0,
	/*
	 * failed at  remote end
	 */
	AUTH_BADCRED = 1,	/* bogus credentials (seal broken) */
	AUTH_REJECTEDCRED = 2,	/* client should begin new session */
	AUTH_BADVERF = 3,	/* bogus verifier (seal broken) */
	AUTH_REJECTEDVERF = 4,	/* verifier expired or was replayed */
	AUTH_TOOWEAK = 5,	/* rejected due to security reasons */
	/*
	 * failed locally
	 */
	AUTH_INVALIDRESP = 6,	/* bogus response verifier */
	AUTH_FAILED = 7,	/* some unknown reason */
#ifdef KERBEROS
	/*
	 * kerberos errors
	 */
	AUTH_KERB_GENERIC = 8,	/* kerberos generic error */
	AUTH_TIMEEXPIRE = 9,	/* time of credential expired */
	AUTH_TKT_FILE = 10,	/* something wrong with ticket file */
	AUTH_DECODE = 11,	/* can't decode authenticator */
	AUTH_NET_ADDR = 12,	/* wrong net address in ticket */
#endif				/* KERBEROS */
	/*
	 * RPCSEC_GSS errors
	 */
	RPCSEC_GSS_CREDPROBLEM = 13,
	RPCSEC_GSS_CTXPROBLEM = 14,
};

#ifdef __cplusplus
}
#endif
#endif				/* !_TIRPC_AUTH_STAT_H_ */
