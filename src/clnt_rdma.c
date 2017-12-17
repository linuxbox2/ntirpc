/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * Copyright (c) 2015-2017 Red Hat, Inc. and/or its affiliates.
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
 * Implements an msk connection client side RPC.
 */
#include <config.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/poll.h>

#include <sys/time.h>

#include <sys/ioctl.h>
#include <rpc/clnt.h>
#include <arpa/inet.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "rpc_rdma.h"

#define MAX_DEFAULT_FDS		 20000

static struct clnt_ops *clnt_rdma_ops(void);

struct cm_data {
	struct cx_data cm_cx;
	XDR cm_xdrs;
	char *buffers;
	struct rpc_msg call_msg;
	//add a lastreceive?
	u_int cm_xdrpos;
	bool cm_closeit; /* close it on destroy */
};
#define CM_DATA(p) (opr_containerof((p), struct cm_data, cm_cx))

static void
clnt_rdma_data_free(struct cm_data *cm)
{
	clnt_data_destroy(&cm->cm_cx);
	mem_free(cm, sizeof(struct cm_data));
}

static struct cm_data *
clnt_rdma_data_zalloc(void)
{
	struct cm_data *cm = mem_zalloc(sizeof(struct cm_data));

	clnt_data_init(&cm->cm_cx);
	return (cm);
}

/*
 * Create a client handle for a connection.
 *
 * Always returns CLIENT. Must check cl_error.re_status,
 * followed by CLNT_DESTROY() as necessary.
 */
CLIENT *
clnt_rdma_ncreate(RDMAXPRT *xprt,		/* init but NOT connect()ed */
		  rpcprog_t program,		/* program number */
		  rpcvers_t version,
		  const u_int flags)
{
	struct cm_data *cm = clnt_rdma_data_zalloc();
	CLIENT *cl = &cm->cm_cx.cx_c;
	struct timeval now;

	cl->cl_ops = clnt_rdma_ops();

	if (!xprt || xprt->state != RDMAXS_INITIAL) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: called with missing transport address",
			__func__);
		cl->cl_error.re_status = RPC_UNKNOWNADDR;
		return (cl);
	}

	/* Other values can also be set through clnt_control() */
	cm->cm_xdrs.x_lib[1] = (void *)xprt;

	(void) gettimeofday(&now, NULL);
	//	cm->call_msg.rm_xid = __RPC_GETXID(&now);
	cm->call_msg.rm_xid = 1;
	cm->call_msg.cb_prog = program;
	cm->call_msg.cb_vers = version;

	rpc_rdma_connect(xprt);

	xdr_rdma_create(&cm->cm_xdrs, xprt);

	rpc_rdma_connect_finalize(xprt);

	/*
	 * By default, closeit is always FALSE. It is users responsibility
	 * to do a close on it, else the user may use clnt_control
	 * to let clnt_destroy do it for him/her.
	 */
	cm->cm_closeit = FALSE;
	//	cl->cl_auth = authnone_create();

	return (cl);
}

static enum clnt_stat
clnt_rdma_call(struct clnt_req *cc)
{
	CLIENT *cl = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(cl);
	struct cm_data *cm = CM_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	SVCXPRT *xprt = &rec->xprt;
	XDR *xdrs;

	xdrs = &(cm->cm_xdrs);
	cc->cc_error.re_status = RPC_SUCCESS;
	cm->call_msg.rm_xid = cc->cc_xid;

	if (!xdr_rdma_clnt_call(&cm->cm_xdrs, cm->call_msg.rm_xid)
	 || !xdr_callhdr(&(cm->cm_xdrs), &cm->call_msg)
	 || !XDR_PUTUINT32(xdrs, cc->cc_proc)
	 || !AUTH_MARSHALL(cc->cc_auth, xdrs)
	 || !AUTH_WRAP(cc->cc_auth, xdrs,
		       cc->cc_call.proc, cc->cc_call.where)) {
		__warnx(TIRPC_DEBUG_FLAG_CLNT_RDMA,
			"%s: fd %d failed",
			__func__, xprt->xp_fd);
		XDR_DESTROY(xdrs);
		return (RPC_CANTENCODEARGS);
	}

	if (! xdr_rdma_clnt_flushout(&cm->cm_xdrs)) {
		cl->cl_error.re_errno = errno;
		return (RPC_CANTSEND);
	}

	return (RPC_SUCCESS);
}

static bool
clnt_rdma_freeres(CLIENT *cl, xdrproc_t xdr_res, void *res_ptr)
{
	struct cx_data *cx = CX_DATA(cl);
	struct cm_data *cm = CM_DATA(cx);
	XDR *xdrs;

	/* XXX guard against illegal invocation from libc (will fix) */
	if (! xdr_res)
		return (0);

	xdrs = &(cm->cm_xdrs);
	xdrs->x_op = XDR_FREE;

	return ((*xdr_res)(xdrs, res_ptr));
}

/*ARGSUSED*/
static void
clnt_rdma_abort(CLIENT *h)
{
}

static bool
clnt_rdma_control(CLIENT *cl, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(cl);
	struct cm_data *cm = CM_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	sigset_t mask;
	bool result = TRUE;

	thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask); /* XXX */
	/* always take recv lock first if taking together */
	rpc_dplx_rli(rec); //receive lock clnt
	mutex_lock(&cl->cl_lock);

	switch (request) {
	case CLSET_FD_CLOSE:
		cm->cm_closeit = TRUE;
		result = TRUE;
		goto unlock;
	case CLSET_FD_NCLOSE:
		cm->cm_closeit = FALSE;
		result = TRUE;
		goto unlock;
	}

	/* for other requests which use info */
	if (info == NULL) {
	    result = FALSE;
	    goto unlock;
	}
	switch (request) {
	case CLGET_FD:
		*(RDMAXPRT **)info = cm->cm_xdrs.x_lib[1];
		break;
	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure *.
		 * This will get the xid of the PREVIOUS call
		 */
		*(u_int32_t *)info = cm->call_msg.rm_xid - 1;
		break;

	case CLSET_XID:
		/* This will set the xid of the NEXT call */
		cm->call_msg.rm_xid = *(u_int32_t *)info;
		break;

	case CLGET_VERS:
		*(u_int32_t *)info = cm->call_msg.cb_vers;
		break;

	case CLSET_VERS:
		cm->call_msg.cb_vers = *(u_int32_t *)info;
		break;

	case CLGET_PROG:
		*(u_int32_t *)info = cm->call_msg.cb_prog;
		break;

	case CLSET_PROG:
		cm->call_msg.cb_prog = *(u_int32_t *)info;
		break;

	default:
		break;
	}

unlock:
	rpc_dplx_rui(rec);
	mutex_unlock(&cl->cl_lock);
	return (result);
}

static void
clnt_rdma_destroy(CLIENT *clnt)
{
	struct cx_data *cx = CX_DATA(clnt);

	SVC_RELEASE(&cx->cx_rec->xprt, SVC_RELEASE_FLAG_NONE);
	clnt_rdma_data_free(CM_DATA(cx));
}

static struct clnt_ops *
clnt_rdma_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t	ops_lock;
	sigset_t mask;
	sigset_t newmask;

/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_rdma_call;
		ops.cl_abort = clnt_rdma_abort;
		ops.cl_freeres = clnt_rdma_freeres;
		ops.cl_destroy = clnt_rdma_destroy;
		ops.cl_control = clnt_rdma_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	return (&ops);
}

