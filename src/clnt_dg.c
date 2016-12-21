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
#include "rpc_com.h"
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>

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

static const char mem_err_clnt_dg[] = "clnt_dg_create: out of memory";

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
clnt_dg_ncreate(int fd,	/* open file descriptor */
		const struct netbuf *svcaddr,	/* servers address */
		rpcprog_t program,	/* program number */
		rpcvers_t version,	/* version number */
		u_int sendsz,	/* buffer recv size */
		u_int recvsz /* buffer send size */)
{
	CLIENT *clnt = NULL;	/* client handle */
	struct cx_data *cx = NULL;	/* private data */
	struct cu_data *cu = NULL;
	struct timespec now;
	struct rpc_msg call_msg;
	struct __rpc_sockinfo si;
	uint32_t oflags;
	int one = 1;

	if (svcaddr == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR;
		return (NULL);
	}

	if (!__rpc_fd2sockinfo(fd, &si)) {
		rpc_createerr.cf_stat = RPC_TLIERROR;
		rpc_createerr.cf_error.re_errno = 0;
		return (NULL);
	}
	/*
	 * Find the receive and the send size
	 */
	sendsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
	recvsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);
	if ((sendsz == 0) || (recvsz == 0)) {
		rpc_createerr.cf_stat = RPC_TLIERROR;	/* XXX */
		rpc_createerr.cf_error.re_errno = 0;
		return (NULL);
	}

	clnt = mem_alloc(sizeof(CLIENT));

	mutex_init(&clnt->cl_lock, NULL);
	clnt->cl_flags = CLNT_FLAG_NONE;

	/*
	 * Should be multiple of 4 for XDR.
	 */
	sendsz = ((sendsz + 3) / 4) * 4;
	recvsz = ((recvsz + 3) / 4) * 4;
	cx = alloc_cx_data(CX_DG_DATA, sendsz, recvsz);
	if (cx == NULL)
		goto err1;
	cu = CU_DATA(cx);
	(void)memcpy(&cu->cu_raddr, svcaddr->buf, (size_t) svcaddr->len);
	cu->cu_rlen = svcaddr->len;
	/* Other values can also be set through clnt_control() */
	cu->cu_wait.tv_sec = 15;	/* heuristically chosen */
	cu->cu_wait.tv_usec = 0;
	cu->cu_total.tv_sec = -1;
	cu->cu_total.tv_usec = -1;
	cu->cu_sendsz = sendsz;
	cu->cu_recvsz = recvsz;
	cu->cu_async = false;
	cu->cu_connect = false;
	cu->cu_connected = false;
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &now);
	call_msg.rm_xid = __RPC_GETXID(&now);	/* XXX? */
	call_msg.cb_prog = program;
	call_msg.cb_vers = version;
	xdrmem_create(&(cu->cu_outxdrs), cu->cu_outbuf, sendsz, XDR_ENCODE);
	if (!xdr_callhdr(&(cu->cu_outxdrs), &call_msg)) {
		rpc_createerr.cf_stat = RPC_CANTENCODEARGS;	/* XXX */
		rpc_createerr.cf_error.re_errno = 0;
		goto err2;
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
	/*
	 * By default, closeit is always false. It is users responsibility
	 * to do a close on it, else the user may use clnt_control
	 * to let clnt_destroy do it for him/her.
	 */
	cu->cu_closeit = false;
	cu->cu_fd = fd;
	clnt->cl_ops = clnt_dg_ops();
	clnt->cl_p1 = cx;
	clnt->cl_p2 = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_FLAG_NONE,
					  &oflags); /* ref+1 */
	clnt->cl_tp = NULL;
	clnt->cl_netid = NULL;

	return (clnt);
 err1:
	__warnx(TIRPC_DEBUG_FLAG_CLNT_DG, mem_err_clnt_dg);
	rpc_createerr.cf_stat = RPC_SYSTEMERROR;
	rpc_createerr.cf_error.re_errno = errno;
 err2:
	mem_free(clnt, sizeof(CLIENT));
	if (cx)
		free_cx_data(cx);
	return (NULL);
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
	struct cu_data *cu = CU_DATA((struct cx_data *)clnt->cl_p1);
	XDR *xdrs;
	size_t outlen = 0;
	struct rpc_msg reply_msg;
	XDR reply_xdrs;
	bool ok;
	int nrefreshes = 2;	/* number of times to refresh cred */
	struct timeval timeout;
	struct pollfd fd;
	int total_time, nextsend_time, tv = 0;
	struct sockaddr *sa;
	socklen_t __attribute__ ((unused)) inlen, salen;
	ssize_t recvlen = 0;
	u_int32_t xid, inval, outval;
	bool slocked = false;
	bool rlocked = false;
	bool once = true;

	outlen = 0;
	rpc_dplx_slc(clnt);
	slocked = true;
	if (cu->cu_total.tv_usec == -1)
		timeout = utimeout;	/* use supplied timeout */
	else
		timeout = cu->cu_total;	/* use default timeout */
	total_time = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
	nextsend_time = cu->cu_wait.tv_sec * 1000 + cu->cu_wait.tv_usec / 1000;

	if (cu->cu_connect && !cu->cu_connected) {
		if (connect
		    (cu->cu_fd, (struct sockaddr *)&cu->cu_raddr,
		     cu->cu_rlen) < 0) {
			cu->cu_error.re_errno = errno;
			cu->cu_error.re_status = RPC_CANTSEND;
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
		rpc_dplx_slc(clnt);
		slocked = true;
	}
	xdrs = &(cu->cu_outxdrs);
	if (cu->cu_async == true && xargs == NULL)
		goto get_reply;
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
		cu->cu_error.re_status = RPC_CANTENCODEARGS;
		goto out;
	}
	outlen = (size_t) XDR_GETPOS(xdrs);

 send_again:
	nextsend_time = cu->cu_wait.tv_sec * 1000 + cu->cu_wait.tv_usec / 1000;
	if (sendto(cu->cu_fd, cu->cu_outbuf, outlen, 0, sa, salen) != outlen) {
		cu->cu_error.re_errno = errno;
		cu->cu_error.re_status = RPC_CANTSEND;
		goto out;
	}

 get_reply:
	/*
	 * sub-optimal code appears here because we have
	 * some clock time to spare while the packets are in flight.
	 * (We assume that this is actually only executed once.)
	 */
	rpc_dplx_suc(clnt);
	slocked = false;

	rpc_dplx_rlc(clnt);
	rlocked = true;

	reply_msg.RPCM_ack.ar_verf = _null_auth;
	reply_msg.RPCM_ack.ar_results.where = NULL;
	reply_msg.RPCM_ack.ar_results.proc = (xdrproc_t) xdr_void;

	fd.fd = cu->cu_fd;
	fd.events = POLLIN;
	fd.revents = 0;
	while ((total_time > 0) || once) {
		tv = total_time < nextsend_time ? total_time : nextsend_time;
		once = false;
		switch (poll(&fd, 1, tv)) {
		case 0:
			total_time -= tv;
			rpc_dplx_ruc(clnt);
			rlocked = false;
			if (total_time <= 0) {
				cu->cu_error.re_status = RPC_TIMEDOUT;
				goto out;
			}
			goto send_again;
		case -1:
			if (errno == EINTR)
				continue;
			cu->cu_error.re_status = RPC_CANTRECV;
			cu->cu_error.re_errno = errno;
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
		ret = recvmsg(cu->cu_fd, &msg, MSG_ERRQUEUE);
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
					cu->cu_error.re_errno = e->ee_errno;
					cu->cu_error.re_status = RPC_CANTRECV;
				}
	}
#endif

	/* We have some data now */
	do {
		recvlen =
		    recvfrom(cu->cu_fd, cu->cu_inbuf, cu->cu_recvsz, 0, NULL,
			     NULL);
	} while (recvlen < 0 && errno == EINTR);
	if (recvlen < 0 && errno != EWOULDBLOCK) {
		cu->cu_error.re_errno = errno;
		cu->cu_error.re_status = RPC_CANTRECV;
		goto out;
	}

	if (recvlen < sizeof(u_int32_t)) {
		total_time -= tv;
		rpc_dplx_ruc(clnt);
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
			rpc_dplx_ruc(clnt);
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
			cu->cu_error.re_status = RPC_SUCCESS;
		else
			_seterr_reply(&reply_msg, &(cu->cu_error));

		if (cu->cu_error.re_status == RPC_SUCCESS) {
			if (!AUTH_VALIDATE
			    (auth, &reply_msg.RPCM_ack.ar_verf)) {
				cu->cu_error.re_status = RPC_AUTHERROR;
				cu->cu_error.re_why = AUTH_INVALIDRESP;
			} else
			    if (!AUTH_UNWRAP
				(auth, &reply_xdrs, xresults, resultsp)) {
				if (cu->cu_error.re_status == RPC_SUCCESS)
					cu->cu_error.re_status =
					    RPC_CANTDECODERES;
			}
		}
		/* end successful completion */
		/*
		 * If unsuccesful AND error is an authentication error
		 * then refresh credentials and try again, else break
		 */
		else if (cu->cu_error.re_status == RPC_AUTHERROR)
			/* maybe our credentials need to be refreshed ... */
			if (nrefreshes > 0 && AUTH_REFRESH(auth, &reply_msg)) {
				nrefreshes--;
				rpc_dplx_ruc(clnt);
				rlocked = false;
				goto call_again;
			}
		/* end of unsuccessful completion */
	} /* end of valid reply message */
	else
		cu->cu_error.re_status = RPC_CANTDECODERES;

out:
	if (slocked)
		rpc_dplx_suc(clnt);
	if (rlocked)
		rpc_dplx_ruc(clnt);

	return (cu->cu_error.re_status);
}

static void
clnt_dg_geterr(CLIENT *clnt, struct rpc_err *errp)
{
	struct cu_data *cu = CU_DATA((struct cx_data *)clnt->cl_p1);
	*errp = cu->cu_error;
}

static bool
clnt_dg_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	struct cu_data *cu = CU_DATA((struct cx_data *)clnt->cl_p1);
	XDR *xdrs;
	sigset_t mask, newmask;
	bool dummy = 0;

	/* XXX guard against illegal invocation from libc (will fix) */
	if (!xdr_res)
		goto out;

	xdrs = &(cu->cu_outxdrs);	/* XXX outxdrs? */

	/* Handle our own signal mask here, the signal section is
	 * larger than the wait (not 100% clear why) */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	/* barrier recv channel */
	rpc_dplx_rwc(clnt, rpc_flag_clear);

	xdrs->x_op = XDR_FREE;
	if (xdr_res)
		dummy = (*xdr_res) (xdrs, res_ptr);

	thr_sigsetmask(SIG_SETMASK, &mask, NULL);

	/* signal recv channel */
	rpc_dplx_rsc(clnt, RPC_DPLX_FLAG_NONE);

 out:
	return (dummy);
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
	struct cu_data *cu = CU_DATA((struct cx_data *)clnt->cl_p1);
	struct netbuf *addr;
	bool rslt = true;

	/* always take recv lock first, if taking both locks together */
	rpc_dplx_rlc(clnt);
	rpc_dplx_slc(clnt);

	switch (request) {
	case CLSET_FD_CLOSE:
		cu->cu_closeit = true;
		rslt = true;
		goto unlock;
	case CLSET_FD_NCLOSE:
		cu->cu_closeit = false;
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
		*(int *)info = cu->cu_fd;
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
	rpc_dplx_ruc(clnt);
	rpc_dplx_suc(clnt);

	return (rslt);
}

static void
clnt_dg_destroy(CLIENT *clnt)
{
	struct cx_data *cx = (struct cx_data *)clnt->cl_p1;
	struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *)clnt->cl_p2;
	int cu_fd = CU_DATA(cx)->cu_fd;
	sigset_t mask, newmask;

	/* Handle our own signal mask here, the signal section is
	 * larger than the wait (not 100% clear why) */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	/* barrier both channels */
	rpc_dplx_swc(clnt, rpc_flag_clear);
	rpc_dplx_rwc(clnt, rpc_flag_clear);

	if (CU_DATA(cx)->cu_closeit)
		(void)close(cu_fd);
	XDR_DESTROY(&(CU_DATA(cx)->cu_outxdrs));

	/* signal both channels */
	rpc_dplx_ssc(clnt, RPC_DPLX_FLAG_NONE);
	rpc_dplx_rsc(clnt, RPC_DPLX_FLAG_NONE);

	/* release */
	rpc_dplx_unref(rec, RPC_DPLX_FLAG_NONE);
	free_cx_data(cx);

	if (clnt->cl_netid && clnt->cl_netid[0])
		mem_free(clnt->cl_netid, strlen(clnt->cl_netid) + 1);
	if (clnt->cl_tp && clnt->cl_tp[0])
		mem_free(clnt->cl_tp, strlen(clnt->cl_tp) + 1);
	mem_free(clnt, sizeof(CLIENT));
	thr_sigsetmask(SIG_SETMASK, &mask, NULL);
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
