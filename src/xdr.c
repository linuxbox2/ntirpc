/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2010-2018 Red Hat, Inc. and/or its affiliates.
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

#include "config.h"
#include <sys/cdefs.h>

/*
 * xdr.c, Generic XDR routines implementation.
 *
 * Copyright (C) 1986, Sun Microsystems, Inc.
 *
 * These are the "generic" xdr routines used to serialize and de-serialize
 * most common data items.  See xdr.h for more info on the interface to
 * xdr.
 */

#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include <rpc/xdr_inline.h>
#include <rpc/rpc.h>
#include "rpc_com.h"

typedef quad_t longlong_t;	/* ANSI long long type */
typedef u_quad_t u_longlong_t;	/* ANSI unsigned long long type */

/*
 * for cleanup
 */
XDR xdr_free_null_stream = {
	.x_op = XDR_FREE,
	.x_public = NULL,
	.x_private = NULL,
	.x_lib = {NULL, NULL},
	.x_data = NULL,
	.x_base = NULL,
	.x_v = {NULL, NULL, NULL, NULL},
};

/*
 * XDR nothing
 */
bool
xdr_void(void)
{
	return (true);
}

/*
 * XDR integers
 */
bool
xdr_int(XDR *xdrs, int *ip)
{
	long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (long)*ip;
		return (XDR_PUTLONG(xdrs, &l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &l))
			return (false);
		*ip = (int)l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned integers
 */
bool
xdr_u_int(XDR *xdrs, u_int *up)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *up;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*up = (u_int) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR long integers
 * same as xdr_u_long - open coded to save a proc call!
 */
bool
xdr_long(XDR *xdrs, long *lp)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (XDR_PUTLONG(xdrs, lp));
	case XDR_DECODE:
		return (XDR_GETLONG(xdrs, lp));
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned long integers
 * same as xdr_long - open coded to save a proc call!
 */
bool
xdr_u_long(XDR *xdrs, u_long *ulp)
{
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		return (XDR_PUTLONG(xdrs, (long *)ulp));
	case XDR_DECODE:
		return (XDR_GETLONG(xdrs, (long *)ulp));
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * Implemented here due to commonality of the object.
 */
bool
xdr_netobj(XDR *xdrs, struct netobj *np)
{
	return (xdr_bytes(xdrs, &np->n_bytes, &np->n_len, MAX_NETOBJ_SZ));
}

/*
 * Non-portable xdr primitives.
 * Care should be taken when moving these routines to new architectures.
 */

/*
 * Wrapper for xdr_string that can be called directly from
 * routines like clnt_call
 */
bool
xdr_wrapstring(XDR *xdrs, char **cpp)
{
	return xdr_string(xdrs, cpp, RPC_MAXDATASIZE);
}

/*
 * NOTE: xdr_hyper(), xdr_u_hyper(), xdr_longlong_t(), and xdr_u_longlong_t()
 * are in the "non-portable" section because they require that a `long long'
 * be a 64-bit type.
 *
 * --thorpej@netbsd.org, November 30, 1999
 */

/*
 * XDR longlong_t's
 */
bool
xdr_longlong_t(XDR *xdrs, longlong_t *llp)
{
	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_int64_t().
	 */
	return (xdr_int64_t(xdrs, (int64_t *) llp));
}

/*
 * XDR u_longlong_t's
 */
bool
xdr_u_longlong_t(XDR *xdrs, u_longlong_t *ullp)
{
	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_u_int64_t().
	 */
	return (xdr_u_int64_t(xdrs, (u_int64_t *) ullp));
}
