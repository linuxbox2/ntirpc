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
int generic_write_vc(XDR *, void *, void *, int);

static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_ops(SVCXPRT *);
static void svc_vc_override_ops(SVCXPRT *, SVCXPRT *);

bool __svc_clean_idle2(int, bool);
static SVCXPRT *makefd_xprt(int, u_int, u_int, bool *);

extern pthread_mutex_t svc_ctr_lock;

/*
 * Usage:
 * xprt = svc_vc_ncreate(sock, send_buf_size, recv_buf_size);
 *
 * Creates, registers, and returns a (rpc) tcp based transport.
 * Once *xprt is initialized, it is registered as a transport
 * see (svc.h, xprt_register).  This routine returns
 * a NULL if a problem occurred.
 *
 * Since streams do buffered io similar to stdio, the caller can specify
 * how big the send and receive buffers are via the second and third parms;
 * 0 => use the system default.
 *
 * Added svc_vc_ncreate2 with flags argument, has the behavior of the
 * original function if flags are SVC_VC_FLAG_NONE (0).
 *
 */
SVCXPRT *
svc_vc_ncreate2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
	SVCXPRT *xprt = NULL;
	struct cf_rendezvous *rdvs = NULL;
	struct __rpc_sockinfo si;
	struct sockaddr_storage sslocal;
	struct sockaddr *salocal;
	struct sockaddr_in *salocal_in;
	struct sockaddr_in6 *salocal_in6;
	struct rpc_dplx_rec *rec = NULL;
	struct x_vc_data *xd = NULL;
	const char *netid;
	uint32_t oflags;
	socklen_t slen;

	if (!__rpc_fd2sockinfo(fd, &si))
		return NULL;

	if (!__rpc_sockinfo2netid(&si, &netid))
		return NULL;

	rdvs = mem_alloc(sizeof(struct cf_rendezvous));
	if (rdvs == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc_ncreate: out of memory");
		goto err;
	}
	rdvs->sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
	rdvs->recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);
	rdvs->maxrec = __svc_maxrec;

	/* atomically find or create shared fd state */
	rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
	if (!rec) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc: makefd_xprt: rpc_dplx_lookup_rec failed");
		goto err;
	}

	/* attach shared state */
	if ((oflags & RPC_DPLX_LKP_OFLAG_ALLOC) || (!rec->hdl.xd)) {
		xd = rec->hdl.xd = alloc_x_vc_data();
		if (xd == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"svc_vc: makefd_xprt: out of memory");
			goto err;
		}

		xd->rec = rec;

		/* XXX tracks outstanding calls */
		opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
		xd->cx.calls.xid = 0;	/* next call xid is 1 */
		xd->refcnt = 1;

		xd->shared.sendsz =
		    __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
		xd->shared.recvsz =
		    __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);

		/* duplex streams are not used by the rendevous transport */
		memset(&xd->shared.xdrs_in, 0, sizeof xd->shared.xdrs_in);
		memset(&xd->shared.xdrs_out, 0, sizeof xd->shared.xdrs_out);
	} else {
		xd = (struct x_vc_data *)rec->hdl.xd;
		/* dont return destroyed xprts */
		if (!(xd->flags & X_VC_DATA_FLAG_SVC_DESTROYED)) {
			if (rec->hdl.xprt) {
				xprt = rec->hdl.xprt;
				/* inc xprt refcnt */
				SVC_REF(xprt, SVC_REF_FLAG_NONE);
				mem_free(rdvs, sizeof(struct cf_rendezvous));
				goto done;
			} else
				++(xd->refcnt);
		}
		/* return extra ref */
		rpc_dplx_unref(rec,
			       RPC_DPLX_FLAG_LOCKED | RPC_DPLX_FLAG_UNLOCK);
	}

	xprt = mem_zalloc(sizeof(SVCXPRT));
	if (xprt == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc_ncreate: out of memory");
		goto err;
	}
	xprt->xp_flags = SVC_XPRT_FLAG_NONE;
	xprt->xp_refs = 1;
	svc_vc_rendezvous_ops(xprt);
	xprt->xp_p1 = rdvs;
	xprt->xp_p2 = xd;
	xprt->xp_p5 = rec;
	xprt->xp_fd = fd;
	mutex_init(&xprt->xp_lock, NULL);

	/* caller should know what it's doing */
	if (flags & SVC_VC_CREATE_LISTEN)
		listen(fd, SOMAXCONN);

	slen = sizeof(struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&sslocal, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc_create: could not retrieve local addr");
		goto err;
	}

	/* XXX following breaks strict aliasing? */
	salocal = (struct sockaddr *)&sslocal;
	switch (salocal->sa_family) {
	case AF_INET:
		salocal_in = (struct sockaddr_in *)salocal;
		xprt->xp_port = ntohs(salocal_in->sin_port);
		break;
	case AF_INET6:
		salocal_in6 = (struct sockaddr_in6 *)salocal;
		xprt->xp_port = ntohs(salocal_in6->sin6_port);
		break;
	}
	__rpc_set_address(&xprt->xp_local, &sslocal, slen);

	xprt->xp_netid = rpc_strdup(netid);

	/* make reachable from rec */
	rec->hdl.xprt = xprt;

	/* release rec */
	REC_UNLOCK(rec);

	/* make reachable from xprt list */
	svc_rqst_init_xprt(xprt);

	/* conditional xprt_register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
	    && (!(flags & SVC_VC_CREATE_XPRT_NOREG)))
		xprt_register(xprt);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_vc");
#endif

 done:
	return (xprt);

 err:
	if (rdvs)
		mem_free(rdvs, sizeof(struct cf_rendezvous));

	if (xprt) {
#if defined(HAVE_BLKIN)
		if (xprt->blkin.svc_name)
			mem_free(xprt->blkin.svc_name, 2*INET6_ADDRSTRLEN);
#endif
		mem_free(xprt, sizeof(SVCXPRT));
	}

	if (rec) {
		rpc_dplx_unref(rec,
			       RPC_DPLX_FLAG_LOCKED | RPC_DPLX_FLAG_UNLOCK);
	}

	return (NULL);
}

SVCXPRT *
svc_vc_ncreate(int fd, u_int sendsize, u_int recvsize)
{
	return (svc_vc_ncreate2(fd, sendsize, recvsize, SVC_VC_CREATE_NONE));
}

/*
 * Like svtcp_ncreate(), except the routine takes any *open* UNIX file
 * descriptor as its first input.
 */
SVCXPRT *
svc_fd_ncreate(int fd, u_int sendsize, u_int recvsize)
{
	struct sockaddr_storage ss;
	socklen_t slen;
	SVCXPRT *xprt;
	bool xprt_allocd;

	assert(fd != -1);

	xprt = makefd_xprt(fd, sendsize, recvsize, &xprt_allocd);
	if ((!xprt) || (!xprt_allocd))	/* ref'd existing xprt handle */
		goto done;

	/* conditional xprt_register */
	if (!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
		xprt_register(xprt);

	slen = sizeof(struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_fd_create: could not retrieve local addr");
		goto freedata;
	}
	__rpc_set_address(&xprt->xp_local, &ss, slen);

	slen = sizeof(struct sockaddr_storage);
	if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_fd_create: could not retrieve remote addr");
		goto freedata;
	}
	__rpc_set_address(&xprt->xp_remote, &ss, slen);

 done:
	return (xprt);

 freedata:
	return (NULL);
}

/*
 * Like sv_fd_ncreate(), except export flags for additional control.
 */
SVCXPRT *
svc_fd_ncreate2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
	struct sockaddr_storage ss;
	socklen_t slen;
	SVCXPRT *xprt;
	bool xprt_allocd;

	assert(fd != -1);

	xprt = makefd_xprt(fd, sendsize, recvsize, &xprt_allocd);
	if ((!xprt) || (!xprt_allocd))	/* ref'd existing xprt handle */
		return (xprt);

	slen = sizeof(struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_fd_ncreate: could not retrieve local addr");
		return (NULL);
	}
	__rpc_set_address(&xprt->xp_local, &ss, slen);

	slen = sizeof(struct sockaddr_storage);
	if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_fd_ncreate: could not retrieve remote addr");
		return (NULL);
	}
	__rpc_set_address(&xprt->xp_remote, &ss, slen);

	/* conditional xprt_register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
	    && (!(flags & SVC_VC_CREATE_XPRT_NOREG)))
		xprt_register(xprt);

	return (xprt);
}

static SVCXPRT *
makefd_xprt(int fd, u_int sendsz, u_int recvsz, bool *allocated)
{
	SVCXPRT *xprt = NULL;
	struct x_vc_data *xd = NULL;
	struct rpc_dplx_rec *rec;
	struct __rpc_sockinfo si;
	const char *netid;
	uint32_t oflags;
	bool newxd = false;
	*allocated = false;

	assert(fd != -1);

	if (!svc_vc_new_conn_ok()) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: makefd_xprt: max_connections exceeded\n",
			__func__);
		goto done;
	}

	/* atomically find or create shared fd state */
	rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
	if (!rec) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc: makefd_xprt: rpc_dplx_lookup_rec failed");
		goto done;
	}

	/* attach shared state */
	if ((oflags & RPC_DPLX_LKP_OFLAG_ALLOC) || (!rec->hdl.xd)) {
		newxd = true;
		xd = rec->hdl.xd = alloc_x_vc_data();
		if (xd == NULL) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"svc_vc: makefd_xprt: out of memory");
			/* return extra ref */
			rpc_dplx_unref(rec,
				       RPC_DPLX_FLAG_LOCKED |
				       RPC_DPLX_FLAG_UNLOCK);
			mem_free(xprt, sizeof(SVCXPRT));
			goto done;
		}

		xd->rec = rec;

		/* XXX tracks outstanding calls */
		opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
		xd->cx.calls.xid = 0;	/* next call xid is 1 */
		xd->refcnt = 1;

		if (__rpc_fd2sockinfo(fd, &si)) {
			xd->shared.sendsz =
				__rpc_get_t_size(
					si.si_af, si.si_proto, (int)sendsz);
			xd->shared.recvsz =
				__rpc_get_t_size(
					si.si_af, si.si_proto, (int)recvsz);
		}

		/* duplex streams */
		xdr_inrec_create(&(xd->shared.xdrs_in), recvsz, xd,
				 generic_read_vc);
		xd->shared.xdrs_in.x_op = XDR_DECODE;

		xdrrec_create(&(xd->shared.xdrs_out), sendsz, recvsz, xd,
			      generic_read_vc, generic_write_vc);
		xd->shared.xdrs_out.x_op = XDR_ENCODE;
	} else {
		xd = (struct x_vc_data *)rec->hdl.xd;
		/* dont return destroyed xprts */
		if (!(xd->flags & X_VC_DATA_FLAG_SVC_DESTROYED)) {
			if (rec->hdl.xprt) {
				xprt = rec->hdl.xprt;
				/* inc xprt refcnt */
				SVC_REF(xprt, SVC_REF_FLAG_NONE);
			} else
				++(xd->refcnt);
		}
		/* return extra ref */
		rpc_dplx_unref(rec,
			       RPC_DPLX_FLAG_LOCKED | RPC_DPLX_FLAG_UNLOCK);
		*allocated = FALSE;

		/* return ref'd xprt */
		goto done_xprt;
	}

	/* XXX bi-directional?  initially I had assumed that explicit
	 * routines to create a clnt or svc handle from an already-connected
	 * handle of the other type, but perhaps it is more natural to
	 * just discover it
	 */

	/* new xprt (the common case) */
	xprt = mem_zalloc(sizeof(SVCXPRT));
	if (xprt == NULL) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"svc_vc: makefd_xprt: out of memory");
		/* return extra ref */
		rpc_dplx_unref(rec,
			       RPC_DPLX_FLAG_LOCKED | RPC_DPLX_FLAG_UNLOCK);
		goto done;
	}

	*allocated = TRUE;
	xprt->xp_p5 = rec;
	mutex_init(&xprt->xp_lock, NULL);
	/* XXX take xp_lock? */
	mutex_init(&xprt->xp_auth_lock, NULL);
	xprt->xp_refs = 1;
	xprt->xp_fd = fd;

	/* the SVCXPRT created in svc_vc_create accepts new connections
	 * in its xp_recv op, the rendezvous_request method, but xprt is
	 * a call channel */
	svc_vc_ops(xprt);

	xd->sx.strm_stat = XPRT_IDLE;

	xprt->xp_p1 = xd;
	if (newxd /* ensures valid si */ && __rpc_sockinfo2netid(&si, &netid))
		xprt->xp_netid = rpc_strdup(netid);

	/* make reachable from rec */
	rec->hdl.xprt = xprt;

	/* release */
	REC_UNLOCK(rec);

	/* Make reachable from xprt list.  Registration deferred. */
	svc_rqst_init_xprt(xprt);

done_xprt:
done:
	return (xprt);
}

 /*ARGSUSED*/
static bool
rendezvous_request(SVCXPRT *xprt, struct svc_req *req)
{
	int fd;
	socklen_t len;
	struct cf_rendezvous *rdvs;
	struct x_vc_data *xd;
	struct sockaddr_storage addr;
	struct __rpc_sockinfo si;
	socklen_t slen;
	SVCXPRT *newxprt;
	bool xprt_allocd;
	static int n = 1;

	rdvs = (struct cf_rendezvous *)xprt->xp_p1;
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
	newxprt = makefd_xprt(fd, rdvs->sendsize, rdvs->recvsize, &xprt_allocd);
	if ((!newxprt) || (!xprt_allocd)) /* ref'd existing xprt handle */
		return (FALSE);

	/*
	 * propagate special ops
	 */
	svc_vc_override_ops(xprt, newxprt);

	/* move xprt_register() out of makefd_xprt */
	(void)svc_rqst_xprt_register(xprt, newxprt);

	__rpc_set_address(&newxprt->xp_remote, &addr, len);
	XPRT_TRACE(newxprt, __func__, __func__, __LINE__);

	/* XXX fvdl - is this useful? (Yes.  Matt) */
	if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
		len = 1;
		(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len,
				  sizeof(len));
	}

	slen = sizeof(struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&addr, &slen) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: could not retrieve local addr", __func__);
	} else {
		__rpc_set_address(&newxprt->xp_local, &addr, slen);
	}

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(newxprt, "svc_vc");
#endif

	xd = (struct x_vc_data *)newxprt->xp_p1;
	xd->shared.recvsz = rdvs->recvsize;
	xd->shared.sendsz = rdvs->sendsize;
	xd->sx.maxrec = rdvs->maxrec;

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
 *
 * note:  currently, rdvs xprt handles have a rec structure,
 * but no xd structure, etc.
 * (they do too have an xd -- to track "destroyed" flag -- fixme?)
 *
 */

static void
svc_rdvs_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct cf_rendezvous *rdvs = (struct cf_rendezvous *)xprt->xp_p1;
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p2;
	struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *)xprt->xp_p5;
	int refcnt;

	/* clears xprt from the xprt table (eg, idle scans) */
	xprt_unregister(xprt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refs %" PRIu32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refs, tag, line);

	if (xprt->xp_fd != RPC_ANYFD)
		(void)close(xprt->xp_fd);

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}

	REC_LOCK(rec);

	mutex_destroy(&xprt->xp_lock);

	if (xprt->xp_tp)
		mem_free(xprt->xp_tp, 0);
	if (xprt->xp_netid)
		mem_free(xprt->xp_netid, 0);

	mem_free(rdvs, sizeof(struct cf_rendezvous));
	mem_free(xprt, sizeof(SVCXPRT));

	refcnt = rpc_dplx_unref(rec,
				RPC_DPLX_FLAG_LOCKED | RPC_DPLX_FLAG_UNLOCK);
	if (!refcnt)
		mem_free(xd, sizeof(struct x_vc_data));
}

static void
svc_vc_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;
	struct rpc_dplx_rec *rec = xd->rec;
	uint32_t xd_refcnt;

	/* connection tracking--decrement now, he's dead jim */
	svc_vc_dec_nconns();

	/* clears xprt from the xprt table (eg, idle scans) */
	xprt_unregister(xprt);

	/* bidirectional */
	REC_LOCK(rec);
	xd->flags |= X_VC_DATA_FLAG_SVC_DESTROYED;
	xd_refcnt = --(xd->refcnt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s: postfinalize %p xp_refs %" PRIu32
		" xd_refcnt %u",
		__func__, xprt, xprt->xp_refs, xd_refcnt);

	/* conditional destroy */
	if (xd_refcnt == 0) {
		__warnx(TIRPC_DEBUG_FLAG_REFCNT,
			"%s: %p xp_refs %" PRIu32
			" xd_refcnt %" PRIu32
			" calling vc_shared_destroy @ %s:%d",
			__func__, xprt, xprt->xp_refs, xd_refcnt, tag, line);
		vc_shared_destroy(xd);	/* RECLOCKED */
	} else {
		__warnx(TIRPC_DEBUG_FLAG_REFCNT,
			"%s: %p xp_refs %" PRIu32
			" xd_refcnt %" PRIu32
			" omit vc_shared_destroy @ %s:%d",
			__func__, xprt, xprt->xp_refs, xd_refcnt, tag, line);
		REC_UNLOCK(rec);
	}
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
	struct cf_rendezvous *cfp;

	cfp = (struct cf_rendezvous *)xprt->xp_p1;
	if (cfp == NULL)
		return (FALSE);
	switch (rq) {
	case SVCGET_CONNMAXREC:
		*(int *)in = cfp->maxrec;
		break;
	case SVCSET_CONNMAXREC:
		cfp->maxrec = *(int *)in;
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
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;

	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)
		return (XPRT_DESTROYED);

	if (!xd)
		return (XPRT_IDLE);

	/* we hold the recv lock */
	if (xd->sx.strm_stat == XPRT_DIED)
		return (XPRT_DIED);

	if (!xdr_inrec_eof(&(xd->shared.xdrs_in)))
		return (XPRT_MOREREQS);

	return (XPRT_IDLE);
}

static bool
svc_vc_recv(SVCXPRT *xprt, struct svc_req *req)
{
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;
	XDR *xdrs = &(xd->shared.xdrs_in);	/* recv queue */

	/* XXX assert(! cd->nonblock) */
	if (xd->shared.nonblock) {
		if (!__xdrrec_getrec(xdrs, &xd->sx.strm_stat, TRUE))
			return FALSE;
	}

	xdrs->x_op = XDR_DECODE;

	xdrs->x_lib[0] = (void *)RPC_DPLX_SVC;
	xdrs->x_lib[1] = (void *)xprt;	/* transiently thread xprt */

	/* Consumes any remaining -fragment- bytes, and clears last_frag */
	(void)xdr_inrec_skiprecord(xdrs);

	req->rq_msg = alloc_rpc_msg();
	req->rq_clntcred = req->rq_msg->rq_cred_body;

	/* Advances to next record, will read up to 1024 bytes
	 * into the stream. */
	(void)xdr_inrec_readahead(xdrs, 1024);

	if (xdr_dplx_decode(xdrs, req->rq_msg)) {
		switch (req->rq_msg->rm_direction) {
		case CALL:
			/* an ordinary call header */
			req->rq_xprt = xprt;
			req->rq_prog = req->rq_msg->rm_call.cb_prog;
			req->rq_vers = req->rq_msg->rm_call.cb_vers;
			req->rq_proc = req->rq_msg->rm_call.cb_proc;
			req->rq_xid = req->rq_msg->rm_xid;
			return (TRUE);
			break;
		case REPLY:
			/* reply header (xprt OK) */
			rpc_ctx_xfer_replymsg(xd, req->rq_msg);
			req->rq_msg = NULL;
			break;
		default:
			/* not good (but xprt OK) */
			break;
		}
		/* XXX skiprecord? */
		return (FALSE);
	}
	__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
		"%s: xdr_dplx_msg_decode failed (will set dead)", __func__);
	return (FALSE);
}

static bool
svc_vc_freeargs(SVCXPRT *xprt, struct svc_req *req, xdrproc_t xdr_args,
		void *args_ptr)
{
	return xdr_free(xdr_args, args_ptr);
}

static bool
svc_vc_getargs(SVCXPRT *xprt, struct svc_req *req,
	       xdrproc_t xdr_args, void *args_ptr, void *u_data)
{
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;
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
		svc_vc_freeargs(xprt, req, xdr_args, args_ptr);

	return (rslt);
}

static bool
svc_vc_reply(SVCXPRT *xprt, struct svc_req *req, struct rpc_msg *msg)
{
	struct x_vc_data *xd = (struct x_vc_data *)xprt->xp_p1;
	XDR *xdrs_2;
	xdrproc_t xdr_results;
	caddr_t xdr_location;
	bool rstat = false;
	bool has_args;
	bool gss;

	if (msg->rm_reply.rp_stat == MSG_ACCEPTED
	    && msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
		has_args = TRUE;
		xdr_results = msg->acpted_rply.ar_results.proc;
		xdr_location = msg->acpted_rply.ar_results.where;

		msg->acpted_rply.ar_results.proc = (xdrproc_t) xdr_void;
		msg->acpted_rply.ar_results.where = NULL;
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
	gss = (req->rq_cred.oa_flavor == RPCSEC_GSS);
	xdrs_2 = xdr_ioq_create(8192 /* default segment size */ ,
				__svc_params->svc_ioq_maxbuf + 8192,
				gss
				? UIO_FLAG_REALLOC | UIO_FLAG_FREE
				: UIO_FLAG_FREE);
	if (xdr_replymsg(xdrs_2, msg)
	    && (!has_args
		|| (req->rq_auth
		    && SVCAUTH_WRAP(req->rq_auth, req, xdrs_2, xdr_results,
				    xdr_location)))) {
		rstat = TRUE;
	}
	svc_ioq_append(xprt, xd, xdrs_2);
	return (rstat);
}

static void
svc_vc_lock(SVCXPRT *xprt, uint32_t flags, const char *func, int line)
{
	if (flags & XP_LOCK_RECV)
		rpc_dplx_rlxi(xprt, func, line);

	if (flags & XP_LOCK_SEND)
		rpc_dplx_slxi(xprt, func, line);
}

static void
svc_vc_unlock(SVCXPRT *xprt, uint32_t flags, const char *func, int line)
{
	if (flags & XP_LOCK_RECV)
		rpc_dplx_rux(xprt);

	if (flags & XP_LOCK_SEND)
		rpc_dplx_sux(xprt);
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
		ops.xp_lock = svc_vc_lock;
		ops.xp_unlock = svc_vc_unlock;
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
	if (xprt->xp_ops->xp_recv_user_data)
		newxprt->xp_ops->xp_recv_user_data = xprt->xp_ops->xp_recv_user_data;
	if (xprt->xp_ops->xp_free_user_data)
		newxprt->xp_ops->xp_free_user_data = xprt->xp_ops->xp_free_user_data;
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
				  (SVCXPRT *, struct svc_req *, xdrproc_t,
				   void *, void *))abort;
		ops.xp_reply = (bool(*)
				(SVCXPRT *, struct svc_req *req,
				 struct rpc_msg *))abort;
		ops.xp_freeargs = (bool(*)
				   (SVCXPRT *, struct svc_req *, xdrproc_t,
				    void *))abort;
		ops.xp_destroy = svc_rdvs_destroy;
		ops.xp_control = svc_vc_rendezvous_control;
		ops.xp_lock = svc_vc_lock;
		ops.xp_unlock = svc_vc_unlock;
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

	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
		/* XXX should not happen--but do no harm */
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: destroyed xprt %p seen in clean idle\n",
			__func__,
			xprt);
		goto unlock;
	}

	if (xprt->xp_ops->xp_recv != svc_vc_recv)
		goto unlock;

	{
		/* XXX nb., safe because xprt type is verfied */
		struct x_vc_data *xd = xprt->xp_p1;
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

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 *
 * If flags & SVC_VC_CLNT_CREATE_DEDICATED, the supplied xprt will be
 * unregistered and disposed inline.
 */
CLIENT *
clnt_vc_ncreate_svc(SVCXPRT *xprt, const rpcprog_t prog,
		    const rpcvers_t vers, const uint32_t flags)
{
	struct x_vc_data *xd;
	CLIENT *clnt;

	mutex_lock(&xprt->xp_lock);

	xd = (struct x_vc_data *)xprt->xp_p1;

	/* XXX return allocated client structure, or allocate one if none
	 * is currently allocated */

	clnt =
	    clnt_vc_ncreate2(xprt->xp_fd, &xprt->xp_remote.nb, prog, vers,
			     xd->shared.sendsz, xd->shared.recvsz,
			     CLNT_CREATE_FLAG_SVCXPRT);

	mutex_unlock(&xprt->xp_lock);

	/* for a dedicated channel, unregister and free xprt */
	if ((flags & SVC_VC_CREATE_ONEWAY) && (flags & SVC_VC_CREATE_DISPOSE)) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s:  disposing--calls svc_vc_destroy\n", __func__);
		svc_vc_destroy(xprt, 0, __func__, __LINE__);
	}

	return (clnt);
}

/*
 * Create an RPC SVCXPRT handle from an active client transport
 * handle, i.e., to service RPC requests.
 *
 * If flags & SVC_VC_CREATE_CL_FLAG_DEDICATED, then clnt is also
 * deallocated without closing cl->cl_p1->ct_fd.
 */
SVCXPRT *
svc_vc_ncreate_clnt(CLIENT *clnt, const u_int sendsz,
		    const u_int recvsz, const uint32_t flags)
{
	int fd;
	socklen_t len;
	struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
	struct ct_data *ct = &xd->cx.data;
	struct sockaddr_storage addr;
	struct __rpc_sockinfo si;
	SVCXPRT *xprt = NULL;
	bool xprt_allocd;

	fd = ct->ct_fd;
	rpc_dplx_rlc(clnt);
	rpc_dplx_slc(clnt);

	len = sizeof(struct sockaddr_storage);
	if (getpeername(fd, (struct sockaddr *)(void *)&addr, &len) < 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
			"%s: could not retrieve remote addr", __func__);
		goto unlock;
	}

	/*
	 * make a new transport
	 */

	xprt = makefd_xprt(fd, sendsz, recvsz, &xprt_allocd);
	if ((!xprt) || (!xprt_allocd))	/* ref'd existing xprt handle */
		goto unlock;

	__rpc_set_address(&xprt->xp_remote, &addr, len);

	if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
		len = 1;
		(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len,
				  sizeof(len));
	}

	xd->sx.maxrec = __svc_maxrec;	/* XXX check */

#if 0				/* XXX wont currently support */
	if (xd->sx.maxrec != 0) {
		fflags = fcntl(fd, F_GETFL, 0);
		if (fflags == -1)
			return (FALSE);
		if (fcntl(fd, F_SETFL, fflags | O_NONBLOCK) == -1)
			return (FALSE);
		if (xd->shared.recvsz > xd->sx.maxrec)
			xd->shared.recvsz = xd->sx.maxrec;
		cd->nonblock = TRUE;
		__xdrrec_setnonblock(&cd->xdrs, xd->sx.maxrec);
	} else
		xd->shared.nonblock = FALSE;
#else
	xd->shared.nonblock = FALSE;
#endif
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &xd->sx.last_recv);

	/* conditional xprt_register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
	    && (!(flags & SVC_VC_CREATE_XPRT_NOREG)))
		xprt_register(xprt);

	/* If creating a dedicated channel collect the supplied client
	 * without closing fd */
	if ((flags & SVC_VC_CREATE_ONEWAY) && (flags & SVC_VC_CREATE_DISPOSE)) {
		ct->ct_closeit = FALSE;	/* must not close */
		rpc_dplx_ruc(clnt);
		rpc_dplx_suc(clnt);
		CLNT_DESTROY(clnt);	/* clean up immediately */
		goto out;
	}

unlock:
	rpc_dplx_ruc(clnt);
	rpc_dplx_suc(clnt);

out:
	return (xprt);
}
