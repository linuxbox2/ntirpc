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

#ifdef IP_RECVERR
#include <asm/types.h>
#include <linux/errqueue.h>
#include <sys/uio.h>
#endif

#include "clnt_internal.h"
#include "rpc_rdma.h"
#include "rpc_dplx_internal.h"

#define MAX_DEFAULT_FDS		 20000

static struct clnt_ops *clnt_rdma_ops(void);

/*
 * Make sure that the time is not garbage.  -1 value is allowed.
 */
static bool
time_not_ok(struct timeval *t)
{
	return (t->tv_sec < -1 || t->tv_sec > 100000000 ||
		t->tv_usec < -1 || t->tv_usec > 1000000);
}

/*
 * client mooshika create
 */
CLIENT *
clnt_rdma_create(RDMAXPRT *xprt,		/* init but NOT connect()ed descriptor */
		 rpcprog_t program,		/* program number */
		 rpcvers_t version,
		 const u_int flags)
{
	CLIENT *cl = NULL;		/* client handle */
	struct cx_data *cx = NULL;	/* private data */

	struct cm_data *cm = NULL;
	struct timeval now;

	if (!xprt || xprt->state != RDMAXS_INITIAL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR; /* FIXME, add a warnx? */
		rpc_createerr.cf_error.re_errno = 0;
		return (NULL);
	}

	/*
	 * Find the receive and the send size
	 */
//	u_int sendsz = 8*1024;
//	u_int recvsz = 4*8*1024;
	u_int sendsz = 1024;
	u_int recvsz = 1024;

	cl = mem_alloc(sizeof (CLIENT));
	/*
	 * Should be multiple of 4 for XDR.
	 */
	cx = alloc_cx_data(CX_MSK_DATA, sendsz, recvsz);
	cm = CM_DATA(cx);
	/* Other values can also be set through clnt_control() */
	cm->cm_xdrs.x_lib[1] = (void *)xprt;
	cm->cm_wait.tv_sec = 15; /* heuristically chosen */
	cm->cm_wait.tv_usec = 0;

	(void) gettimeofday(&now, NULL);
	//	cm->call_msg.rm_xid = __RPC_GETXID(&now);
	cm->call_msg.rm_xid = 1;
	cm->call_msg.cb_prog = program;
	cm->call_msg.cb_vers = version;

	rpc_rdma_connect(xprt);

	xdr_rdma_create(&cm->cm_xdrs, xprt, sendsz, recvsz, flags);

	rpc_rdma_connect_finalize(xprt);

	/*
	 * By default, closeit is always FALSE. It is users responsibility
	 * to do a close on it, else the user may use clnt_control
	 * to let clnt_destroy do it for him/her.
	 */
	cm->cm_closeit = FALSE;
	cl->cl_ops = clnt_rdma_ops();
	//	cl->cl_private = (caddr_t)(void *) cx;
	cl->cl_p1 =  (caddr_t)(void *) cx;
	cl->cl_p2 =  NULL;
	//	cl->cl_p2 =  rec;
	//	cl->cl_auth = authnone_create();
	cl->cl_tp = NULL;
	cl->cl_netid = NULL;

	return (cl);
}

static enum clnt_stat
clnt_rdma_call(CLIENT *cl,		/* client handle */
	       AUTH *auth,
	       rpcproc_t proc,		/* procedure number */
	       xdrproc_t xargs,		/* xdr routine for args */
	       void *argsp,		/* pointer to args */
	       xdrproc_t xresults,	/* xdr routine for results */
	       void *resultsp,		/* pointer to results */
	       struct timeval utimeout	/* seconds to wait before giving up */)
{
	struct cm_data *cm = CM_DATA((struct cx_data *) cl->cl_p1);
	XDR *xdrs;
	struct rpc_msg reply_msg;
	bool ok;
#if 0
	struct timeval timeout;
	int total_time;
#endif
//	sigset_t mask;
	socklen_t  __attribute__((unused)) inlen, salen;
	int nrefreshes = 2;		/* number of times to refresh cred */

//	thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask); /* XXX */
//	vc_fd_lock_c(cl, &mask); //What does that do?
#if 0
	if (cm->cm_total.tv_usec == -1) {
		timeout = utimeout;	/* use supplied timeout */
	} else {
		timeout = cm->cm_total;	/* use default timeout */
	}
	total_time = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
#endif

	/* Clean up in case the last call ended in a longjmp(3) call. */
call_again:
	xdrs = &(cm->cm_xdrs);

	if (0) //FIXME check for async
		goto get_reply;

	if (! xdr_rdma_clnt_call(&cm->cm_xdrs, cm->call_msg.rm_xid) ||
	    ! xdr_callhdr(&(cm->cm_xdrs), &cm->call_msg)) {
		rpc_createerr.cf_stat = RPC_CANTENCODEARGS;  /* XXX */
		rpc_createerr.cf_error.re_errno = 0;
		goto out;
	}

	if ((! XDR_PUTINT32(xdrs, (int32_t *)&proc)) ||
	    (! AUTH_MARSHALL(auth, xdrs)) ||
	    (! AUTH_WRAP(auth, xdrs, xargs, argsp))) {
		cm->cm_error.re_status = RPC_CANTENCODEARGS;
		goto out;
	}

	if (! xdr_rdma_clnt_flushout(&cm->cm_xdrs)) {
		cm->cm_error.re_errno = errno;
		cm->cm_error.re_status = RPC_CANTSEND;
		goto out;
	}

get_reply:

	/*
	 * sub-optimal code appears here because we have
	 * some clock time to spare while the packets are in flight.
	 * (We assume that this is actually only executed once.)
	 */
	reply_msg.RPCM_ack.ar_verf = _null_auth;
	reply_msg.RPCM_ack.ar_results.where = NULL;
	reply_msg.RPCM_ack.ar_results.proc = (xdrproc_t)xdr_void;

	if (! xdr_rdma_clnt_reply(&cm->cm_xdrs, cm->call_msg.rm_xid)) {
		//FIXME add timeout
		cm->cm_error.re_status = RPC_TIMEDOUT;
		goto out;
	}

	/*
	 * now decode and validate the response
	 */

	ok = xdr_replymsg(&cm->cm_xdrs, &reply_msg);
	if (ok) {
		if ((reply_msg.rm_reply.rp_stat == MSG_ACCEPTED) &&
			(reply_msg.RPCM_ack.ar_stat == SUCCESS))
			cm->cm_error.re_status = RPC_SUCCESS;
		else
			_seterr_reply(&reply_msg, &(cm->cm_error));

		if (cm->cm_error.re_status == RPC_SUCCESS) {
			if (! AUTH_VALIDATE(auth,
					    &(reply_msg.RPCM_ack.ar_verf))) {
				cm->cm_error.re_status = RPC_AUTHERROR;
				cm->cm_error.re_why = AUTH_INVALIDRESP;
			} else if (! AUTH_UNWRAP(auth, &cm->cm_xdrs,
						 xresults, resultsp)) {
				if (cm->cm_error.re_status == RPC_SUCCESS)
				     cm->cm_error.re_status = RPC_CANTDECODERES;
			}
		}	/* end successful completion */
		/*
		 * If unsuccesful AND error is an authentication error
		 * then refresh credentials and try again, else break
		 */
		else if (cm->cm_error.re_status == RPC_AUTHERROR)
			/* maybe our credentials need to be refreshed ... */
			if (nrefreshes > 0 &&
			    AUTH_REFRESH(auth, &reply_msg)) {
				nrefreshes--;
				goto call_again;
			}
		/* end of unsuccessful completion */
	}	/* end of valid reply message */
	else {
		cm->cm_error.re_status = RPC_CANTDECODERES;

	}
out:
	cm->call_msg.rm_xid++;

//	vc_fd_unlock_c(cl, &mask);
	return (cm->cm_error.re_status);
}

static void
clnt_rdma_geterr(CLIENT *cl, struct rpc_err *errp)
{
	struct cm_data *cm = CM_DATA((struct cx_data *) cl->cl_p1);
	*errp = cm->cm_error;
}

static bool
clnt_rdma_freeres(CLIENT *cl, xdrproc_t xdr_res, void *res_ptr)
{
	struct cm_data *cm = CM_DATA((struct cx_data *)cl->cl_p1);
	XDR *xdrs;
	sigset_t mask, newmask;
	bool dummy = 0;

	/* XXX guard against illegal invocation from libc (will fix) */
	if (! xdr_res)
		goto out;

	xdrs = &(cm->cm_xdrs);

	/* Handle our own signal mask here, the signal section is
	 * larger than the wait (not 100% clear why) */

	/* barrier recv channel */
	//	rpc_dplx_rwc(clnt, rpc_flag_clear);

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	xdrs->x_op = XDR_FREE;
	if (xdr_res)
		dummy = (*xdr_res)(xdrs, res_ptr);

	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	rpc_dplx_rsc(cl, RPC_DPLX_FLAG_NONE);

out:
	return (dummy);
}

/*ARGSUSED*/
static void
clnt_rdma_abort(CLIENT *h)
{
}

static bool
clnt_rdma_control(CLIENT *cl, u_int request, void *info)
{
	struct cm_data *cm = CM_DATA((struct cx_data *) cl->cl_p1);
	sigset_t mask;
	bool result = TRUE;

	thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask); /* XXX */
	/* always take recv lock first if taking together */
	rpc_dplx_rlc(cl); //receive lock clnt
	rpc_dplx_slc(cl); //send lock clnt

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
	case CLSET_TIMEOUT:
		if (time_not_ok((struct timeval *)info)) {
			result = FALSE;
			goto unlock;
		}
		cm->cm_total = *(struct timeval *)info;
		break;
	case CLGET_TIMEOUT:
		*(struct timeval *)info = cm->cm_total;
		break;
	case CLSET_RETRY_TIMEOUT:
		if (time_not_ok((struct timeval *)info)) {
			result = FALSE;
			goto unlock;
		}
		cm->cm_wait = *(struct timeval *)info;
		break;
	case CLGET_RETRY_TIMEOUT:
		*(struct timeval *)info = cm->cm_wait;
		break;
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

	case CLSET_ASYNC:
		//FIXME cm->cm_async = *(int *)info;
		break;
	case CLSET_CONNECT:
		//FIXMEcm->cm_connect = *(int *)info;
		break;
	default:
		break;
	}

unlock:
	rpc_dplx_ruc(cl);
	rpc_dplx_suc(cl);
	return (result);
}

static void
clnt_rdma_destroy(CLIENT *cl)
{
//	struct cx_data *cx = (struct cx_data *)cl->cl_private;

//FIXME
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
		ops.cl_geterr = clnt_rdma_geterr;
		ops.cl_freeres = clnt_rdma_freeres;
		ops.cl_destroy = clnt_rdma_destroy;
		ops.cl_control = clnt_rdma_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	return (&ops);
}

