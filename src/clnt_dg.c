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
 * Implements a connectionless client side RPC.
 */
#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <rpc/clnt.h>
#include <arpa/inet.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>

#ifdef IP_RECVERR
#include <asm/types.h>
#include <linux/errqueue.h>
#include <sys/uio.h>
#endif

#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <rpc/svc_rqst.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "rpc_ctx.h"

#define MAX_DEFAULT_FDS                 20000

static struct clnt_ops *clnt_dg_ops(void);
static bool time_not_ok(struct timeval *);
static enum clnt_stat clnt_dg_call(CLIENT *, AUTH *, rpcproc_t, xdrproc_t,
				   void *, xdrproc_t, void *, struct timeval);
static void clnt_dg_geterr(CLIENT *, struct rpc_err *);
static bool clnt_dg_freeres(CLIENT *, xdrproc_t, void *);
static bool clnt_dg_ref(CLIENT *, u_int);
static void clnt_dg_release(CLIENT *, u_int flags);
static void clnt_dg_abort(CLIENT *);
static bool clnt_dg_control(CLIENT *, u_int, void *);
static void clnt_dg_destroy(CLIENT *);

/*
 * Connection less client creation returns with client handle parameters.
 * Default options are set, which the user can change using clnt_control().
 * fd should be open and bound.
 *
 * sendsz and recvsz are the maximum allowable packet sizes that can be
 * sent and received. Normally they are the same, but they can be
 * changed to improve the program efficiency and buffer allocation.
 * If they are 0, use the transport default.
 *
 * If svcaddr is NULL, returns NULL.
 */
CLIENT *
clnt_dg_ncreatef(const int fd,	/* open file descriptor */
		 const struct netbuf *svcaddr,	/* servers address */
		 const rpcprog_t program,	/* program number */
		 const rpcvers_t version,	/* version number */
		 const u_int sendsz,	/* buffer recv size */
		 const u_int recvsz,	/* buffer send size */
		 const uint32_t flags)
{
	CLIENT *clnt;		/* client handle */
	struct cx_data *cx;
	struct cu_data *cu;
	SVCXPRT *xprt;
	struct svc_dg_xprt *su;
	struct timespec now;
	struct rpc_msg call_msg;
	int one = 1;

	if (svcaddr == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR;
		return (NULL);
	}

	/* find or create shared fd state; ref+1 */
	xprt = svc_dg_ncreatef(fd, sendsz, recvsz, flags);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d svc_dg_ncreatef failed",
			__func__, fd);
		rpc_createerr.cf_stat = RPC_TLIERROR;	/* XXX */
		rpc_createerr.cf_error.re_errno = 0;
		return (NULL);
	}
	su = su_data(xprt);

	/* buffer sizes should match svc side */
	cx = alloc_cx_data(CX_DG_DATA, su->su_dr.sendsz, su->su_dr.recvsz);
	cx->cx_rec = &su->su_dr;
	cu = CU_DATA(cx);

	(void)memcpy(&cu->cu_raddr, svcaddr->buf, (size_t) svcaddr->len);
	cu->cu_rlen = svcaddr->len;

	/* Other values can also be set through clnt_control() */
	cu->cu_wait.tv_sec = 15;	/* heuristically chosen */
	cu->cu_wait.tv_usec = 0;
	cu->cu_total.tv_sec = -1;
	cu->cu_total.tv_usec = -1;
	cu->cu_async = false;
	cu->cu_connect = false;
	cu->cu_connected = false;

	/*
	 * initialize call message
	 */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &now);
	call_msg.rm_xid = __RPC_GETXID(&now);	/* XXX? */
	call_msg.cb_prog = program;
	call_msg.cb_vers = version;

	xdrmem_create(&(cu->cu_outxdrs), cu->cu_outbuf, su->su_dr.sendsz,
		      XDR_ENCODE);
	if (!xdr_callhdr(&(cu->cu_outxdrs), &call_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d xdr_callhdr failed",
			__func__, fd);
		rpc_createerr.cf_stat = RPC_CANTENCODEARGS;	/* XXX */
		rpc_createerr.cf_error.re_errno = 0;
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		free_cx_data(cx);
		return (NULL);
	}
	cu->cu_xdrpos = XDR_GETPOS(&(cu->cu_outxdrs));

	/* XXX fvdl - do we still want this? */
#if 0
	(void)bindresvport_sa(fd, (struct sockaddr *)svcaddr->buf);
#endif
#ifdef IP_RECVERR
	{
		int on = 1;
		(void) setsockopt(fd, SOL_IP, IP_RECVERR, &on, sizeof(on));
	}
#endif
	ioctl(fd, FIONBIO, (char *)(void *)&one);

	clnt = &cx->cx_c;
	clnt->cl_ops = clnt_dg_ops();

	__warnx(TIRPC_DEBUG_FLAG_CLNT_DG,
		"%s: fd %d completed",
		__func__, fd);
	return (clnt);
}

static enum clnt_stat
clnt_dg_call(CLIENT *clnt,	/* client handle */
	     AUTH *auth,	/* auth handle */
	     rpcproc_t proc,	/* procedure number */
	     xdrproc_t xargs,	/* xdr routine for args */
	     void *argsp,	/* pointer to args */
	     xdrproc_t xresults,	/* xdr routine for results */
	     void *resultsp,	/* pointer to results */
	     struct timeval utimeout
	     /* seconds to wait before giving up */)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	SVCXPRT *xprt = &rec->xprt;
	XDR *xdrs;
	struct sockaddr *sa;
	struct rpc_msg reply_msg;
	XDR reply_xdrs;
	struct timeval timeout;
	struct pollfd fd;
	int total_time, nextsend_time, tv = 0;
	socklen_t __attribute__ ((unused)) inlen, salen;
	size_t outlen = 0;
	ssize_t recvlen = 0;
	int nrefreshes = 2;	/* number of times to refresh cred */
	int xp_fd = xprt->xp_fd;
	u_int32_t xid, inval, outval;
	bool ok;
	bool slocked = true;
	bool rlocked = false;
	bool once = true;

	/* Need lock for cu.
	 */
	mutex_lock(&clnt->cl_lock);

	if (cu->cu_total.tv_usec == -1)
		timeout = utimeout;	/* use supplied timeout */
	else
		timeout = cu->cu_total;	/* use default timeout */
	total_time = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
	nextsend_time = cu->cu_wait.tv_sec * 1000 + cu->cu_wait.tv_usec / 1000;

	if (cu->cu_connect && !cu->cu_connected) {
		if (connect
		    (xp_fd, (struct sockaddr *)&cu->cu_raddr,
		     cu->cu_rlen) < 0) {
			cx->cx_error.re_errno = errno;
			cx->cx_error.re_status = RPC_CANTSEND;
			goto out;
		}
		cu->cu_connected = 1;
	}
	if (cu->cu_connected) {
		sa = NULL;
		salen = 0;
	} else {
		sa = (struct sockaddr *)&cu->cu_raddr;
		salen = cu->cu_rlen;
	}

	/* Clean up in case the last call ended in a longjmp(3) call. */
 call_again:
	if (!slocked) {
		slocked = true;
		mutex_lock(&clnt->cl_lock);
	}
	xdrs = &(cu->cu_outxdrs);
	if (cu->cu_async == true && xargs == NULL) {
		mutex_unlock(&clnt->cl_lock);
		slocked = false;
		goto get_reply;
	}
	xdrs->x_op = XDR_ENCODE;
	XDR_SETPOS(xdrs, cu->cu_xdrpos);
	/*
	 * the transaction is the first thing in the out buffer
	 * XXX Yes, and it's in network byte order, so we should to
	 * be careful when we increment it, shouldn't we.
	 */
	xid = ntohl(*(u_int32_t *) (void *)(cu->cu_outbuf));
	xid++;
	*(u_int32_t *) (void *)(cu->cu_outbuf) = htonl(xid);

	if ((!XDR_PUTINT32(xdrs, (int32_t *) &proc))
	    || (!AUTH_MARSHALL(auth, xdrs))
	    || (!AUTH_WRAP(auth, xdrs, xargs, argsp))) {
		cx->cx_error.re_status = RPC_CANTENCODEARGS;
		goto out;
	}
	outlen = (size_t) XDR_GETPOS(xdrs);
	mutex_unlock(&clnt->cl_lock);
	slocked = false;

 send_again:
	nextsend_time = cu->cu_wait.tv_sec * 1000 + cu->cu_wait.tv_usec / 1000;
	if (sendto(xp_fd, cu->cu_outbuf, outlen, 0, sa, salen) != outlen) {
		cx->cx_error.re_errno = errno;
		cx->cx_error.re_status = RPC_CANTSEND;
		goto out;
	}

 get_reply:
	/*
	 * sub-optimal code appears here because we have
	 * some clock time to spare while the packets are in flight.
	 * (We assume that this is actually only executed once.)
	 */
	rpc_dplx_rli(rec);
	rlocked = true;

	if (!rec->ev_p)
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);

	reply_msg.RPCM_ack.ar_verf = _null_auth;
	reply_msg.RPCM_ack.ar_results.where = NULL;
	reply_msg.RPCM_ack.ar_results.proc = (xdrproc_t) xdr_void;

	fd.fd = xp_fd;
	fd.events = POLLIN;
	fd.revents = 0;
	while ((total_time > 0) || once) {
		tv = total_time < nextsend_time ? total_time : nextsend_time;
		once = false;
		switch (poll(&fd, 1, tv)) {
		case 0:
			total_time -= tv;
			rpc_dplx_rui(rec);
			rlocked = false;
			if (total_time <= 0) {
				cx->cx_error.re_status = RPC_TIMEDOUT;
				goto out;
			}
			goto send_again;
		case -1:
			if (errno == EINTR)
				continue;
			cx->cx_error.re_status = RPC_CANTRECV;
			cx->cx_error.re_errno = errno;
			goto out;
		}
		break;
	}
#ifdef IP_RECVERR
	if (fd.revents & POLLERR) {
		struct msghdr msg;
		struct cmsghdr *cmsg;
		struct sock_extended_err *e;
		struct sockaddr_in err_addr;
		struct sockaddr_in *sin = (struct sockaddr_in *)&cu->cu_raddr;
		struct iovec iov;
		char *cbuf = (char *)alloca(outlen + 256);
		int ret;

		iov.iov_base = cbuf + 256;
		iov.iov_len = outlen;
		msg.msg_name = (void *)&err_addr;
		msg.msg_namelen = sizeof(err_addr);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_flags = 0;
		msg.msg_control = cbuf;
		msg.msg_controllen = 256;
		ret = recvmsg(xp_fd, &msg, MSG_ERRQUEUE);
		if (ret >= 0 && memcmp(cbuf + 256, cu->cu_outbuf, ret) == 0
		    && (msg.msg_flags & MSG_ERRQUEUE)
		    && ((msg.msg_namelen == 0 && ret >= 12)
			|| (msg.msg_namelen == sizeof(err_addr)
			    && err_addr.sin_family == AF_INET
			    && memcmp(&err_addr.sin_addr, &sin->sin_addr,
				      sizeof(err_addr.sin_addr)) == 0
			    && err_addr.sin_port == sin->sin_port)))
			for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
			     cmsg = CMSG_NXTHDR(&msg, cmsg))
				if ((cmsg->cmsg_level == SOL_IP)
				    && (cmsg->cmsg_type == IP_RECVERR)) {
					e = (struct sock_extended_err *)
					    CMSG_DATA(cmsg);
					cx->cx_error.re_errno = e->ee_errno;
					cx->cx_error.re_status = RPC_CANTRECV;
				}
	}
#endif

	/* We have some data now */
	do {
		recvlen =
		    recvfrom(xp_fd, cu->cu_inbuf, cu->cu_recvsz, 0, NULL,
			     NULL);
	} while (recvlen < 0 && errno == EINTR);
	if (recvlen < 0 && errno != EWOULDBLOCK) {
		cx->cx_error.re_errno = errno;
		cx->cx_error.re_status = RPC_CANTRECV;
		goto out;
	}

	if (recvlen < sizeof(u_int32_t)) {
		total_time -= tv;
		rpc_dplx_rui(rec);
		rlocked = false;
		goto send_again;
	}

	if (cu->cu_async == true)
		inlen = (socklen_t) recvlen;
	else {
		memcpy(&inval, cu->cu_inbuf, sizeof(u_int32_t));
		memcpy(&outval, cu->cu_outbuf, sizeof(u_int32_t));
		if (inval != outval) {
			total_time -= tv;
			rpc_dplx_rui(rec);
			rlocked = false;
			goto send_again;
		}
		inlen = (socklen_t) recvlen;
	}

	/*
	 * now decode and validate the response
	 */

	xdrmem_create(&reply_xdrs, cu->cu_inbuf, (u_int) recvlen, XDR_DECODE);
	ok = xdr_replymsg(&reply_xdrs, &reply_msg);
	/* XDR_DESTROY(&reply_xdrs); save a few cycles on noop destroy */
	if (ok) {
		if ((reply_msg.rm_reply.rp_stat == MSG_ACCEPTED)
		    && (reply_msg.RPCM_ack.ar_stat == SUCCESS))
			cx->cx_error.re_status = RPC_SUCCESS;
		else
			_seterr_reply(&reply_msg, &(cx->cx_error));

		if (cx->cx_error.re_status == RPC_SUCCESS) {
			if (!AUTH_VALIDATE
			    (auth, &reply_msg.RPCM_ack.ar_verf)) {
				cx->cx_error.re_status = RPC_AUTHERROR;
				cx->cx_error.re_why = AUTH_INVALIDRESP;
			} else
			    if (!AUTH_UNWRAP
				(auth, &reply_xdrs, xresults, resultsp)) {
				if (cx->cx_error.re_status == RPC_SUCCESS)
					cx->cx_error.re_status =
					    RPC_CANTDECODERES;
			}
		}
		/* end successful completion */
		/*
		 * If unsuccesful AND error is an authentication error
		 * then refresh credentials and try again, else break
		 */
		else if (cx->cx_error.re_status == RPC_AUTHERROR)
			/* maybe our credentials need to be refreshed ... */
			if (nrefreshes > 0 && AUTH_REFRESH(auth, &reply_msg)) {
				nrefreshes--;
				rpc_dplx_rui(rec);
				rlocked = false;
				goto call_again;
			}
		/* end of unsuccessful completion */
	} /* end of valid reply message */
	else
		cx->cx_error.re_status = RPC_CANTDECODERES;

out:
	if (slocked)
		mutex_unlock(&clnt->cl_lock);
	if (rlocked)
		rpc_dplx_rui(rec);

	return (cx->cx_error.re_status);
}

static void
clnt_dg_geterr(CLIENT *clnt, struct rpc_err *errp)
{
	struct cx_data *cx = CX_DATA(clnt);

	*errp = cx->cx_error;
}

static bool
clnt_dg_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	XDR *xdrs;

	/* XXX guard against illegal invocation from libc (will fix) */
	if (!xdr_res)
		return (0);

	xdrs = &(cu->cu_outxdrs);	/* XXX outxdrs? */
	xdrs->x_op = XDR_FREE;

	return ((*xdr_res)(xdrs, res_ptr));
}

static bool
clnt_dg_ref(CLIENT *clnt, u_int flags)
{
	return (true);
}

static void
clnt_dg_release(CLIENT *clnt, u_int flags)
{
}

 /*ARGSUSED*/
static void
clnt_dg_abort(CLIENT *h)
{
}

static bool
clnt_dg_control(CLIENT *clnt, u_int request, void *info)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct netbuf *addr;
	bool rslt = true;

	/* always take recv lock first, if taking both locks together */
	rpc_dplx_rli(rec);
	mutex_lock(&clnt->cl_lock);

	/*
	 * By default, SVC_XPRT_FLAG_CLOSE starts false.  It is user
	 * responsibility to do a close on the fd, or set by cl_control.
	 */
	switch (request) {
	case CLSET_FD_CLOSE:
		(void)atomic_set_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		rslt = true;
		goto unlock;
	case CLSET_FD_NCLOSE:
		(void)atomic_clear_uint16_t_bits(&rec->xprt.xp_flags,
						SVC_XPRT_FLAG_CLOSE);
		rslt = true;
		goto unlock;
	}

	/* for other requests which use info */
	if (info == NULL) {
		rslt = false;
		goto unlock;
	}
	switch (request) {
	case CLSET_TIMEOUT:
		if (time_not_ok((struct timeval *)info)) {
			rslt = false;
			goto unlock;
		}
		cu->cu_total = *(struct timeval *)info;
		break;
	case CLGET_TIMEOUT:
		*(struct timeval *)info = cu->cu_total;
		break;
	case CLGET_SERVER_ADDR:	/* Give him the fd address */
		/* Now obsolete. Only for backward compatibility */
		(void)memcpy(info, &cu->cu_raddr, (size_t) cu->cu_rlen);
		break;
	case CLSET_RETRY_TIMEOUT:
		if (time_not_ok((struct timeval *)info)) {
			rslt = false;
			goto unlock;
		}
		cu->cu_wait = *(struct timeval *)info;
		break;
	case CLGET_RETRY_TIMEOUT:
		*(struct timeval *)info = cu->cu_wait;
		break;
	case CLGET_FD:
		*(int *)info = rec->xprt.xp_fd;
		break;
	case CLGET_SVC_ADDR:
		addr = (struct netbuf *)info;
		addr->buf = &cu->cu_raddr;
		addr->len = cu->cu_rlen;
		addr->maxlen = sizeof(cu->cu_raddr);
		break;
	case CLSET_SVC_ADDR:	/* set to new address */
		addr = (struct netbuf *)info;
		if (addr->len < sizeof(cu->cu_raddr)) {
			rslt = false;
			goto unlock;

		}
		(void)memcpy(&cu->cu_raddr, addr->buf, addr->len);
		cu->cu_rlen = addr->len;
		break;
	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure *.
		 * This will get the xid of the PREVIOUS call
		 */
		*(u_int32_t *) info =
		    ntohl(*(u_int32_t *) (void *)cu->cu_outbuf);
		break;

	case CLSET_XID:
		/* This will set the xid of the NEXT call */
		*(u_int32_t *) (void *)cu->cu_outbuf =
		    htonl(*(u_int32_t *) info - 1);
		/* decrement by 1 as clnt_dg_call() increments once */
		break;

	case CLGET_VERS:
		/*
		 * This RELIES on the information that, in the call body,
		 * the version number field is the fifth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		*(u_int32_t *) info = ntohl(*(u_int32_t *) (void *)
					    (cu->cu_outbuf +
					     4 * BYTES_PER_XDR_UNIT));
		break;

	case CLSET_VERS:
		*(u_int32_t *) (void *)(cu->cu_outbuf + 4 * BYTES_PER_XDR_UNIT)
		    = htonl(*(u_int32_t *) info);
		break;

	case CLGET_PROG:
		/*
		 * This RELIES on the information that, in the call body,
		 * the program number field is the fourth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		*(u_int32_t *) info = ntohl(*(u_int32_t *) (void *)
					    (cu->cu_outbuf +
					     3 * BYTES_PER_XDR_UNIT));
		break;

	case CLSET_PROG:
		*(u_int32_t *) (void *)(cu->cu_outbuf + 3 * BYTES_PER_XDR_UNIT)
		    = htonl(*(u_int32_t *) info);
		break;
	case CLSET_ASYNC:
		cu->cu_async = *(int *)info;
		break;
	case CLSET_CONNECT:
		cu->cu_connect = *(int *)info;
		break;
	default:
		break;
	}

 unlock:
	rpc_dplx_rui(rec);
	mutex_unlock(&clnt->cl_lock);

	return (rslt);
}

static void
clnt_dg_destroy(CLIENT *clnt)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct cu_data *cu = CU_DATA(cx);
	struct rpc_dplx_rec *rec = cx->cx_rec;

	XDR_DESTROY(&cu->cu_outxdrs);

	/* release */
	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
	free_cx_data(cx);
}

static struct clnt_ops *
clnt_dg_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock; /* XXXX does it need to be extern? */
	sigset_t mask;
	sigset_t newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_dg_call;
		ops.cl_abort = clnt_dg_abort;
		ops.cl_geterr = clnt_dg_geterr;
		ops.cl_freeres = clnt_dg_freeres;
		ops.cl_ref = clnt_dg_ref;
		ops.cl_release = clnt_dg_release;
		ops.cl_destroy = clnt_dg_destroy;
		ops.cl_control = clnt_dg_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
	return (&ops);
}

/*
 * Make sure that the time is not garbage.  -1 value is allowed.
 */
static bool
time_not_ok(struct timeval *t)
{
	return (t->tv_sec < -1 || t->tv_sec > 100000000 || t->tv_usec < -1
		|| t->tv_usec > 1000000);
}
