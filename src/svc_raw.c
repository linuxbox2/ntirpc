
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
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

/*
 * svc_raw.c,   This a toy for simple testing and timing.
 * Interface to create an rpc client and server in the same UNIX process.
 * This lets us similate rpc and get rpc (round trip) overhead, without
 * any interference from the kernel.
 *
 */
#include <config.h>
#include <pthread.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <sys/types.h>
#include <rpc/raw.h>
#include <stdlib.h>

#ifndef UDPMSGSIZE
#define UDPMSGSIZE 8800
#endif

/*
 * This is the "network" that we will be moving data over
 */
static struct svc_raw_private {
	char *raw_buf;		/* should be shared with the cl handle */
	SVCXPRT server;
	XDR xdr_stream;
	char verf_body[MAX_AUTH_BYTES];
} *svc_raw_private;

extern mutex_t svcraw_lock;

static void svc_raw_ops(SVCXPRT *);

char *__rpc_rawcombuf = NULL;

SVCXPRT *
svc_raw_ncreate(void)
{
	struct svc_raw_private *srp;

	/* VARIABLES PROTECTED BY svcraw_lock: svc_raw_private, srp */
	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		srp = (struct svc_raw_private *)mem_alloc(sizeof(*srp));
		if (__rpc_rawcombuf == NULL)
			__rpc_rawcombuf = mem_alloc(UDPMSGSIZE * sizeof(char));
		srp->raw_buf = __rpc_rawcombuf;	/* Share it with the client */
		svc_raw_private = srp;
	}
	srp->server.xp_fd = FD_SETSIZE;
	srp->server.xp_port = 0;
	srp->server.xp_p3 = NULL;
	svc_raw_ops(&srp->server);
/* XXX check and or fixme */
#if 0
	srp->server.xp_verf.oa_base = srp->verf_body;
#endif
	xdrmem_create(&srp->xdr_stream, srp->raw_buf, UDPMSGSIZE, XDR_DECODE);
	xprt_register(&srp->server);
	mutex_unlock(&svcraw_lock);

	return (&srp->server);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_raw_stat(SVCXPRT *xprt)
{
	return (XPRT_IDLE);
}

 /*ARGSUSED*/
static bool
svc_raw_recv(SVCXPRT *xprt, struct svc_req *req)
{
	struct rpc_msg *msg = req->rq_msg;
	struct svc_raw_private *srp;
	XDR *xdrs;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (false);
	}
	mutex_unlock(&svcraw_lock);

	xdrs = &srp->xdr_stream;
	xdrs->x_op = XDR_DECODE;
	(void)XDR_SETPOS(xdrs, 0);
	if (!xdr_callmsg(xdrs, msg))
		return (false);

	return (true);
}

 /*ARGSUSED*/
static bool
svc_raw_reply(SVCXPRT *xprt, struct svc_req *req, struct rpc_msg *msg)
{
	struct svc_raw_private *srp;
	XDR *xdrs;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (false);
	}
	mutex_unlock(&svcraw_lock);

	xdrs = &srp->xdr_stream;
	xdrs->x_op = XDR_ENCODE;
	(void)XDR_SETPOS(xdrs, 0);
	if (!xdr_replymsg(xdrs, msg))
		return (false);
	(void)XDR_GETPOS(xdrs);	/* called just for overhead */

	return (true);
}

 /*ARGSUSED*/
static bool
svc_raw_freeargs(SVCXPRT *xprt, struct svc_req *req, xdrproc_t xdr_args,
		 void *args_ptr)
{
	struct svc_raw_private *srp;
	XDR *xdrs;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (false);
	}
	mutex_unlock(&svcraw_lock);

	xdrs = &srp->xdr_stream;
	xdrs->x_op = XDR_FREE;

	return (*xdr_args) (xdrs, args_ptr);
}

 /*ARGSUSED*/
static bool
svc_raw_getargs(SVCXPRT *xprt, struct svc_req *req, xdrproc_t xdr_args,
		void *args_ptr, void *u_data)
{
	struct svc_raw_private *srp;

	mutex_lock(&svcraw_lock);
	srp = svc_raw_private;
	if (srp == NULL) {
		mutex_unlock(&svcraw_lock);
		return (false);
	}
	mutex_unlock(&svcraw_lock);
	return (*xdr_args) (&srp->xdr_stream, args_ptr);
}

 /*ARGSUSED*/
static void
svc_raw_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
}

 /*ARGSUSED*/
static bool
svc_raw_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	return (false);
}

static void
svc_raw_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	extern mutex_t ops_lock;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_raw_recv;
		ops.xp_stat = svc_raw_stat;
		ops.xp_getargs = svc_raw_getargs;
		ops.xp_reply = svc_raw_reply;
		ops.xp_freeargs = svc_raw_freeargs;
		ops.xp_destroy = svc_raw_destroy;
		ops.xp_control = svc_raw_control;
		ops.xp_lock = NULL;	/* no default */
		ops.xp_unlock = NULL;	/* no default */
		ops.xp_getreq = svc_getreq_default;
		ops.xp_dispatch = svc_dispatch_default;
		ops.xp_recv_user_data = NULL;	/* no default */
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}
