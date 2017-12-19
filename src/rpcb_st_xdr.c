/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2013-2017 Red Hat, Inc. and/or its affiliates.
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
 * Copyright 1991 Sun Microsystems, Inc.
 * rpcb_stat_xdr.c
 */

/*
 * This file was generated from rpcb_prot.x, but includes only those
 * routines used with the rpcbind stats facility.
 */

#include <config.h>
#include <sys/cdefs.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpc/xdr_inline.h>

/* Link list of all the stats about getport and getaddr */

bool
xdr_rpcbs_addrlist(XDR *xdrs, rpcbs_addrlist *objp)
{
	if (!xdr_u_int32_t(xdrs, &objp->prog))
		return (false);
	if (!xdr_u_int32_t(xdrs, &objp->vers))
		return (false);
	if (!xdr_int(xdrs, &objp->success))
		return (false);
	if (!xdr_int(xdrs, &objp->failure))
		return (false);
	if (!xdr_string(xdrs, &objp->netid, RPC_MAXDATASIZE))
		return (false);
	if (!xdr_pointer
	    (xdrs, (char **)&objp->next, sizeof(rpcbs_addrlist),
	     (xdrproc_t) xdr_rpcbs_addrlist))
		return (false);

	return (true);
}

/* Link list of all the stats about rmtcall */

bool
xdr_rpcbs_rmtcalllist(XDR *xdrs, rpcbs_rmtcalllist *objp)
{
	int32_t *buf;

	if (xdrs->x_op == XDR_ENCODE) {
		buf = xdr_inline_encode(xdrs, 6 * BYTES_PER_XDR_UNIT);
		if (buf != NULL) {
			/* most likely */
			IXDR_PUT_U_INT32(buf, objp->prog);
			IXDR_PUT_U_INT32(buf, objp->vers);
			IXDR_PUT_U_INT32(buf, objp->proc);
			IXDR_PUT_INT32(buf, objp->success);
			IXDR_PUT_INT32(buf, objp->failure);
			IXDR_PUT_INT32(buf, objp->indirect);
		} else if (!XDR_PUTUINT32(xdrs, objp->prog)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR prog %u",
				__func__, __LINE__,
				objp->prog);
			return (false);
		} else if (!XDR_PUTUINT32(xdrs, objp->vers)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR vers %u",
				__func__, __LINE__,
				objp->vers);
			return (false);
		} else if (!XDR_PUTUINT32(xdrs, objp->proc)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR proc %u",
				__func__, __LINE__,
				objp->proc);
			return (false);
		} else if (!XDR_PUTINT32(xdrs, objp->success)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR success %u",
				__func__, __LINE__,
				objp->success);
			return (false);
		} else if (!XDR_PUTINT32(xdrs, objp->failure)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR failure %u",
				__func__, __LINE__,
				objp->failure);
			return (false);
		} else if (!XDR_PUTINT32(xdrs, objp->indirect)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR indirect %u",
				__func__, __LINE__,
				objp->indirect);
			return (false);
		}
		if (!xdr_string_encode(xdrs, &objp->netid, RPC_MAXDATASIZE))
			return (false);
		if (!xdr_pointer
		    (xdrs, (char **)&objp->next, sizeof(rpcbs_rmtcalllist),
		     (xdrproc_t) xdr_rpcbs_rmtcalllist))
			return (false);
		return (true);
	}

	if (xdrs->x_op == XDR_DECODE) {
		buf = xdr_inline_decode(xdrs, 6 * BYTES_PER_XDR_UNIT);
		if (buf != NULL) {
			/* most likely */
			objp->prog = (rpcprog_t) IXDR_GET_U_INT32(buf);
			objp->vers = (rpcvers_t) IXDR_GET_U_INT32(buf);
			objp->proc = (rpcproc_t) IXDR_GET_U_INT32(buf);
			objp->success = (int)IXDR_GET_INT32(buf);
			objp->failure = (int)IXDR_GET_INT32(buf);
			objp->indirect = (int)IXDR_GET_INT32(buf);
		} else if (!XDR_GETUINT32(xdrs, &(objp->prog))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR prog",
				__func__, __LINE__);
			return (false);
		} else if (!XDR_GETUINT32(xdrs, &(objp->vers))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR vers",
				__func__, __LINE__);
			return (false);
		} else if (!XDR_GETUINT32(xdrs, &(objp->proc))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR proc",
				__func__, __LINE__);
			return (false);
		} else if (!XDR_GETINT32(xdrs, &(objp->success))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR success",
				__func__, __LINE__);
			return (false);
		} else if (!XDR_GETINT32(xdrs, &(objp->failure))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR failure",
				__func__, __LINE__);
			return (false);
		} else if (!XDR_GETINT32(xdrs, &(objp->indirect))) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s:%u ERROR indirect",
				__func__, __LINE__);
			return (false);
		}
		if (!xdr_string_decode(xdrs, &objp->netid, RPC_MAXDATASIZE))
			return (false);
		if (!xdr_pointer
		    (xdrs, (char **)&objp->next, sizeof(rpcbs_rmtcalllist),
		     (xdrproc_t) xdr_rpcbs_rmtcalllist))
			return (false);
		return (true);
	}

	if (!xdr_u_int32_t(xdrs, &objp->prog))
		return (false);
	if (!xdr_u_int32_t(xdrs, &objp->vers))
		return (false);
	if (!xdr_u_int32_t(xdrs, &objp->proc))
		return (false);
	if (!xdr_int(xdrs, &objp->success))
		return (false);
	if (!xdr_int(xdrs, &objp->failure))
		return (false);
	if (!xdr_int(xdrs, &objp->indirect))
		return (false);
	if (!xdr_string(xdrs, &objp->netid, RPC_MAXDATASIZE))
		return (false);
	if (!xdr_pointer
	    (xdrs, (char **)&objp->next, sizeof(rpcbs_rmtcalllist),
	     (xdrproc_t) xdr_rpcbs_rmtcalllist))
		return (false);

	return (true);
}

bool
xdr_rpcbs_proc(XDR *xdrs, rpcbs_proc objp)
{
	if (!xdr_vector
	    (xdrs, (char *)(void *)objp, RPCBSTAT_HIGHPROC, sizeof(int),
	     (xdrproc_t) xdr_int))
		return (false);

	return (true);
}

bool
xdr_rpcbs_addrlist_ptr(XDR *xdrs, rpcbs_addrlist_ptr *objp)
{
	if (!xdr_pointer
	    (xdrs, (char **)objp, sizeof(rpcbs_addrlist),
	     (xdrproc_t) xdr_rpcbs_addrlist))
		return (false);

	return (true);
}

bool
xdr_rpcbs_rmtcalllist_ptr(XDR *xdrs, rpcbs_rmtcalllist_ptr *objp)
{
	if (!xdr_pointer
	    (xdrs, (char **)objp, sizeof(rpcbs_rmtcalllist),
	     (xdrproc_t) xdr_rpcbs_rmtcalllist))
		return (false);

	return (true);
}

bool
xdr_rpcb_stat(XDR *xdrs, rpcb_stat *objp)
{
	if (!xdr_rpcbs_proc(xdrs, objp->info))
		return (false);
	if (!xdr_int(xdrs, &objp->setinfo))
		return (false);
	if (!xdr_int(xdrs, &objp->unsetinfo))
		return (false);
	if (!xdr_rpcbs_addrlist_ptr(xdrs, &objp->addrinfo))
		return (false);
	if (!xdr_rpcbs_rmtcalllist_ptr(xdrs, &objp->rmtinfo))
		return (false);

	return (true);
}

/*
 * One rpcb_stat structure is returned for each version of rpcbind
 * being monitored.
 */
bool
xdr_rpcb_stat_byvers(XDR *xdrs, rpcb_stat_byvers objp)
{
	if (!xdr_vector
	    (xdrs, (char *)(void *)objp, RPCBVERS_STAT, sizeof(rpcb_stat),
	     (xdrproc_t) xdr_rpcb_stat))
		return (false);

	return (true);
}
