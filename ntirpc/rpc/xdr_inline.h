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

#ifndef _TIRPC_INLINE_XDR_H
#define _TIRPC_INLINE_XDR_H

#if !defined(_WIN32)
#include <err.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <rpc/xdr.h>

/*
 * constants specific to the xdr "protocol"
 */
#define XDR_FALSE ((long) 0)
#define XDR_TRUE ((long) 1)
#define LASTUNSIGNED ((u_int) 0-1)

/*
 * Free a data structure using XDR
 * Not a filter, but a convenient utility nonetheless
 */
static inline void
inline_xdr_free(xdrproc_t proc, void *objp)
{
	XDR x;

	x.x_op = XDR_FREE;
	(*proc) (&x, objp);
}

/*
 * XDR nothing
 */
static inline bool
inline_xdr_void(void)
{
	return (true);
}

/*
 * XDR integers
 */
static inline bool
inline_xdr_int(XDR *xdrs, int *ip)
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
static inline bool
inline_xdr_u_int(XDR *xdrs, u_int *up)
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
static inline bool
inline_xdr_long(XDR *xdrs, long *lp)
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
static inline bool
inline_xdr_u_long(XDR *xdrs, u_long *ulp)
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
static inline bool
inline_xdr_int32_t(XDR *xdrs, int32_t *int32_p)
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
static inline bool
inline_xdr_u_int32_t(XDR *xdrs, u_int32_t *u_int32_p)
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
 * XDR short integers
 */
static inline bool
inline_xdr_short(XDR *xdrs, short *sp)
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
static inline bool
inline_xdr_u_short(XDR *xdrs, u_short *usp)
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
static inline bool
inline_xdr_int16_t(XDR *xdrs, int16_t *int16_p)
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
static inline bool
inline_xdr_u_int16_t(XDR *xdrs, u_int16_t *u_int16_p)
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
static inline bool
inline_xdr_char(XDR *xdrs, char *cp)
{
	int i;

	i = (*cp);
	if (!inline_xdr_int(xdrs, &i))
		return (false);
	*cp = i;
	return (true);
}

/*
 * XDR an unsigned char
 */
static inline bool
inline_xdr_u_char(XDR *xdrs, u_char *cp)
{
	u_int u;

	u = (*cp);
	if (!inline_xdr_u_int(xdrs, &u))
		return (false);
	*cp = u;
	return (true);
}

/*
 * XDR booleans
 */
static inline bool
inline_xdr_bool(XDR *xdrs, bool_t *bp)
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
static inline bool
inline_xdr_enum(XDR *xdrs, enum_t *ep)
{
	enum sizecheck { SIZEVAL };	/* used to find the size of an enum */

	/*
	 * enums are treated as ints
	 */
	/* LINTED */ if (sizeof(enum sizecheck) == sizeof(long)) {
		return (inline_xdr_long(xdrs, (long *)(void *)ep));
	} else /* LINTED */ if (sizeof(enum sizecheck) == sizeof(int)) {
		return (inline_xdr_int(xdrs, (int *)(void *)ep));
	} else /* LINTED */ if (sizeof(enum sizecheck) == sizeof(short)) {
		return (inline_xdr_short(xdrs, (short *)(void *)ep));
	} else {
		return (false);
	}
}

/*
 * decode opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
inline_xdr_getopaque(XDR *xdrs, caddr_t cp, u_int cnt)
{
	u_int rndup;

	/*
	 * if no data we are done
	 */
	if (cnt == 0)
		return (true);

	/*
	 * XDR_INLINE is just as likely to do a function call,
	 * so don't bother with it here.
	 */
	if (!XDR_GETBYTES(xdrs, cp, cnt)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR opaque",
			__func__, __LINE__);
		return (false);
	}

	/*
	 * round byte count to full xdr units
	 */
	rndup = cnt & (BYTES_PER_XDR_UNIT - 1);

	if (rndup > 0) {
		uint32_t crud;

		if (!XDR_GETBYTES(xdrs, (caddr_t) &crud,
				  BYTES_PER_XDR_UNIT - rndup)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR crud",
				__func__, __LINE__);
			return (false);
		}
	}

	return (true);
}

/*
 * encode opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
inline_xdr_putopaque(XDR *xdrs, caddr_t cp, u_int cnt)
{
	u_int rndup;

	/*
	 * if no data we are done
	 */
	if (cnt == 0)
		return (true);

	/*
	 * XDR_INLINE is just as likely to do a function call,
	 * so don't bother with it here.
	 */
	if (!XDR_PUTBYTES(xdrs, cp, cnt)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR opaque",
			__func__, __LINE__);
		return (false);
	}

	/*
	 * round byte count to full xdr units
	 */
	rndup = cnt & (BYTES_PER_XDR_UNIT - 1);

	if (rndup > 0) {
		uint32_t zero = 0;

		if (!XDR_PUTBYTES(xdrs, (caddr_t) &zero,
				  BYTES_PER_XDR_UNIT - rndup)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR zero",
				__func__, __LINE__);
			return (false);
		}
	}

	return (true);
}

/*
 * XDR opaque data
 * Allows the specification of a fixed size sequence of opaque bytes.
 * cp points to the opaque object and cnt gives the byte length.
 */
static inline bool
inline_xdr_opaque(XDR *xdrs, caddr_t cp, u_int cnt)
{
	if (xdrs->x_op == XDR_DECODE)
		return (inline_xdr_getopaque(xdrs, cp, cnt));

	if (xdrs->x_op == XDR_ENCODE)
		return (inline_xdr_putopaque(xdrs, cp, cnt));

	if (xdrs->x_op == XDR_FREE)
		return (true);

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s:%u ERROR xdrs->x_op (%u)",
		__func__, __LINE__,
		xdrs->x_op);
	return (false);
}

/*
 * XDR counted bytes
 * *cpp is a pointer to the bytes, *sizep is the count.
 * If *cpp is NULL maxsize bytes are allocated
 */
static inline bool
inline_xdr_bytes(XDR *xdrs, char **cpp, u_int *sizep,
		 u_int maxsize)
{
	char *sp = *cpp;	/* sp is the actual string pointer */
	u_int nodesize;

	/*
	 * first deal with the length since xdr bytes are counted
	 */
	if (!inline_xdr_u_int(xdrs, sizep))
		return (false);
	nodesize = *sizep;
	if ((nodesize > maxsize) && (xdrs->x_op != XDR_FREE))
		return (false);

	/*
	 * now deal with the actual bytes
	 */
	switch (xdrs->x_op) {

	case XDR_DECODE:
		if (nodesize == 0)
			return (true);
		if (sp == NULL)
			*cpp = sp = (char *)mem_alloc(nodesize);
		return (inline_xdr_getopaque(xdrs, sp, nodesize));

	case XDR_ENCODE:
		return (inline_xdr_putopaque(xdrs, sp, nodesize));

	case XDR_FREE:
		if (sp != NULL) {
			mem_free(sp, -1);
			*cpp = NULL;
		}
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * Implemented here due to commonality of the object.
 */
static inline bool
inline_xdr_netobj(XDR *xdrs, struct netobj *np)
{

	return (inline_xdr_bytes
		(xdrs, &np->n_bytes, &np->n_len, MAX_NETOBJ_SZ));
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
static inline bool
inline_xdr_union(XDR *xdrs, enum_t *dscmp,
  /* enum to decide which arm to work on */
		 char *unp,	/* the union itself */
		 const struct xdr_discrim *choices,
/* [value, xdr proc] for each arm */
		 xdrproc_t dfault)
{				/* default xdr routine */
	enum_t dscm;

	/*
	 * we deal with the discriminator;  it's an enum
	 */
	if (!inline_xdr_enum(xdrs, dscmp))
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
 * XDR null terminated ASCII strings
 * xdr_string deals with "C strings" - arrays of bytes that are
 * terminated by a NULL character.  The parameter cpp references a
 * pointer to storage; If the pointer is null, then the necessary
 * storage is allocated.  The last parameter is the max allowed length
 * of the string as specified by a protocol.
 */
static inline bool
inline_xdr_string(XDR *xdrs, char **cpp, u_int maxsize)
{
	char *sp = *cpp;	/* sp is the actual string pointer */
	u_int size = 0;		/* XXX remove warning */
	u_int nodesize;

	/*
	 * first deal with the length since xdr strings are counted-strings
	 */
	switch (xdrs->x_op) {
	case XDR_FREE:
		if (sp == NULL)
			return (true);	/* already free */
		break;
	case XDR_ENCODE:
		if (sp == NULL)
			return false;
		size = strlen(sp);
		break;
	case XDR_DECODE:
		break;
	}
	if (!inline_xdr_u_int(xdrs, &size))
		return (false);
	if (size > maxsize && xdrs->x_op != XDR_FREE)
		return (false);
	nodesize = size + 1;
	if (nodesize == 0) {
		/* This means an overflow.  It a bug in the caller which
		 * provided a too large maxsize but nevertheless catch it
		 * here.
		 */
		return false;
	}

	/*
	 * now deal with the actual bytes
	 */
	switch (xdrs->x_op) {

	case XDR_DECODE:
		if (sp == NULL)
			*cpp = sp = (char *)mem_alloc(nodesize);
		sp[size] = 0;
		return (inline_xdr_getopaque(xdrs, sp, size));

	case XDR_ENCODE:
		return (inline_xdr_putopaque(xdrs, sp, size));

	case XDR_FREE:
		mem_free(sp, -1);
		*cpp = NULL;
		return (true);
	}
	/* NOTREACHED */
	return (false);
}

/*
 * Wrapper for xdr_string that can be called directly from
 * routines like clnt_call
 */
static inline bool
inline_xdr_wrapstring(XDR *xdrs, char **cpp)
{
	return inline_xdr_string(xdrs, cpp, LASTUNSIGNED);
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
static inline bool
inline_xdr_int64_t(XDR *xdrs, int64_t *llp)
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
static inline bool
inline_xdr_u_int64_t(XDR *xdrs, u_int64_t *ullp)
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
 * XDR hypers
 */
static inline bool
inline_xdr_hyper(XDR *xdrs, quad_t *llp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_int64_t().
	 */
	return (inline_xdr_int64_t(xdrs, (int64_t *) llp));
}

/*
 * XDR unsigned hypers
 */
static inline bool
inline_xdr_u_hyper(XDR *xdrs, u_quad_t *ullp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_u_int64_t().
	 */
	return (inline_xdr_u_int64_t(xdrs, (u_int64_t *) ullp));
}

/*
 * XDR longlong_t's
 */
static inline bool
inline_xdr_longlong_t(XDR *xdrs, quad_t *llp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_int64_t().
	 */
	return (inline_xdr_int64_t(xdrs, (int64_t *) llp));
}

/*
 * XDR u_longlong_t's
 */
static inline bool
inline_xdr_u_longlong_t(XDR *xdrs, u_quad_t *ullp)
{

	/*
	 * Don't bother open-coding this; it's a fair amount of code.  Just
	 * call xdr_u_int64_t().
	 */
	return (inline_xdr_u_int64_t(xdrs, (u_int64_t *) ullp));
}

#endif				/* _TIRPC_INLINE_XDR_H */
