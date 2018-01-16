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

typedef quad_t longlong_t;	/* ANSI long long type */
typedef u_quad_t u_longlong_t;	/* ANSI unsigned long long type */

/*
 * constants specific to the xdr "protocol"
 */
#define XDR_FALSE ((long) 0)
#define XDR_TRUE ((long) 1)
#define RPC_MAXDATASIZE 9000

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
 * XDR 32-bit integers
 * same as xdr_u_int32_t - open coded to save a proc call!
 */
bool
xdr_int32_t(XDR *xdrs, int32_t *int32_p)
{
	long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (long)*int32_p;
		return (XDR_PUTLONG(xdrs, &l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &l))
			return (false);
		*int32_p = (int32_t) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned 32-bit integers
 * same as xdr_int32_t - open coded to save a proc call!
 */
bool
xdr_u_int32_t(XDR *xdrs, u_int32_t *u_int32_p)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *u_int32_p;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*u_int32_p = (u_int32_t) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned 32-bit integers
 * same as xdr_int32_t - open coded to save a proc call!
 */
bool
xdr_uint32_t(XDR *xdrs, u_int32_t *uint32_p)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *uint32_p;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*uint32_p = (uint32_t) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR short integers
 */
bool
xdr_short(XDR *xdrs, short *sp)
{
	long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (long)*sp;
		return (XDR_PUTLONG(xdrs, &l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &l))
			return (false);
		*sp = (short)l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned short integers
 */
bool
xdr_u_short(XDR *xdrs, u_short *usp)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *usp;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*usp = (u_short) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR 16-bit integers
 */
bool
xdr_int16_t(XDR *xdrs, int16_t *int16_p)
{
	long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (long)*int16_p;
		return (XDR_PUTLONG(xdrs, &l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &l))
			return (false);
		*int16_p = (int16_t) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned 16-bit integers
 */
bool
xdr_u_int16_t(XDR *xdrs, u_int16_t *u_int16_p)
{
	u_long l;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		l = (u_long) *u_int16_p;
		return (XDR_PUTLONG(xdrs, (long *)&l));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, (long *)&l))
			return (false);
		*u_int16_p = (u_int16_t) l;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR a char
 */
bool
xdr_char(XDR *xdrs, char *cp)
{
	int i;

	i = (*cp);
	if (!xdr_int(xdrs, &i))
		return (false);
	*cp = i;
	return (true);
}

/*
 * XDR an unsigned char
 */
bool
xdr_u_char(XDR *xdrs, u_char *cp)
{
	u_int u;

	u = (*cp);
	if (!xdr_u_int(xdrs, &u))
		return (false);
	*cp = u;
	return (true);
}

/*
 * XDR booleans
 */
bool
xdr_bool(XDR *xdrs, bool_t *bp)
{
	long lb;

	switch (xdrs->x_op) {

	case XDR_ENCODE:
		lb = *bp ? XDR_TRUE : XDR_FALSE;
		return (XDR_PUTLONG(xdrs, &lb));

	case XDR_DECODE:
		if (!XDR_GETLONG(xdrs, &lb))
			return (false);
		*bp = (lb == XDR_FALSE) ? false : true;
		return (true);

	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR enumerations
 */
bool
xdr_enum(XDR *xdrs, enum_t *ep)
{
	enum sizecheck { SIZEVAL };	/* used to find the size of an enum */

	/*
	 * enums are treated as ints
	 */
	if (sizeof(enum sizecheck) == sizeof(long)) {
		return (xdr_long(xdrs, (long *)(void *)ep));
	} else if (sizeof(enum sizecheck) == sizeof(int)) {
		return (xdr_int(xdrs, (int *)(void *)ep));
	} else if (sizeof(enum sizecheck) == sizeof(short)) {
		return (xdr_short(xdrs, (short *)(void *)ep));
	} else {
		return (false);
	}
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
 * XDR a descriminated union
 * Support routine for discriminated unions.
 * You create an array of xdrdiscrim structures, terminated with
 * an entry with a null procedure pointer.  The routine gets
 * the discriminant value and then searches the array of xdrdiscrims
 * looking for that value.  It calls the procedure given in the xdrdiscrim
 * to handle the discriminant.  If there is no specific routine a default
 * routine may be called.
 * If there is no specific or default routine an error is returned.
 */
bool
xdr_union(XDR  *xdrs, enum_t *dscmp, /* enum to decide which arm to work on */
	  char *unp,	/* the union itself */
	  const struct xdr_discrim *choices,
			/* [value, xdr proc] for each arm */
	  xdrproc_t dfault /* default xdr routine */)
{
	enum_t dscm;

	/*
	 * we deal with the discriminator;  it's an enum
	 */
	if (!xdr_enum(xdrs, dscmp))
		return (false);
	dscm = *dscmp;

	/*
	 * search choices for a value that matches the discriminator.
	 * if we find one, execute the xdr routine for that value.
	 */
	for (; choices->proc != NULL_xdrproc_t; choices++) {
		if (choices->value == dscm)
			return ((*(choices->proc)) (xdrs, unp));
	}

	/*
	 * no match - execute the default xdr routine if there is one
	 */
	return ((dfault == NULL_xdrproc_t) ? false : (*dfault) (xdrs, unp));
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
 * XDR 64-bit integers
 */
bool
xdr_int64_t(XDR *xdrs, int64_t *llp)
{
	u_long ul[2];

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		ul[0] = (u_long) ((u_int64_t) *llp >> 32) & 0xffffffff;
		ul[1] = (u_long) ((u_int64_t) *llp) & 0xffffffff;
		if (XDR_PUTLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		return (XDR_PUTLONG(xdrs, (long *)&ul[1]));
	case XDR_DECODE:
		if (XDR_GETLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		if (XDR_GETLONG(xdrs, (long *)&ul[1]) == false)
			return (false);
		*llp = (int64_t)
		    (((u_int64_t) ul[0] << 32) | ((u_int64_t) ul[1]));
		return (true);
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned 64-bit integers
 */
bool
xdr_u_int64_t(XDR *xdrs, u_int64_t *ullp)
{
	u_long ul[2];

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		ul[0] = (u_long) (*ullp >> 32) & 0xffffffff;
		ul[1] = (u_long) (*ullp) & 0xffffffff;
		if (XDR_PUTLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		return (XDR_PUTLONG(xdrs, (long *)&ul[1]));
	case XDR_DECODE:
		if (XDR_GETLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		if (XDR_GETLONG(xdrs, (long *)&ul[1]) == false)
			return (false);
		*ullp = (u_int64_t)
		    (((u_int64_t) ul[0] << 32) | ((u_int64_t) ul[1]));
		return (true);
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR unsigned 64-bit integers
 */
bool
xdr_uint64_t(XDR *xdrs, uint64_t *ullp)
{
	u_long ul[2];

	switch (xdrs->x_op) {
	case XDR_ENCODE:
		ul[0] = (u_long) (*ullp >> 32) & 0xffffffff;
		ul[1] = (u_long) (*ullp) & 0xffffffff;
		if (XDR_PUTLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		return (XDR_PUTLONG(xdrs, (long *)&ul[1]));
	case XDR_DECODE:
		if (XDR_GETLONG(xdrs, (long *)&ul[0]) == false)
			return (false);
		if (XDR_GETLONG(xdrs, (long *)&ul[1]) == false)
			return (false);
		*ullp = (uint64_t)
		    (((uint64_t) ul[0] << 32) | ((uint64_t) ul[1]));
		return (true);
	case XDR_FREE:
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * XDR hypers
 */
bool
xdr_hyper(XDR *xdrs, longlong_t *llp)
{
	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_int64_t().
	 */
	return (xdr_int64_t(xdrs, (int64_t *) llp));
}

/*
 * XDR unsigned hypers
 */
bool
xdr_u_hyper(XDR *xdrs, u_longlong_t *ullp)
{
	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_u_int64_t().
	 */
	return (xdr_u_int64_t(xdrs, (u_int64_t *) ullp));
}

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
