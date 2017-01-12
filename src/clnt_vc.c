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
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.  [Ouch, that was intemperate.]
 */
#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <misc/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_ioq.h>
#include "svc_ioq.h"


static enum clnt_stat clnt_vc_call(CLIENT *, AUTH *, rpcproc_t, xdrproc_t,
				   void *, xdrproc_t, void *, struct timeval);
static void clnt_vc_geterr(CLIENT *, struct rpc_err *);
static bool clnt_vc_freeres(CLIENT *, xdrproc_t, void *);
static void clnt_vc_abort(CLIENT *);
static bool clnt_vc_control(CLIENT *, u_int, void *);
static bool clnt_vc_ref(CLIENT *, u_int);
static void clnt_vc_release(CLIENT *, u_int);
static void clnt_vc_destroy(CLIENT *);
static struct clnt_ops *clnt_vc_ops(void);
static bool time_not_ok(struct timeval *);
int generic_read_vc(XDR *, void *, void *, int);
int generic_write_vc(XDR *, void *, void *, int);

#include "clnt_internal.h"
#include "svc_internal.h"

/*
 *      This machinery implements per-fd locks for MT-safety.  It is not
 *      sufficient to do per-CLIENT handle locks for MT-safety because a
 *      user may create more than one CLIENT handle with the same fd behind
 *      it.  Therfore, we allocate an array of flags (vc_fd_locks), protected
 *      by the clnt_fd_lock mutex, and an array (vc_cv) of condition variables
 *      similarly protected.  Vc_fd_lock[fd] == 1 => a call is active on some
 *      CLIENT handle created for that fd.  (Historical interest only.)
 *
 *      The current implementation holds locks across the entire RPC and reply.
 *      Yes, this is silly, and as soon as this code is proven to work, this
 *      should be the first thing fixed.  One step at a time.  (Fixing this.)
 *
 *      The ONC RPC record marking (RM) standard (RFC 5531, s. 11) does not
 *      provide for mixed call and reply fragment reassembly, so writes to the
 *      bytestream with MUST be record-wise atomic.  It appears that an
 *      implementation may interleave call and reply messages on distinct
 *      conversations.  This is not incompatible with holding a transport
 *      exclusive locked across a full call and reply, bud does require new
 *      control tranfers and delayed decoding support in the transport.  For
 *      duplex channels, full coordination is required between client and
 *      server tranpsorts sharing an underlying bytestream (Matt).
 */

static const char clnt_vc_errstr[] = "%s : %s";
static const char clnt_vc_str[] = "clnt_vc_ncreate";
static const char __no_mem_str[] = "out of memory";

/*
 * Create a client handle for a connection.
 * Default options are set, which the user can change using clnt_control()'s.
 * The rpc/vc package does buffering similar to stdio, so the client
 * must pick send and receive buffer sizes, 0 => use the default.
 * NB: fd is copied into a private area.
 * NB: The rpch->cl_auth is set null authentication. Caller may wish to
 * set this something more useful.
 *
 * fd should be an open socket
 */
CLIENT *
clnt_vc_ncreatef(const int fd,	/* open file descriptor */
		 const struct netbuf *raddr,	/* servers address */
		 const rpcprog_t prog,	/* program number */
		 const rpcvers_t vers,	/* version number */
		 const u_int sendsz,	/* buffer send size */
		 const u_int recvsz,	/* buffer recv size */
		 const uint32_t flags)
{
	CLIENT *clnt = NULL;
	struct rpc_dplx_rec *rec = NULL;
	struct x_vc_data *xd = NULL;
	struct cx_data *cx = NULL;
	struct ct_data *ct = NULL;
	struct ct_data *cs;
	struct rpc_msg call_msg;
	sigset_t mask, newmask;
	struct __rpc_sockinfo si;
	struct sockaddr_storage ss;
	XDR ct_xdrs[1];		/* temp XDR stream */
	uint32_t oflags;
	socklen_t slen;

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	if (flags & CLNT_CREATE_FLAG_CONNECT) {
		slen = sizeof(ss);
		if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
			if (errno != ENOTCONN) {
				rpc_createerr.cf_stat = RPC_SYSTEMERROR;
				rpc_createerr.cf_error.re_errno = errno;
				goto err;
			}
			if (connect
			    (fd, (struct sockaddr *)raddr->buf,
			     raddr->len) < 0) {
				rpc_createerr.cf_stat = RPC_SYSTEMERROR;
				rpc_createerr.cf_error.re_errno = errno;
				goto err;
			}
			__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
				"%s: fd %d connected",
				__func__, fd);
		}
	}
	/* connect */
	if (!__rpc_fd2sockinfo(fd, &si))
		goto err;

	/* atomically find or create shared fd state */
	rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
	if (!rec) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"clnt_vc_ncreate2: rpc_dplx_lookup_rec failed");
		goto err;
	}

	/* attach shared state */
	if ((oflags & RPC_DPLX_LKP_OFLAG_ALLOC) || (!rec->hdl.xd)) {
		xd = rec->hdl.xd = alloc_x_vc_data();
		if (!xd) {
			(void)syslog(LOG_ERR, clnt_vc_errstr, clnt_vc_str,
				     __no_mem_str);
			rpc_createerr.cf_stat = RPC_SYSTEMERROR;
			rpc_createerr.cf_error.re_errno = errno;
			goto err;
		}
		xd->rec = rec;
		/* XXX tracks outstanding calls */
		opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
		xd->cx.calls.xid = 0;	/* next call xid is 1 */
		xd->refcnt = 1;

		xd->shared.sendsz =
		    __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
		xd->shared.recvsz =
		    __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);

		/* duplex streams */
		xdr_inrec_create(&(xd->shared.xdrs_in), xd->shared.recvsz, xd,
				 generic_read_vc);
		xd->shared.xdrs_in.x_op = XDR_DECODE;

		xdrrec_create(&(xd->shared.xdrs_out), sendsz, xd->shared.recvsz,
			      xd, generic_read_vc, generic_write_vc);
		xd->shared.xdrs_out.x_op = XDR_ENCODE;
	} else {
		xd = rec->hdl.xd;
		++(xd->refcnt);
	}

	/* buffer sizes should match svc side */
	cx = alloc_cx_data(CX_VC_DATA, xd->shared.sendsz, xd->shared.recvsz);

	/* private data struct */
	xd->cx.data.ct_fd = fd;
	cs = CT_DATA(cx);

	ct = &xd->cx.data;
	ct->ct_closeit = false;
	ct->ct_wait.tv_usec = 0;
	ct->ct_waitset = false;

	if (sizeof(struct sockaddr_storage) < raddr->len) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d called with invalid address length"
			" (max %z < %u len)",
			__func__, fd,
			sizeof(struct sockaddr_storage),
			raddr->len);
		goto err;
	}
	memcpy(&cs->ct_raddr, raddr->buf, raddr->len);
	cs->ct_rlen = raddr->len;

	/*
	 * initialize call message
	 */
	call_msg.rm_xid = 1;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.cb_prog = prog;
	call_msg.cb_vers = vers;

	/*
	 * pre-serialize the static part of the call msg and stash it away
	 */
	xdrmem_create(ct_xdrs, cs->ct_u.ct_mcallc, MCALL_MSG_SIZE, XDR_ENCODE);
	if (!xdr_callhdr(ct_xdrs, &call_msg)) {
		if (ct->ct_closeit)
			(void)close(fd);
		goto err;
	}
	cs->ct_mpos = XDR_GETPOS(ct_xdrs);
	XDR_DESTROY(ct_xdrs);

	/*
	 * Create a client handle which uses xdrrec for serialization
	 * and authnone for authentication.
	 */
	clnt = &cx->cx_c;
	clnt->cl_ops = clnt_vc_ops();
	clnt->cl_p1 = xd;
	clnt->cl_p2 = rec;

	/* release rec */
	REC_UNLOCK(rec);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
		"%s: fd %d completed",
		__func__, fd);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (clnt);

 err:
	if (cx) {
		free_cx_data(cx);
	}

	if (rec) {
		if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
			REC_UNLOCK(rec);
	}

	if (xd) {
		if (xd->refcnt == 0) {
			XDR_DESTROY(&xd->shared.xdrs_in);
			XDR_DESTROY(&xd->shared.xdrs_out);
			free_x_vc_data(xd);
		}
	}

	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (NULL);
}

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 */
CLIENT *
clnt_vc_ncreate_svc(const SVCXPRT *xprt, const rpcprog_t prog,
		    const rpcvers_t vers, const uint32_t flags)
{
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;

	return clnt_vc_ncreatef(xprt->xp_fd, &xprt->xp_remote.nb, prog, vers,
				xd->shared.sendsz, xd->shared.recvsz,
				flags | CLNT_CREATE_FLAG_SVCXPRT);
}

#define vc_call_return_slocked(r)		\
	do {					\
		result = (r);			\
		if (!bidi)			\
			rpc_dplx_suc(clnt);	\
		goto out;			\
	} while (0)

#define vc_call_return_rlocked(r)		\
	do {					\
		result = (r);			\
		rpc_dplx_ruc(clnt);		\
		goto out;			\
	} while (0)

static enum clnt_stat
clnt_vc_call(CLIENT *clnt, AUTH *auth, rpcproc_t proc,
	     xdrproc_t xdr_args, void *args_ptr,
	     xdrproc_t xdr_results, void *results_ptr,
	     struct timeval timeout)
{
	struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
	struct ct_data *ct = &(xd->cx.data);
	struct cx_data *cx = CX_DATA(clnt);
	struct ct_data *cs = CT_DATA(cx);
	struct rpc_dplx_rec *rec = xd->rec;
	enum clnt_stat result = RPC_SUCCESS;
	rpc_ctx_t *ctx = NULL;
	XDR *xdrs;
	int code, refreshes = 2;
	bool ctx_needack = false;
	bool bidi = (rec->hdl.xprt != NULL);
	bool shipnow;
	bool gss = false;
	bool slocked = true;

	/* Need lock for ct_wait, ct_mcallc, and sequential xid.
	 */
	rpc_dplx_slc(clnt);

	/* Create a call context.  A lot of TI-RPC decisions need to be
	 * looked at, including:
	 *
	 * 1. the client has a serialized call.  This looks harmless, so long
	 * as the xid is adjusted.
	 *
	 * 2. the last xid used is now saved in handle shared private data, it
	 * will be incremented by rpc_call_create (successive calls).  There's
	 * no more reason to use the old time-dependent xid logic.  It should be
	 * preferable to count atomically from 1.
	 *
	 * 3. the client has an XDR structure, which contains the initialied
	 * xdrrec stream.  Since there is only one physical byte stream, it
	 * would potentially be worse to do anything else?  The main issue which
	 * will arise is the need to transition the stream between calls--which
	 * may require adjustment to xdrrec code.  But on review it seems to
	 * follow that one xdrrec stream would be parameterized by different
	 * call contexts.  We'll keep the call parameters, control transfer
	 * machinery, etc, in an rpc_ctx_t, to permit this.
	 */
	ctx = rpc_ctx_alloc(clnt, proc, xdr_args, args_ptr, xdr_results,
			    results_ptr, timeout);	/*add total timeout? */

	if (!ct->ct_waitset) {
		/* If time is not within limits, we ignore it. */
		if (time_not_ok(&timeout) == false)
			ct->ct_wait = timeout;
	}

	shipnow = (xdr_results == NULL && timeout.tv_sec == 0
		   && timeout.tv_usec == 0) ? false : true;

 call_again:
	if (!slocked) {
		slocked = true;
		rpc_dplx_slc(clnt);
	}

	if (bidi) {
		/* XXX Until gss_get_mic and gss_wrap can be replaced with
		 * iov equivalents, replies with RPCSEC_GSS security must be
		 * encoded in a contiguous buffer.
		 *
		 * Nb, we should probably use getpagesize() on Unix.  Need
		 * an equivalent for Windows.
		 */
		gss = (auth->ah_cred.oa_flavor == RPCSEC_GSS);
		xdrs = xdr_ioq_create(8192 /* default segment size */ ,
				      __svc_params->svc_ioq_maxbuf + 8192,
				      gss
				      ? UIO_FLAG_REALLOC | UIO_FLAG_FREE
				      : UIO_FLAG_FREE);
	} else {
		xdrs = &(xd->shared.xdrs_out);
		xdrs->x_lib[0] = (void *)RPC_DPLX_CLNT;
		xdrs->x_lib[1] = (void *)ctx;	/* thread call ctx */
	}

	ctx->error.re_status = RPC_SUCCESS;
	cs->ct_u.ct_mcalli = ntohl(ctx->xid);

	if ((!XDR_PUTBYTES(xdrs, cs->ct_u.ct_mcallc, cs->ct_mpos))
	    || (!XDR_PUTINT32(xdrs, (int32_t *) &proc))
	    || (!AUTH_MARSHALL(auth, xdrs))
	    || (!AUTH_WRAP(auth, xdrs, xdr_args, args_ptr))) {
		if (ctx->error.re_status == RPC_SUCCESS)
			ctx->error.re_status = RPC_CANTENCODEARGS;
		/* error case */
		if (!bidi)
			(void)xdrrec_endofrecord(xdrs, true);
		vc_call_return_slocked(ctx->error.re_status);
	}

	if (bidi) {
		svc_ioq_append(rec->hdl.xprt, XIOQ(xdrs));
	} else {
		if (!xdrrec_endofrecord(xdrs, shipnow))
			vc_call_return_slocked(ctx->error.re_status =
					       RPC_CANTSEND);
		if (!shipnow)
			vc_call_return_slocked(RPC_SUCCESS);

		/*
		 * Hack to provide rpc-based message passing
		 */
		if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
			vc_call_return_slocked(ctx->error.re_status =
					       RPC_TIMEDOUT);
	}
	rpc_dplx_suc(clnt);
	slocked = false;

	/* reply */
	rpc_dplx_rlc(clnt);

	/* if the channel is bi-directional, then the the shared conn is in a
	 * svc event loop, and recv processing decodes reply headers */

	xdrs = &(xd->shared.xdrs_in);

	xdrs->x_lib[0] = (void *)RPC_DPLX_CLNT;
	xdrs->x_lib[1] = (void *)ctx;	/* transiently thread call ctx */

	if (bidi) {
		code = rpc_ctx_wait_reply(ctx, RPC_DPLX_FLAG_LOCKED);/* RECV! */
		if (code == ETIMEDOUT) {
			/* UL can retry, we dont.  This CAN indicate xprt
			 * destroyed (error status already set). */
			ctx->error.re_status = RPC_TIMEDOUT;
			goto unlock;
		}
		/* switch on direction */
		switch (ctx->cc_msg.rm_direction) {
		case REPLY:
			if (ctx->cc_msg.rm_xid == ctx->xid) {
				ctx_needack = true;
				goto replied;
			}
			break;
		case CALL:
			/* in this configuration, we do not expect calls */
			break;
		default:
			break;
		}
	} else {
		/*
		 * Keep receiving until we get a valid transaction id.
		 */
		while (true) {

			/* skiprecord */
			if (!xdr_inrec_skiprecord(xdrs)) {
				__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
					"%s: error at skiprecord", __func__);
				vc_call_return_rlocked(ctx->error.re_status);
			}

			/* now decode and validate the response header */
			if (!xdr_dplx_decode(xdrs, &ctx->cc_msg)) {
				__warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
					"%s: error at xdr_dplx_decode",
					__func__);
				vc_call_return_rlocked(ctx->error.re_status);
			}

			/* switch on direction */
			switch (ctx->cc_msg.rm_direction) {
			case REPLY:
				if (ctx->cc_msg.rm_xid == ctx->xid)
					goto replied;
				break;
			case CALL:
				/* in this configuration, we do not expect
				 * calls */
				break;
			default:
				break;
			}
		}		/* while (true) */
	}			/* ! bi-directional */

	/*
	 * process header
	 */
 replied:
	/* XXX move into routine which can be called from rpc_ctx_xfer_replymsg,
	 * for (maybe) reduced MP overhead */
	_seterr_reply(&ctx->cc_msg, &(ctx->error));
	if (ctx->error.re_status == RPC_SUCCESS) {
		if (!AUTH_VALIDATE(auth, &(ctx->cc_msg.RPCM_ack.ar_verf))) {
			ctx->error.re_status = RPC_AUTHERROR;
			ctx->error.re_why = AUTH_INVALIDRESP;
		} else if (xdr_results /* XXX caller setup error? */ &&
			   !AUTH_UNWRAP(auth, xdrs, xdr_results, results_ptr)) {
			if (ctx->error.re_status == RPC_SUCCESS)
				ctx->error.re_status = RPC_CANTDECODERES;
		}
		if (ctx_needack)
			rpc_ctx_ack_xfer(ctx);
	} /* end successful completion */
	else {
		/* maybe our credentials need to be refreshed ... */
		if (refreshes-- && AUTH_REFRESH(auth, &(ctx->cc_msg))) {
			rpc_ctx_next_xid(ctx, RPC_CTX_FLAG_NONE);
			rpc_dplx_ruc(clnt);
			if (ctx_needack)
				rpc_ctx_ack_xfer(ctx);
			goto call_again;
		}
	}			/* end of unsuccessful completion */

 unlock:
	vc_call_return_rlocked(ctx->error.re_status);

 out:
	rpc_ctx_free(ctx, RPC_CTX_FLAG_NONE);

	return (result);
}

static void
clnt_vc_geterr(CLIENT *clnt, struct rpc_err *errp)
{
	struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
	XDR *xdrs;

	/* assert: it doesn't matter which we use */
	xdrs = &xd->shared.xdrs_out;
	if (xdrs->x_lib[0]) {
		rpc_ctx_t *ctx = (rpc_ctx_t *) xdrs->x_lib[1];
		*errp = ctx->error;
	} else {
		/* XXX we don't want (overhead of) an unsafe last-error value */
		struct rpc_err err;
		memset(&err, 0, sizeof(struct rpc_err));
		*errp = err;
	}
}

static bool
clnt_vc_freeres(CLIENT *clnt, xdrproc_t xdr_res, void *res_ptr)
{
	sigset_t mask, newmask;
	bool rslt;

	/* XXX is this (legacy) signalling/barrier logic needed? */

	/* Handle our own signal mask here, the signal section is
	 * larger than the wait (not 100% clear why) */
	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	/* barrier recv channel */
	rpc_dplx_rwc(clnt, rpc_flag_clear);

	rslt = xdr_free(xdr_res, res_ptr);

	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);

	/* signal recv channel */
	rpc_dplx_rsc(clnt, RPC_DPLX_FLAG_NONE);

	return (rslt);
}

 /*ARGSUSED*/
static void
clnt_vc_abort(CLIENT *clnt)
{
}

static bool
clnt_vc_control(CLIENT *clnt, u_int request, void *info)
{
	struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
	struct ct_data *ct = &(xd->cx.data);
	struct cx_data *cx = CX_DATA(clnt);
	struct ct_data *cs = CT_DATA(cx);
	void *infop = info;
	struct netbuf *addr;
	bool rslt = true;

	/* always take recv lock first if taking together */
	rpc_dplx_rlc(clnt);
	rpc_dplx_slc(clnt);

	switch (request) {
	case CLSET_FD_CLOSE:
		ct->ct_closeit = true;
		goto unlock;
		return (true);
	case CLSET_FD_NCLOSE:
		ct->ct_closeit = false;
		goto unlock;
		return (true);
	default:
		break;
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
		ct->ct_wait = *(struct timeval *)infop;
		ct->ct_waitset = true;
		break;
	case CLGET_TIMEOUT:
		*(struct timeval *)infop = ct->ct_wait;
		break;
	case CLGET_SERVER_ADDR:
		/* Now obsolete. Only for backward compatibility */
		(void)memcpy(info, &cs->ct_raddr, (size_t) cs->ct_rlen);
		break;
	case CLGET_FD:
		*(int *)info = ct->ct_fd;
		break;
	case CLGET_SVC_ADDR:
		/* The caller should not free this memory area */
		addr = (struct netbuf *)info;
		addr->buf = &cs->ct_raddr;
		addr->len = cs->ct_rlen;
		addr->maxlen = sizeof(cs->ct_raddr);
		break;
	case CLSET_SVC_ADDR:	/* set to new address */
		rslt = false;
		goto unlock;
	case CLGET_XID:
		/*
		 * use the knowledge that xid is the
		 * first element in the call structure
		 * This will get the xid of the PREVIOUS call
		 */
		*(u_int32_t *) info =
		    ntohl(*(u_int32_t *) (void *)&cs->ct_u.ct_mcalli);
		break;
	case CLSET_XID:
		/* This will set the xid of the NEXT call */
		*(u_int32_t *) (void *)&cs->ct_u.ct_mcalli =
		    htonl(*((u_int32_t *) info) + 1);
		/* increment by 1 as clnt_vc_call() decrements once */
		break;
	case CLGET_VERS:
		/*
		 * This RELIES on the information that, in the call body,
		 * the version number field is the fifth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		{
			u_int32_t *tmp =
			    (u_int32_t *) (cs->ct_u.ct_mcallc +
					   4 * BYTES_PER_XDR_UNIT);
			*(u_int32_t *) info = ntohl(*tmp);
		}
		break;

	case CLSET_VERS:
		{
			u_int32_t tmp = htonl(*(u_int32_t *) info);
			*(cs->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT) = tmp;
		}
		break;

	case CLGET_PROG:
		/*
		 * This RELIES on the information that, in the call body,
		 * the program number field is the fourth field from the
		 * begining of the RPC header. MUST be changed if the
		 * call_struct is changed
		 */
		{
			u_int32_t *tmp =
			    (u_int32_t *) (cs->ct_u.ct_mcallc +
					   3 * BYTES_PER_XDR_UNIT);
			*(u_int32_t *) info = ntohl(*tmp);
		}
		break;

	case CLSET_PROG:
		{
			u_int32_t tmp = htonl(*(u_int32_t *) info);
			*(cs->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT) = tmp;
		}
		break;

	default:
		rslt = false;
		goto unlock;
		break;
	}

 unlock:
	rpc_dplx_ruc(clnt);
	rpc_dplx_suc(clnt);

	return (rslt);
}

static bool
clnt_vc_ref(CLIENT *clnt, u_int flags)
{
	uint32_t refcnt;

	if (!(flags & CLNT_REF_FLAG_LOCKED))
		mutex_lock(&clnt->cl_lock);

	if (clnt->cl_flags & CLNT_FLAG_DESTROYED) {
		mutex_unlock(&clnt->cl_lock);
		return (false);
	}
	refcnt = ++(clnt->cl_refcnt);
	mutex_unlock(&clnt->cl_lock);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: postref %p %u", __func__, clnt,
		refcnt);

	return (true);
}

static void
clnt_vc_release(CLIENT *clnt, u_int flags)
{
	uint32_t cl_refcnt;

	if (!(flags & CLNT_RELEASE_FLAG_LOCKED))
		mutex_lock(&clnt->cl_lock);

	cl_refcnt = --(clnt->cl_refcnt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: postunref %p cl_refcnt %u",
		__func__, clnt, cl_refcnt);

	/* conditional destroy */
	if ((clnt->cl_flags & CLNT_FLAG_DESTROYED) && (cl_refcnt == 0)) {
		struct cx_data *cx = CX_DATA(clnt);
		struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
		struct rpc_dplx_rec *rec = xd->rec;
		uint32_t xd_refcnt;

		mutex_unlock(&clnt->cl_lock);

		/* client handles are now freed directly */
		free_cx_data(cx);

		REC_LOCK(rec);
		xd_refcnt = --(xd->refcnt);

		if (xd_refcnt == 0) {
			__warnx(TIRPC_DEBUG_FLAG_REFCNT,
				"%s: xd_refcnt %u on destroyed %p %u calling "
				"vc_shared_destroy", __func__, clnt, cl_refcnt);
			vc_shared_destroy(xd);	/* RECLOCKED */
			rpc_dplx_unref(rec, RPC_DPLX_FLAG_NONE);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_REFCNT,
				"%s: xd_refcnt %u on destroyed %p omit "
				"vc_shared_destroy", __func__, clnt, cl_refcnt);
			rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED);
		}
	} else
		mutex_unlock(&clnt->cl_lock);
}

static void
clnt_vc_destroy(CLIENT *clnt)
{
	struct rpc_dplx_rec *rec;
	struct x_vc_data *xd;
	uint32_t cl_refcnt = 0;
	uint32_t xd_refcnt = 0;

	mutex_lock(&clnt->cl_lock);
	if (clnt->cl_flags & CLNT_FLAG_DESTROYED) {
		mutex_unlock(&clnt->cl_lock);
		goto out;
	}

	xd = (struct x_vc_data *)clnt->cl_p1;
	rec = xd->rec;

	clnt->cl_flags |= CLNT_FLAG_DESTROYED;
	cl_refcnt = --(clnt->cl_refcnt);
	mutex_unlock(&clnt->cl_lock);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: cl_destroy %p cl_refcnt %u",
		__func__, clnt, cl_refcnt);

	/* bidirectional */
	REC_LOCK(rec);

	xd_refcnt = --(xd->refcnt);

	/* conditional destroy */
	if (cl_refcnt == 0) {
		struct cx_data *cx = CX_DATA(clnt);

		/* client handles are now freed directly */
		free_cx_data(cx);

		if (xd_refcnt == 0) {
			__warnx(TIRPC_DEBUG_FLAG_REFCNT,
				"%s: %p cl_refcnt %u xd_refcnt %u calling "
				"vc_shared_destroy", __func__, clnt, cl_refcnt,
				xd_refcnt);
			vc_shared_destroy(xd);	/* RECLOCKED */
			rpc_dplx_unref(rec, RPC_DPLX_FLAG_NONE);
			goto out;
		}
	}

	REC_UNLOCK(rec);

 out:
	return;
}

static struct clnt_ops *
clnt_vc_ops(void)
{
	static struct clnt_ops ops;
	extern mutex_t ops_lock;
	sigset_t mask, newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_vc_call;
		ops.cl_abort = clnt_vc_abort;
		ops.cl_geterr = clnt_vc_geterr;
		ops.cl_freeres = clnt_vc_freeres;
		ops.cl_ref = clnt_vc_ref;
		ops.cl_release = clnt_vc_release;
		ops.cl_destroy = clnt_vc_destroy;
		ops.cl_control = clnt_vc_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (&ops);
}

/*
 * Make sure that the time is not garbage.   -1 value is disallowed.
 * Note this is different from time_not_ok in clnt_dg.c
 */
static bool time_not_ok(struct timeval *t)
{
	return (t->tv_sec <= -1 || t->tv_sec > 100000000 || t->tv_usec <= -1
		|| t->tv_usec > 1000000);
}
