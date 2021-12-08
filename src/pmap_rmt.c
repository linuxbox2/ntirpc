/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2012-2018 Red Hat, Inc. and/or its affiliates.
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
 * pmap_rmt.c
 * remote call and broadcast service
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */

#include "config.h"
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>
#include <rpc/xdr_inline.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_rmt.h>

/*
 * XDR remote call arguments
 * written for XDR_ENCODE direction only
 */
bool
xdr_rmtcall_args(XDR *xdrs, struct rmtcallargs *cap)
{
	u_int lenposition, argposition, position;

	assert(xdrs != NULL);
	assert(cap != NULL);

	if (xdr_rpcprog(xdrs, &(cap->prog))
	 && xdr_rpcvers(xdrs, &(cap->vers))
	 && xdr_rpcproc(xdrs, &(cap->proc))) {
		lenposition = XDR_GETPOS(xdrs);
		if (!xdr_uint32_t(xdrs, &(cap->arglen)))
			return (false);
		argposition = XDR_GETPOS(xdrs);
		if (!(*(cap->xdr_args)) (xdrs, cap->args_ptr))
			return (false);
		position = XDR_GETPOS(xdrs);
		cap->arglen = position - argposition;
		XDR_SETPOS(xdrs, lenposition);
		if (!xdr_uint32_t(xdrs, &(cap->arglen)))
			return (false);
		XDR_SETPOS(xdrs, position);
		return (true);
	}
	return (false);
}

/*
 * XDR remote call results
 * written for XDR_DECODE direction only
 */
bool
xdr_rmtcallres(XDR *xdrs, struct rmtcallres *crp)
{
	assert(xdrs != NULL);
	assert(crp != NULL);

	if (xdr_rpcport(xdrs, &crp->port)
	 && xdr_uint32_t(xdrs, &crp->resultslen)) {
		return ((*(crp->xdr_results)) (xdrs, crp->results_ptr));
	}
	return (false);
}
