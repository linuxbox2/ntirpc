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

#include <config.h>

/*
 * svc_vc.c, Server side for Connection Oriented RPC.
 *
 * Actually implements two flavors of transporter -
 * a tcp rendezvouser (a listner and connection establisher)
 * and a record/tcp stream.
 */
#include <sys/cdefs.h>
#include <sys/socket.h>
#ifdef RPC_VSOCK
#include <linux/vm_sockets.h>
#endif /* VSOCK */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include "svc_ioq.h"

int generic_read_vc(XDR *, void *, void *, int);

static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_ops(SVCXPRT *);
static void svc_vc_override_ops(SVCXPRT *, SVCXPRT *);

bool __svc_clean_idle2(int, bool);

static SVCXPRT *makefd_xprt(const int, const u_int, const u_int,
			    struct __rpc_sockinfo *, uint32_t *);

extern pthread_mutex_t svc_ctr_lock;

/*
 * Usage:
 * xprt = svc_vc_ncreate(sock, send_buf_size, recv_buf_size);
 *
 * Creates, registers, and returns a (rpc) tcp based transport.
 * If a problem occurred, this routine returns a NULL.
 *
 * Since streams do buffered io similar to stdio, the caller can specify
 * how big the send and receive buffers are via the second and third parms;
 * 0 => use the system default.
 *
 * Added svc_vc_ncreatef with flags argument, has the behavior of the
 * original function with flags SVC_CREATE_FLAG_CLOSE.
 *
 */
static void
svc_vc_xprt_free(struct svc_vc_xprt *xd)
{
	rpc_dplx_rec_destroy(&xd->sx_dr);
	mutex_destroy(&xd->sx_dr.xprt.xp_lock);
	mutex_destroy(&xd->sx_dr.xprt.xp_auth_lock);

#if defined(HAVE_BLKIN)
	if (xd->sx_dr.xprt.blkin.svc_name)
		mem_free(xd->sx_dr.xprt.blkin.svc_name, 2*INET6_ADDRSTRLEN);
#endif
	mem_free(xd, sizeof(struct svc_vc_xprt));
}

static struct svc_vc_xprt *
svc_vc_xprt_zalloc(void)
{
	struct svc_vc_xprt *xd = mem_zalloc(sizeof(struct svc_vc_xprt));

	/* Init SVCXPRT locks, etc */
	mutex_init(&xd->sx_dr.xprt.xp_lock, NULL);
	mutex_init(&xd->sx_dr.xprt.xp_auth_lock, NULL);
/*	TAILQ_INIT_ENTRY(&xd->sx_dr.xprt, xp_evq); sets NULL */
	rpc_dplx_rec_init(&xd->sx_dr);

	xd->sx.strm_stat = XPRT_IDLE;
	xd->sx_dr.xprt.xp_refs = 1;
	return (xd);
}

void
svc_vc_xprt_setup(SVCXPRT **sxpp)
{
	if (unlikely(*sxpp)) {
		svc_vc_xprt_free(VC_DR(REC_XPRT(*sxpp)));
		*sxpp = NULL;
	} else {
		struct svc_vc_xprt *xd = svc_vc_xprt_zalloc();

		*sxpp = &xd->sx_dr.xprt;
	}
}

SVCXPRT *
svc_vc_ncreatef(const int fd, const u_int sendsz, const u_int recvsz,
		const uint32_t flags)
{
	struct __rpc_sockinfo si;
	SVCXPRT *xprt;
	struct rpc_dplx_rec *rec;
	struct svc_vc_xprt *xd;
	const char *netid;
	u_int recvsize;
	u_int sendsize;
	u_int xp_flags;
	int rc;

	/* atomically find or create shared fd state; ref+1; locked */
	xprt = svc_xprt_lookup(fd, svc_vc_xprt_setup);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d svc_xprt_lookup failed",
			__func__, fd);
		return (NULL);
	}
	rec = REC_XPRT(xprt);

	xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags, flags
						| SVC_XPRT_FLAG_INITIALIZED);
	if (xp_flags & SVC_XPRT_FLAG_INITIALIZED) {
		rpc_dplx_rui(rec);
		XPRT_TRACE(xprt, __func__, __func__, __LINE__);
		return (xprt);
	}

	if (!__rpc_fd2sockinfo(fd, &si)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get transport information",
			__func__, fd);
		return (NULL);
	}

	if (!__rpc_sockinfo2netid(&si, &netid)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get network information",
			__func__, fd);
		return (NULL);
	}
	xd = VC_DR(rec);

	opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
/*	xd->cx.calls.xid = 0;	next call xid is 1 */

	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
	recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);
	/*
	 * Should be multiple of 4 for XDR.
	 */
	xd->shared.sendsz = ((sendsize + 3) / 4) * 4;
	xd->shared.recvsz = ((recvsize + 3) / 4) * 4;
	xd->sx.maxrec = __svc_maxrec;

	/* duplex streams are not used by the rendevous transport */

	svc_vc_rendezvous_ops(xprt);
#ifdef RPC_VSOCK
	if (si.si_af == AF_VSOCK)
		 xprt->xp_type = XPRT_VSOCK_RENDEZVOUS;
#endif /* VSOCK */

	/* caller should know what it's doing */
	if (flags & SVC_CREATE_FLAG_LISTEN) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d listen",
			 __func__, fd);
		listen(fd, SOMAXCONN);
	}

	__rpc_address_setup(&xprt->xp_local);
	rc = getsockname(fd, xprt->xp_local.nb.buf, &xprt->xp_local.nb.len);
	if (rc < 0) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}

	xprt->xp_netid = mem_strdup(netid);

	/* release rec */
	rpc_dplx_rui(rec);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	/* Conditional register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
	     && !(flags & SVC_CREATE_FLAG_XPRT_NOREG))
	    || (flags & SVC_CREATE_FLAG_XPRT_DOREG))
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_vc");
#endif

	return (xprt);
}

/*
 * Like sv_fd_ncreate(), except export flags for additional control.
 */
SVCXPRT *
svc_fd_ncreatef(const int fd, const u_int sendsize, const u_int recvsize,
		const uint32_t flags)
{
	SVCXPRT *xprt;
	struct __rpc_sockinfo si;
	int rc;
	uint32_t make_flags = flags;

	assert(fd != -1);

	xprt = makefd_xprt(fd, sendsize, recvsize, &si, &make_flags);
	if ((!xprt) || (!(make_flags & SVC_XPRT_FLAG_ADDED)))
		return (xprt);

	__rpc_address_setup(&xprt->xp_local);
	rc = getsockname(fd, xprt->xp_local.nb.buf, &xprt->xp_local.nb.len);
	if (rc < 0) {
		xprt->xp_local.nb.len = sizeof(struct sockaddr_storage);
		memset(xprt->xp_local.nb.buf, 0xfe, xprt->xp_local.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}

	__rpc_address_setup(&xprt->xp_remote);
	rc = getpeername(fd, xprt->xp_remote.nb.buf, &xprt->xp_remote.nb.len);
	if (rc < 0) {
		xprt->xp_remote.nb.len = sizeof(struct sockaddr_storage);
		memset(xprt->xp_remote.nb.buf, 0xfe, xprt->xp_remote.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d getpeername failed (%d)",
			 __func__, fd, rc);
		return (NULL);
	}
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	/* Conditional register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
	     && !(flags & SVC_CREATE_FLAG_XPRT_NOREG))
	    || (flags & SVC_CREATE_FLAG_XPRT_DOREG))
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_vc");
#endif

	return (xprt);
}

static SVCXPRT *
makefd_xprt(const int fd, const u_int sendsz, const u_int recvsz,
	    struct __rpc_sockinfo *si, u_int *flags)
{
	SVCXPRT *xprt;
	struct svc_vc_xprt *xd;
	struct rpc_dplx_rec *rec;
	const char *netid;
	u_int recvsize;
	u_int sendsize;
	u_int xp_flags;

	assert(fd != -1);

	if (!svc_vc_new_conn_ok()) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d max_connections exceeded\n",
			__func__, fd);
		return (NULL);
	}

	/* atomically find or create shared fd state; ref+1; locked */
	xprt = svc_xprt_lookup(fd, svc_vc_xprt_setup);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d svc_xprt_lookup failed",
			__func__, fd);
		return (NULL);
	}
	rec = REC_XPRT(xprt);

	xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags, *flags
						| SVC_XPRT_FLAG_INITIALIZED);
	if (xp_flags & SVC_XPRT_FLAG_INITIALIZED) {
		rpc_dplx_rui(rec);
		XPRT_TRACE(xprt, __func__, __func__, __LINE__);
		return (xprt);
	}

	/* XXX bi-directional?  initially I had assumed that explicit
	 * routines to create a clnt or svc handle from an already-connected
	 * handle of the other type, but perhaps it is more natural to
	 * just discover it
	 */
	if (!__rpc_fd2sockinfo(fd, si)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get transport information",
			__func__, fd);
		return (NULL);
	}

	if (!__rpc_sockinfo2netid(si, &netid)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d could not get network information",
			__func__, fd);
		return (NULL);
	}
	xd = VC_DR(rec);

	opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
/*	xd->cx.calls.xid = 0;	next call xid is 1 */

	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si->si_af, si->si_proto, (int)sendsz);
	recvsize = __rpc_get_t_size(si->si_af, si->si_proto, (int)recvsz);
	/*
	 * Should be multiple of 4 for XDR.
	 */
	xd->shared.sendsz = ((sendsize + 3) / 4) * 4;
	xd->shared.recvsz = ((recvsize + 3) / 4) * 4;
	xd->sx.maxrec = __svc_maxrec;

	/* duplex streams */
	xdr_inrec_create(&(xd->shared.xdrs_in), xd->shared.recvsz, xd,
			 generic_read_vc);
	xd->shared.xdrs_in.x_op = XDR_DECODE;

	/* the SVCXPRT created in svc_vc_create accepts new connections
	 * in its xp_recv op, the rendezvous_request method, but xprt is
	 * a call channel */
	svc_vc_ops(xprt);
#ifdef RPC_VSOCK
	if (si->si_af == AF_VSOCK)
		 xprt->xp_type = XPRT_VSOCK;
#endif /* VSOCK */

	xprt->xp_netid = mem_strdup(netid);

	*flags |= SVC_XPRT_FLAG_ADDED;

	/* release */
	rpc_dplx_rui(rec);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	return (xprt);
}

 /*ARGSUSED*/
static bool
rendezvous_request(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct svc_vc_xprt *req_xd = VC_DR(REC_XPRT(xprt));
	SVCXPRT *newxprt;
	struct svc_vc_xprt *xd;
	struct sockaddr_storage addr;
	struct __rpc_sockinfo si;
	int fd;
	int rc;
	socklen_t len;
	u_int make_flags;
	static int n = 1;

 again:
	len = sizeof(addr);
	fd = accept(xprt->xp_fd, (struct sockaddr *)(void *)&addr, &len);
	if (fd < 0) {
		if (errno == EINTR)
			goto again;
		/*
		 * Clean out the most idle file descriptor when we're
		 * running out.
		 */
		if (errno == EMFILE || errno == ENFILE) {
			switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
			case SVC_EVENT_EPOLL:
				break;
#endif
			default:
				abort();	/* XXX */
				break;
			}	/* switch */
			goto again;
		}
		return (FALSE);
	}

	(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));

	/*
	 * make a new transport (re-uses xprt)
	 */
	make_flags = SVC_XPRT_FLAG_CLOSE;
	newxprt = makefd_xprt(fd, req_xd->shared.sendsz, req_xd->shared.recvsz,
			      &si, &make_flags);
	if ((!newxprt) || (!(make_flags & SVC_XPRT_FLAG_ADDED)))
		return (FALSE);

	/*
	 * propagate special ops
	 */
	svc_vc_override_ops(xprt, newxprt);

	/* move xprt_register() out of makefd_xprt */
	(void)svc_rqst_xprt_register(xprt, newxprt);

	__rpc_address_setup(&newxprt->xp_remote);
	memcpy(newxprt->xp_remote.nb.buf, &addr, len);
	newxprt->xp_remote.nb.len = len;
	XPRT_TRACE(newxprt, __func__, __func__, __LINE__);

	/* XXX fvdl - is this useful? (Yes.  Matt) */
	if (si.si_proto == IPPROTO_TCP) {
		len = 1;
		(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len,
				  sizeof(len));
	}

	__rpc_address_setup(&newxprt->xp_local);
	rc = getsockname(fd, newxprt->xp_local.nb.buf,
			 &newxprt->xp_local.nb.len);
	if (rc < 0) {
		newxprt->xp_local.nb.len = sizeof(struct sockaddr_storage);
		memset(newxprt->xp_local.nb.buf, 0xfe,
		       newxprt->xp_local.nb.len);
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: fd %d getsockname failed (%d)",
			 __func__, fd, rc);
	}

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(newxprt, "svc_vc");
#endif

	xd = VC_DR(REC_XPRT(newxprt));
	xd->shared.recvsz = req_xd->shared.recvsz;
	xd->shared.sendsz = req_xd->shared.sendsz;
	xd->sx.maxrec = req_xd->sx.maxrec;

#if 0  /* XXX vrec wont support atm (and it seems to need work) */
	if (cd->maxrec != 0) {
		flags = fcntl(fd, F_GETFL, 0);
		if (flags == -1)
			return (FALSE);
		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
			return (FALSE);
		if (xd->shared.recvsz > xd->sx.maxrec)
			xd->shared.recvsz = xd->sx.maxrec;
		xd->shared.nonblock = TRUE;
		__xdrrec_setnonblock(&xd->shared.xdrs_in, xd->sx.maxrec);
		__xdrrec_setnonblock(&xd->shared.xdrs_out, xd->sx.maxrec);
	} else
		cd->nonblock = FALSE;
#else
	xd->shared.nonblock = FALSE;
#endif
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &xd->sx.last_recv);

	/* if parent has xp_recv_user_data, use it */
	if (xprt->xp_ops->xp_recv_user_data)
		xprt->xp_ops->xp_recv_user_data(xprt, newxprt,
						SVC_RQST_FLAG_NONE, NULL);

	return (FALSE);	/* there is never an rpc msg to be processed */
}

 /*ARGSUSED*/
static enum xprt_stat
rendezvous_stat(SVCXPRT *xprt)
{
	return (XPRT_IDLE);
}

/* XXX pending further unification
 */

static void
svc_vc_destroy_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	/* clears xprt from the xprt table (eg, idle scans) */
	svc_rqst_xprt_unregister(xprt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refs %" PRIu32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refs, tag, line);

	if ((xprt->xp_flags & SVC_XPRT_FLAG_CLOSE)
	    && xprt->xp_fd != RPC_ANYFD)
		(void)close(xprt->xp_fd);

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}

	if (xprt->xp_tp)
		mem_free(xprt->xp_tp, 0);
	if (xprt->xp_netid)
		mem_free(xprt->xp_netid, 0);

	svc_vc_xprt_free(xd);
}

static void
svc_vc_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	/* connection tracking--decrement now, he's dead jim */
	svc_vc_dec_nconns();

	/* destroy shared XDR record streams (once) */
	XDR_DESTROY(&(VC_DR(REC_XPRT(xprt))->shared.xdrs_in));

	svc_vc_destroy_it(xprt, flags, tag, line);
}

extern mutex_t ops_lock;

 /*ARGSUSED*/
static bool
svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
		*(u_int *) in = xprt->xp_flags;
		break;
	case SVCSET_XP_FLAGS:
		xprt->xp_flags = *(u_int *) in;
		break;
	case SVCGET_XP_RECV:
		mutex_lock(&ops_lock);
		*(xp_recv_t *) in = xprt->xp_ops->xp_recv;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_RECV:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_recv = *(xp_recv_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_GETREQ:
		mutex_lock(&ops_lock);
		*(xp_getreq_t *) in = xprt->xp_ops->xp_getreq;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_GETREQ:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_getreq = *(xp_getreq_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		*(xp_dispatch_t *) in = xprt->xp_ops->xp_dispatch;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_dispatch = *(xp_dispatch_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_RECV_USER_DATA:
		mutex_lock(&ops_lock);
		*(xp_recv_user_data_t *) in = xprt->xp_ops->xp_recv_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_RECV_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_recv_user_data = *(xp_recv_user_data_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		*(xp_free_user_data_t *) in = xprt->xp_ops->xp_free_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_free_user_data = *(xp_free_user_data_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (FALSE);
	}
	return (TRUE);
}

static bool
svc_vc_rendezvous_control(SVCXPRT *xprt, const u_int rq, void *in)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	switch (rq) {
	case SVCGET_CONNMAXREC:
		*(int *)in = xd->sx.maxrec;
		break;
	case SVCSET_CONNMAXREC:
		xd->sx.maxrec = *(int *)in;
		break;
	case SVCGET_XP_RECV:
		mutex_lock(&ops_lock);
		*(xp_recv_t *) in = xprt->xp_ops->xp_recv;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_RECV:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_recv = *(xp_recv_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_GETREQ:
		mutex_lock(&ops_lock);
		*(xp_getreq_t *) in = xprt->xp_ops->xp_getreq;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_GETREQ:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_getreq = *(xp_getreq_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		*(xp_dispatch_t *) in = xprt->xp_ops->xp_dispatch;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_DISPATCH:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_dispatch = *(xp_dispatch_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_RECV_USER_DATA:
		mutex_lock(&ops_lock);
		*(xp_recv_user_data_t *) in = xprt->xp_ops->xp_recv_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_RECV_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_recv_user_data = *(xp_recv_user_data_t) in;
		mutex_unlock(&ops_lock);
		break;
	case SVCGET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		*(xp_free_user_data_t *) in = xprt->xp_ops->xp_free_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_free_user_data = *(xp_free_user_data_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (FALSE);
	}
	return (TRUE);
}

static enum xprt_stat
svc_vc_stat(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_vc_xprt *xd = VC_DR(rec);
	enum xprt_stat result = XPRT_IDLE;
	uint16_t xp_flags = atomic_postclear_uint16_t_bits(&xprt->xp_flags,
							SVC_XPRT_FLAG_BLOCKED);

	if (xp_flags & SVC_XPRT_FLAG_BLOCKED) {
		if (xd->sx.strm_stat == XPRT_DIED)
			result = XPRT_DIED;
		else if (!xdr_inrec_eof(&(xd->shared.xdrs_in)))
			result = XPRT_MOREREQS;
		rpc_dplx_rui(rec);
		rpc_dplx_rsi(rec);
	}
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)
		return (XPRT_DESTROYED);

	return (result);
}

static bool
svc_vc_recv(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_vc_xprt *xd = VC_DR(rec);
	XDR *xdrs = &(xd->shared.xdrs_in);	/* recv queue */
	uint16_t xp_flags;

	/* SVC_RECV() locks dplx_rec, that is also unlocked and locked again
	 * for rpc_ctx.  Need to ensure we're the only one with the lock now.
	 */
	rpc_dplx_rli(rec);
	do {
		xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags,
							SVC_XPRT_FLAG_BLOCKED);
		if (!(xp_flags & SVC_XPRT_FLAG_BLOCKED))
			break;
		rpc_dplx_rwi(rec);
	} while (TRUE);

	/* XXX assert(! cd->nonblock) */
	if (xd->shared.nonblock) {
		if (!__xdrrec_getrec(xdrs, &xd->sx.strm_stat, TRUE))
			return FALSE;
	}

	xdrs->x_op = XDR_DECODE;
	xdrs->x_lib[1] = (void *)xprt;	/* transiently thread xprt */

	/* Consumes any remaining -fragment- bytes, and clears last_frag */
	(void)xdr_inrec_skiprecord(xdrs);

	rpc_msg_init(&req->rq_msg);

	/* Advances to next record, will read up to 1024 bytes
	 * into the stream. */
	(void)xdr_inrec_readahead(xdrs, 1024);

	if (xdr_dplx_decode(xdrs, &req->rq_msg)) {
		switch (req->rq_msg.rm_direction) {
		case CALL:
			/* an ordinary call header */
			return (TRUE);
			break;
		case REPLY:
			/* reply header (xprt OK) */
			rpc_ctx_xfer_replymsg(xd, &req->rq_msg);
			break;
		default:
			/* not good (but xprt OK) */
			break;
		}
		/* XXX skiprecord? */
		return (FALSE);
	}
	__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
		"%s: fd %d failed (will set xprt %p dead)",
		__func__, xprt->xp_fd, xprt);
	return (FALSE);
}

static bool
svc_vc_freeargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr)
{
	return xdr_free(xdr_args, args_ptr);
}

static bool
svc_vc_getargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr,
	       void *u_data)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(req->rq_xprt));
	XDR *xdrs = &xd->shared.xdrs_in;	/* recv queue */
	bool rslt;

	/* threads u_data for advanced decoders */
	xdrs->x_public = u_data;

	rslt = SVCAUTH_UNWRAP(req->rq_auth, req, xdrs, xdr_args, args_ptr);

	/* XXX Upstream TI-RPC lacks this call, but -does- call svc_dg_freeargs
	 * in svc_dg_getargs if SVCAUTH_UNWRAP fails. */
	if (rslt)
		req->rq_cksum = xdr_inrec_cksum(xdrs);
	else
		svc_vc_freeargs(req, xdr_args, args_ptr);

	return (rslt);
}

static bool
svc_vc_reply(struct svc_req *req)
{
	XDR *xdrs_2;
	xdrproc_t xdr_results;
	caddr_t xdr_location;
	bool rstat = false;
	bool has_args;
	bool gss;

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	    && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS) {
		has_args = TRUE;
		xdr_results = req->rq_msg.RPCM_ack.ar_results.proc;
		xdr_location = req->rq_msg.RPCM_ack.ar_results.where;

		req->rq_msg.RPCM_ack.ar_results.proc = (xdrproc_t) xdr_void;
		req->rq_msg.RPCM_ack.ar_results.where = NULL;
	} else {
		has_args = FALSE;
		xdr_results = NULL;
		xdr_location = NULL;
	}

	/* XXX Until gss_get_mic and gss_wrap can be replaced with
	 * iov equivalents, replies with RPCSEC_GSS security must be
	 * encoded in a contiguous buffer.
	 *
	 * Nb, we should probably use getpagesize() on Unix.  Need
	 * an equivalent for Windows.
	 */
	gss = (req->rq_msg.cb_cred.oa_flavor == RPCSEC_GSS);
	xdrs_2 = xdr_ioq_create(8192 /* default segment size */ ,
				__svc_params->svc_ioq_maxbuf + 8192,
				gss
				? UIO_FLAG_REALLOC | UIO_FLAG_FREE
				: UIO_FLAG_FREE);
	if (xdr_replymsg(xdrs_2, &req->rq_msg)
	    && (!has_args
		|| (req->rq_auth
		    && SVCAUTH_WRAP(req->rq_auth, req, xdrs_2, xdr_results,
				    xdr_location)))) {
		rstat = TRUE;
	}

	xdrs_2->x_lib[1] = (void *)req->rq_xprt;
	svc_ioq_write_now(req->rq_xprt, XIOQ(xdrs_2));
	return (rstat);
}

static void
svc_vc_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */
	mutex_lock(&ops_lock);

	xprt->xp_type = XPRT_TCP;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_vc_recv;
		ops.xp_stat = svc_vc_stat;
		ops.xp_getargs = svc_vc_getargs;
		ops.xp_reply = svc_vc_reply;
		ops.xp_freeargs = svc_vc_freeargs;
		ops.xp_destroy = svc_vc_destroy;
		ops.xp_control = svc_vc_control;
		ops.xp_getreq = svc_getreq_default;
		ops.xp_dispatch = svc_dispatch_default;
		ops.xp_recv_user_data = NULL;	/* no default */
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

static void
svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *newxprt)
{
	if (xprt->xp_ops->xp_getreq)
		newxprt->xp_ops->xp_getreq = xprt->xp_ops->xp_getreq;

	if (xprt->xp_ops->xp_dispatch)
		newxprt->xp_ops->xp_dispatch = xprt->xp_ops->xp_dispatch;

	if (xprt->xp_ops->xp_recv_user_data) {
		newxprt->xp_ops->xp_recv_user_data =
			xprt->xp_ops->xp_recv_user_data;
	}
	if (xprt->xp_ops->xp_free_user_data) {
		newxprt->xp_ops->xp_free_user_data =
			xprt->xp_ops->xp_free_user_data;
	}
}

static void
svc_vc_rendezvous_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	extern mutex_t ops_lock;

	mutex_lock(&ops_lock);

	xprt->xp_type = XPRT_TCP_RENDEZVOUS;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = rendezvous_request;
		ops.xp_stat = rendezvous_stat;
		/* XXX wow */
		ops.xp_getargs = (bool(*)
				  (struct svc_req *, xdrproc_t,
				   void *, void *))abort;
		ops.xp_reply = (bool(*)
				(struct svc_req *req))abort;
		ops.xp_freeargs = (bool(*)
				   (struct svc_req *, xdrproc_t,
				    void *))abort;
		ops.xp_destroy = svc_vc_destroy_it;
		ops.xp_control = svc_vc_rendezvous_control;
		ops.xp_getreq = svc_getreq_default;
		ops.xp_dispatch = svc_dispatch_default;
		ops.xp_recv_user_data = NULL;	/* no default */
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

/*
 * Get the effective UID of the sending process. Used by rpcbind, keyserv
 * and rpc.yppasswdd on AF_LOCAL.
 */
int
__rpc_get_local_uid(SVCXPRT *transp, uid_t *uid)
{
	int sock, ret;
	gid_t egid;
	uid_t euid;
	struct sockaddr *sa;

	sock = transp->xp_fd;
	sa = (struct sockaddr *)&transp->xp_remote.ss;
	if (sa->sa_family == AF_LOCAL) {
		ret = getpeereid(sock, &euid, &egid);
		if (ret == 0)
			*uid = euid;
		return (ret);
	} else
		return (-1);
}

/*
 * Destroy xprts that have not have had any activity in 'timeout' seconds.
 * If 'cleanblock' is true, blocking connections (the default) are also
 * cleaned. If timeout is 0, the least active connection is picked.
 *
 * Though this is not a publicly documented interface, some versions of
 * rpcbind are known to call this function.  Do not alter or remove this
 * API without changing the library's sonum.
 */

bool
__svc_clean_idle(fd_set *fds, int timeout, bool cleanblock)
{
	return (__svc_clean_idle2(timeout, cleanblock));

}				/* __svc_clean_idle */

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */

struct svc_clean_idle_arg {
	SVCXPRT *least_active;
	struct timespec ts, tmax;
	int cleanblock, ncleaned, timeout;
};

static uint32_t
svc_clean_idle2_func(SVCXPRT *xprt, void *arg)
{
	struct timespec tdiff;
	struct svc_clean_idle_arg *acc = (struct svc_clean_idle_arg *)arg;
	uint32_t rflag = SVC_XPRT_FOREACH_NONE;

	if (!acc->cleanblock)
		goto out;

	mutex_lock(&xprt->xp_lock);

	/* invalid xprt (error) */
	if (xprt->xp_ops == NULL)
		goto unlock;

	if (xprt->xp_ops->xp_recv != svc_vc_recv)
		goto unlock;

	if (xprt->xp_flags & (SVC_XPRT_FLAG_DESTROYED | SVC_XPRT_FLAG_UREG))
		goto unlock;

	{
		/* XXX nb., safe because xprt type is verfied */
		struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));
		if (!xd->shared.nonblock)
			goto unlock;

		if (acc->timeout == 0) {
			tdiff = acc->ts;
			timespecsub(&tdiff, &xd->sx.last_recv);
			if (timespeccmp(&tdiff, &acc->tmax, >)) {
				acc->tmax = tdiff;
				acc->least_active = xprt;
			}
			goto unlock;
		}
		if (acc->ts.tv_sec - xd->sx.last_recv.tv_sec > acc->timeout) {
			rflag = SVC_XPRT_FOREACH_CLEAR;
			mutex_unlock(&xprt->xp_lock);
			SVC_DESTROY(xprt);
			acc->ncleaned++;
			goto out;
		}
	}

 unlock:
	mutex_unlock(&xprt->xp_lock);

 out:
	return (rflag);
}

/* XXX move to svc_run */
void authgss_ctx_gc_idle(void);

bool
__svc_clean_idle2(int timeout, bool cleanblock)
{
	struct svc_clean_idle_arg acc;
	static mutex_t active_mtx = MUTEX_INITIALIZER;
	static uint32_t active;
	bool_t rslt = FALSE;

	if (mutex_trylock(&active_mtx) != 0)
		goto out;

	if (active > 0)
		goto unlock;

	++active;

	/* trim gss context cache */
	authgss_ctx_gc_idle();

	/* trim xprts (not sorted, not aggressive [but self limiting]) */
	memset(&acc, 0, sizeof(struct svc_clean_idle_arg));
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &acc.ts);
	acc.cleanblock = cleanblock;
	acc.timeout = timeout;

	svc_xprt_foreach(svc_clean_idle2_func, (void *)&acc);

	if (timeout == 0 && acc.least_active != NULL) {
		SVC_DESTROY(acc.least_active);
		acc.ncleaned++;
	}
	rslt = (acc.ncleaned > 0) ? TRUE : FALSE;
	--active;

 unlock:
	mutex_unlock(&active_mtx);
 out:
	return (rslt);

}				/* __svc_clean_idle2 */
