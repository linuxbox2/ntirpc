/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * contributeur : William Allen Simpson <bill@cohortfs.com>
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
 * Implements connection server side RPC/RDMA.
 */

#include <config.h>

#include <sys/cdefs.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/poll.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h> /* before rpc.h */
#endif
#include <rpc/rpc.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netconfig.h>
#include <err.h>

#include "rpc_com.h"
#include "rpc_ctx.h"
#include "misc/city.h"
#include "rpc/xdr_inrec.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_rdma.h"
#include <rpc/svc_rqst.h>
#include <rpc/svc_auth.h>

/*
 * kept in xprt->xp_p2 (sm_data(xprt))
 */
struct svc_rdma_xdr {
	XDR	sm_xdrs;			/* XDR handle *MUST* be top */
	char	sm_verfbody[MAX_AUTH_BYTES];	/* verifier body */

	struct msghdr   sm_msghdr;		/* msghdr received from	clnt */
	unsigned char   sm_cmsg[64];		/* cmsghdr received from clnt */
	size_t		sm_iosz;		/* size of send.recv buffer */
};

#define	sm_data(xprt)	((struct svc_rdma_xdr *)(xprt->xp_p2))

static void svc_rdma_ops(SVCXPRT *);

/*
 * svc_rdma_ncreate: waits for connection request and returns transport
 */
SVCXPRT *
svc_rdma_ncreate(void *arg, const u_int sendsize, const u_int recvsize,
		 const u_int flags)
{
	struct svc_rdma_xdr *sm;
	struct sockaddr_storage *ss;
	RDMAXPRT *l_xprt = arg;
	RDMAXPRT *xprt = rpc_rdma_accept_wait(l_xprt,
						__svc_params->idle_timeout);

	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s:%u ERROR (return)",
			__func__, __LINE__);
		return (NULL);
	}

	sm = mem_zalloc(sizeof (*sm));

	sm->sm_xdrs.x_lib[1] = xprt;
	xprt->xprt.xp_p2 = sm;

	xprt->xprt.xp_flags = flags;
	/* fixme: put something here, but make it not work on fd operations. */
	xprt->xprt.xp_fd = -1;

	ss = (struct sockaddr_storage *)rdma_get_local_addr(xprt->cm_id);
	__rpc_set_address(&xprt->xprt.xp_local, ss, 0);

	ss = (struct sockaddr_storage *)rdma_get_peer_addr(xprt->cm_id);
	__rpc_set_address(&xprt->xprt.xp_remote, ss, 0);

	svc_rdma_ops(&xprt->xprt);

	if (xdr_rdma_create(&sm->sm_xdrs, xprt, sendsize, recvsize, flags)) {
		goto freedata;
	}

	if (rpc_rdma_accept_finalize(xprt)) {
		goto freedata;
	}

	return (&xprt->xprt);

freedata:
	mem_free(sm, sizeof (*sm));
	xprt->xprt.xp_p2 = NULL;
	xprt_unregister(&xprt->xprt);
	return (NULL);
}

/*ARGSUSED*/
static enum xprt_stat
svc_rdma_stat(SVCXPRT *xprt)
{
	/* note: RDMAXPRT is this xprt! */
	switch(((RDMAXPRT *)xprt)->state) {
		case RDMAXS_LISTENING:
		case RDMAXS_CONNECTED:
			return (XPRT_IDLE);
			/* any point in adding a break? */
		default: /* suppose anything else means a problem */
			return (XPRT_DIED);
	}
}

static bool
svc_rdma_recv(struct svc_req *req)
{
	struct rpc_rdma_cbc *cbc = req->rq_context;
	XDR *xdrs = cbc->holdq.xdrs;

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p incoming xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	rpc_msg_init(&req->rq_msg);

	if (!xdr_rdma_svc_recv(cbc, 0)){
		__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
			"%s: xdr_rdma_svc_recv failed",
			__func__);
		return (FALSE);
	}
	xdrs->x_op = XDR_DECODE;

	/* No need, already positioned to beginning ...
	XDR_SETPOS(xdrs, 0);
	 */
	if (!xdr_dplx_decode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
			"%s: xdr_dplx_decode failed",
			__func__);
		return (FALSE);
	}

	/* the checksum */
	req->rq_cksum = 0;

	return (TRUE);
}

static bool
svc_rdma_reply(struct svc_req *req)
{
	struct rpc_rdma_cbc *cbc = req->rq_context;
	XDR *xdrs = cbc->holdq.xdrs;
	xdrproc_t proc;
	void *where;
	bool has_args;

	__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
		"%s() xprt %p req %p cbc %p outgoing xdr %p\n",
		__func__, req->rq_xprt, req, cbc, xdrs);

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	 && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS) {
	    has_args = TRUE;
	    proc = req->rq_msg.RPCM_ack.ar_results.proc;
	    where = req->rq_msg.RPCM_ack.ar_results.where;
	    req->rq_msg.RPCM_ack.ar_results.proc = (xdrproc_t)xdr_void;
	    req->rq_msg.RPCM_ack.ar_results.where = NULL;
	} else {
	    has_args = FALSE;
	    proc = NULL;
	    where = NULL;
	}

	if (!xdr_rdma_svc_reply(cbc, 0)){
		__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
			"%s: xdr_rdma_svc_reply failed (will set dead)",
			__func__);
		return (FALSE);
	}
	xdrs->x_op = XDR_ENCODE;

	if (!xdr_reply_encode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
			"%s: xdr_reply_encode failed (will set dead)",
			__func__);
		return (FALSE);
	}
	xdr_tail_update(xdrs);

	if (has_args && req->rq_auth
	  && !SVCAUTH_WRAP(req->rq_auth, req, xdrs, proc, where)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RDMA,
			"%s: SVCAUTH_WRAP failed (will set dead)",
			__func__);
		return (FALSE);
	}
	xdr_tail_update(xdrs);

	return xdr_rdma_svc_flushout(cbc);
}

static bool
svc_rdma_freeargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr)
{
	struct rpc_rdma_cbc *cbc = req->rq_context;
	XDR *xdrs = cbc->holdq.xdrs;

	xdrs->x_op = XDR_FREE;
	return (*xdr_args)(xdrs, args_ptr);
}

static bool
svc_rdma_getargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr,
		 void *u_data)
{
	struct rpc_rdma_cbc *cbc = req->rq_context;
	XDR *xdrs = cbc->holdq.xdrs;
	bool rslt;

	/* threads u_data for advanced decoders*/
	xdrs->x_public = u_data;

	rslt = SVCAUTH_UNWRAP(req->rq_auth, req, xdrs, xdr_args, args_ptr);
	if (!rslt)
		svc_rdma_freeargs(req, xdr_args, args_ptr);

	return (rslt);
}

static void
svc_rdma_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct svc_rdma_xdr *sm = sm_data(xprt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refs %" PRId32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refs, tag, line);

	xdr_rdma_destroy(&(sm->sm_xdrs));
	mem_free(sm, sizeof (*sm));

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}

	rpc_rdma_destroy(xprt);
}

extern mutex_t ops_lock;

static void
svc_rdma_lock(SVCXPRT *xprt, uint32_t flags, const char *file, int line)
{
/* pretend we lock for now */
}

static void
svc_rdma_unlock(SVCXPRT *xprt, uint32_t flags, const char *file, int line)
{
}

static bool
/*ARGSUSED*/
svc_rdma_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
	    *(u_int *)in = xprt->xp_flags;
	    break;
	case SVCSET_XP_FLAGS:
	    xprt->xp_flags = *(u_int *)in;
	    break;
	case SVCGET_XP_RECV:
	    mutex_lock(&ops_lock);
	    *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_RECV:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_recv = *(xp_recv_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCGET_XP_GETREQ:
	    mutex_lock(&ops_lock);
	    *(xp_getreq_t *)in = xprt->xp_ops->xp_getreq;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_GETREQ:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_getreq = *(xp_getreq_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCGET_XP_DISPATCH:
	    mutex_lock(&ops_lock);
	    *(xp_dispatch_t *)in = xprt->xp_ops->xp_dispatch;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_DISPATCH:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_dispatch = *(xp_dispatch_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCGET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    *(xp_free_user_data_t *)in = xprt->xp_ops->xp_free_user_data;
	    mutex_unlock(&ops_lock);
	    break;
	case SVCSET_XP_FREE_USER_DATA:
	    mutex_lock(&ops_lock);
	    xprt->xp_ops->xp_free_user_data = *(xp_free_user_data_t)in;
	    mutex_unlock(&ops_lock);
	    break;
	default:
	    return (FALSE);
	}
	return (TRUE);
}

static void
svc_rdma_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */

	mutex_lock(&ops_lock);

	/* Fill in type of service */
	xprt->xp_type = XPRT_RDMA;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_rdma_recv;
		ops.xp_stat = svc_rdma_stat;
		ops.xp_getargs = svc_rdma_getargs;
		ops.xp_reply = svc_rdma_reply;
		ops.xp_freeargs = svc_rdma_freeargs;
		ops.xp_destroy = svc_rdma_destroy,
		ops.xp_control = svc_rdma_control;
		ops.xp_lock = svc_rdma_lock;
		ops.xp_unlock = svc_rdma_unlock;
		ops.xp_getreq = svc_getreq_default;
		ops.xp_dispatch = svc_dispatch_default;
		ops.xp_recv_user_data = NULL;	/* no default */
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;

	mutex_unlock(&ops_lock);
}
