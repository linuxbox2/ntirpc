/*
 * Copyright (c) 2010, Oracle America, Inc.
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
 * - Neither the name of the "Oracle America, Inc." nor the names of its
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
 * from: @(#)clnt.h 1.31 94/04/29 SMI
 * from: @(#)clnt.h 2.1 88/07/29 4.0 RPCSRC
 * from: $NetBSD: clnt.h,v 1.14 2000/06/02 22:57:55 fvdl Exp $
 * from: $FreeBSD: src/include/rpc/clnt.h,v 1.21 2003/01/24 01:47:55 fjoe Exp $
 */

#ifndef _TIRPC_RPC_ERR_H_
#define _TIRPC_RPC_ERR_H_

#include <rpc/types.h>
#include <rpc/clnt_stat.h>
#include <rpc/auth_stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error info.
 */
struct rpc_err {
	union {
		int RE_errno;	/* related system error */
		enum auth_stat RE_why;	/* why the auth error occurred */
		struct {
			rpcvers_t low;	/* lowest version supported */
			rpcvers_t high;	/* highest version supported */
		} RE_vers;
		struct {	/* maybe meaningful if RPC_FAILED */
			int32_t s1;
			int32_t s2;
		} RE_lb;	/* life boot & debugging only */
	} ru;
#define re_errno ru.RE_errno
#define re_why  ru.RE_why
#define re_vers  ru.RE_vers
#define re_lb  ru.RE_lb

	enum clnt_stat re_status;
};

#ifdef __cplusplus
}
#endif
#endif				/* !_TIRPC_RPC_ERR_H_ */
