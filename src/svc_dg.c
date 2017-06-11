
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

#include <config.h>

/*
 * svc_dg.c, Server side for connectionless RPC.
 *
 * Does some caching in the hopes of achieving execute-at-most-once semantics.
 */
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netconfig.h>
#include <err.h>

#include "rpc_com.h"
#include "rpc_ctx.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include <rpc/svc_rqst.h>
#include <misc/city.h>
#include <rpc/rpc_cksum.h>

#define rpc_buffer(xprt) ((xprt)->xp_p1)

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static void svc_dg_ops(SVCXPRT *);

static void svc_dg_enable_pktinfo(int, const struct __rpc_sockinfo *);
static int svc_dg_store_pktinfo(struct msghdr *, struct svc_req *);

/*
 * Usage:
 * xprt = svc_dg_ncreate(sock, sendsize, recvsize);
 *
 * If recvsize or sendsize are 0 suitable,
 * system defaults are chosen.
 * If a problem occurred, this routine returns NULL.
 */
static void
svc_dg_xprt_free(struct svc_dg_xprt *su)
{
	rpc_dplx_rec_destroy(&su->su_dr);
	mutex_destroy(&su->su_dr.xprt.xp_lock);
	mutex_destroy(&su->su_dr.xprt.xp_auth_lock);

#if defined(HAVE_BLKIN)
	if (su->su_dr.xprt.blkin.svc_name)
		mem_free(su->su_dr.xprt.blkin.svc_name, 2*INET6_ADDRSTRLEN);
#endif
	mem_free(su, sizeof(struct svc_dg_xprt));
}

static struct svc_dg_xprt *
svc_dg_xprt_zalloc(void)
{
	struct svc_dg_xprt *su = mem_zalloc(sizeof(struct svc_dg_xprt));

	/* Init SVCXPRT locks, etc */
	mutex_init(&su->su_dr.xprt.xp_lock, NULL);
	mutex_init(&su->su_dr.xprt.xp_auth_lock, NULL);
/*	TAILQ_INIT_ENTRY(&su->su_dr.xprt, xp_evq); sets NULL */
	rpc_dplx_rec_init(&su->su_dr);

	su->su_dr.xprt.xp_refs = 1;
	return (su);
}

static void
svc_dg_xprt_setup(SVCXPRT **sxpp)
{
	if (unlikely(*sxpp)) {
		svc_dg_xprt_free(su_data(*sxpp));
		*sxpp = NULL;
	} else {
		struct svc_dg_xprt *su = svc_dg_xprt_zalloc();

		*sxpp = &su->su_dr.xprt;
	}
}

SVCXPRT *
svc_dg_ncreatef(const int fd, const u_int sendsz, const u_int recvsz,
		const uint32_t flags)
{
	SVCXPRT *xprt;
	struct rpc_dplx_rec *rec;
	struct svc_dg_xprt *su;
	struct __rpc_sockinfo si;
	u_int recvsize;
	u_int sendsize;
	u_int xp_flags;
	int rc;

	/* atomically find or create shared fd state; ref+1; locked */
	xprt = svc_xprt_lookup(fd, svc_dg_xprt_setup);
	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d svc_xprt_lookup failed",
			__func__, fd);
		return (NULL);
	}
	rec = REC_XPRT(xprt);

	xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags, flags
						| SVC_XPRT_FLAG_INITIALIZED);
	if ((xp_flags & SVC_XPRT_FLAG_INITIALIZED)) {
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
	/*
	 * Find the receive and the send size
	 */
	sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
	recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);
	if ((sendsize == 0) || (recvsize == 0)) {
		atomic_clear_uint16_t_bits(&xprt->xp_flags,
					   SVC_XPRT_FLAG_INITIALIZED);
		rpc_dplx_rui(rec);
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: fd %d transport does not support data transfer",
			__func__, fd);
		return (NULL);
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

	/*
	 * Should be multiple of 4 for XDR.
	 */
	su = DG_DR(rec);
	su->su_sendsz = ((sendsize + 3) / 4) * 4;
	su->su_recvsz = ((recvsize + 3) / 4) * 4;
	su->su_iosz = ((MAX(sendsize, recvsize) + 3) / 4) * 4;
	rpc_buffer(xprt) = mem_alloc(su->su_iosz);

	xdrmem_create(su->su_dr.ioq.xdrs,  rpc_buffer(xprt), su->su_iosz,
		      XDR_DECODE);

	svc_dg_ops(xprt);

	/* Enable reception of IP*_PKTINFO control msgs */
	svc_dg_enable_pktinfo(fd, &si);

	/* release */
	rpc_dplx_rui(rec);
	XPRT_TRACE(xprt, __func__, __func__, __LINE__);

	/* Conditional register */
	if ((!(__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
	     && !(flags & SVC_CREATE_FLAG_XPRT_NOREG))
	    || (flags & SVC_CREATE_FLAG_XPRT_DOREG))
		svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				    SVC_RQST_FLAG_CHAN_AFFINITY);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(xprt, "svc_dg");
#endif

	return (xprt);
}

 /*ARGSUSED*/
static enum xprt_stat
svc_dg_stat(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	uint16_t xp_flags = atomic_postclear_uint16_t_bits(&xprt->xp_flags,
							SVC_XPRT_FLAG_BLOCKED);

	if (xp_flags & SVC_XPRT_FLAG_BLOCKED) {
		rpc_dplx_rui(rec);
		rpc_dplx_rsi(rec);
	}
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)
		return (XPRT_DESTROYED);

	return (XPRT_IDLE);
}

static void svc_dg_set_pktinfo(struct cmsghdr *cmsg, struct svc_req *req)
{
	switch (req->rq_daddr.ss_family) {
	case AF_INET: {
		struct in_pktinfo *pki = (struct in_pktinfo *)CMSG_DATA(cmsg);
		struct sockaddr_in *daddr =
			(struct sockaddr_in *)&req->rq_daddr;

		cmsg->cmsg_level = SOL_IP;
		cmsg->cmsg_type = IP_PKTINFO;
		pki->ipi_ifindex = 0;
#ifdef __FreeBSD__
		pki->ipi_addr = daddr->sin_addr;
#else
		pki->ipi_spec_dst = daddr->sin_addr;
#endif
		cmsg->cmsg_len = CMSG_LEN(sizeof(*pki));
		break;
	}
	case AF_INET6: {
		struct in6_pktinfo *pki = (struct in6_pktinfo *)CMSG_DATA(cmsg);
		struct sockaddr_in6 *daddr =
			(struct sockaddr_in6 *)&req->rq_daddr;

		cmsg->cmsg_level = SOL_IPV6;
		cmsg->cmsg_type = IPV6_PKTINFO;
		pki->ipi6_ifindex = daddr->sin6_scope_id;
		pki->ipi6_addr = daddr->sin6_addr;
		cmsg->cmsg_len = CMSG_LEN(sizeof(*pki));
		break;
	}
	default:
	       break;
	}
}

static bool
svc_dg_recv(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_dg_xprt *su = DG_DR(rec);
	XDR *xdrs = rec->ioq.xdrs;
	struct sockaddr *sp = (struct sockaddr *)&xprt->xp_remote.ss;
	struct msghdr *mesgp;
	struct iovec iov;
	ssize_t rlen;
	uint16_t xp_flags;

	__rpc_address_setup(&xprt->xp_remote);

	/* XXX same XDR used in both directions */
	rpc_dplx_rli(rec);
	do {
		xp_flags = atomic_postset_uint16_t_bits(&xprt->xp_flags,
							SVC_XPRT_FLAG_BLOCKED);
		if (!(xp_flags & SVC_XPRT_FLAG_BLOCKED))
			break;
		rpc_dplx_rwi(rec);
	} while (TRUE);

 again:
	iov.iov_base = rpc_buffer(xprt);
	iov.iov_len = su->su_iosz;
	mesgp = &su->su_msghdr;
	memset(mesgp, 0, sizeof(*mesgp));
	mesgp->msg_iov = &iov;
	mesgp->msg_iovlen = 1;
	mesgp->msg_name = sp;
	sp->sa_family = (sa_family_t) 0xffff;
	mesgp->msg_namelen = sizeof(struct sockaddr_storage);
	mesgp->msg_control = su->su_cmsg;
	mesgp->msg_controllen = sizeof(su->su_cmsg);

	rlen = recvmsg(xprt->xp_fd, mesgp, 0);

	if (sp->sa_family == (sa_family_t) 0xffff)
		return false;

	if (rlen == -1 && errno == EINTR)
		goto again;
	if (rlen == -1 || (rlen < (ssize_t) (4 * sizeof(u_int32_t))))
		return (false);

	xprt->xp_remote.nb.len = mesgp->msg_namelen;

	/* Check whether there's an IP_PKTINFO or IP6_PKTINFO control message.
	 * If yes, preserve it for svc_dg_reply; otherwise just zap any cmsgs */
	if (!svc_dg_store_pktinfo(mesgp, req)) {
		mesgp->msg_control = NULL;
		mesgp->msg_controllen = 0;
		req->rq_daddr_len = 0;
	}

	xdrs->x_op = XDR_DECODE;
	XDR_SETPOS(xdrs, 0);
	rpc_msg_init(&req->rq_msg);

	if (!xdr_callmsg(xdrs, &req->rq_msg))
		return (false);

	/* save remote address */
	req->rq_raddr_len = xprt->xp_remote.nb.len;
	memcpy(&req->rq_raddr, xprt->xp_remote.nb.buf, req->rq_raddr_len);

	/* the checksum */
	req->rq_cksum =
#if 1
	    CityHash64WithSeed(iov.iov_base, MIN(256, iov.iov_len), 103);
#else
	    calculate_crc32c(0, iov.iov_base, MIN(256, iov.iov_len));
#endif
	return (true);
}

static bool
svc_dg_reply(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_dg_xprt *su = DG_DR(rec);
	XDR *xdrs = rec->ioq.xdrs;
	bool stat = false;
	size_t slen;

	xdrproc_t xdr_results;
	caddr_t xdr_location;
	bool has_args;

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	    && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS) {
		has_args = true;
		xdr_results = req->rq_msg.RPCM_ack.ar_results.proc;
		xdr_location = req->rq_msg.RPCM_ack.ar_results.where;
		req->rq_msg.RPCM_ack.ar_results.proc = (xdrproc_t) xdr_void;
		req->rq_msg.RPCM_ack.ar_results.where = NULL;
	} else {
		xdr_results = NULL;
		xdr_location = NULL;
		has_args = false;
	}

	/* XXX same XDR used in both directions */
	rpc_dplx_rli(rec);
	xdrs->x_op = XDR_ENCODE;
	XDR_SETPOS(xdrs, 0);

	if (xdr_replymsg(xdrs, &req->rq_msg) && req->rq_raddr_len
	    && (!has_args
		||
		(SVCAUTH_WRAP
		 (req->rq_auth, req, xdrs, xdr_results, xdr_location)))) {
		struct msghdr *msg = &su->su_msghdr;
		struct cmsghdr *cmsg;
		struct iovec iov;

		iov.iov_base = rpc_buffer(xprt);
		iov.iov_len = slen = XDR_GETPOS(xdrs);
		msg->msg_iov = &iov;
		msg->msg_iovlen = 1;
		msg->msg_name = (struct sockaddr *)&req->rq_raddr;
		msg->msg_namelen = req->rq_raddr_len;

		/* Set source IP address of the reply message in PKTINFO */
		if (req->rq_daddr_len != 0) {
			msg->msg_control = su->su_cmsg;
			cmsg = (struct cmsghdr *)msg->msg_control;
			svc_dg_set_pktinfo(cmsg, req);
			msg->msg_controllen = CMSG_ALIGN(cmsg->cmsg_len);
		}

		if (sendmsg(xprt->xp_fd, msg, 0) == (ssize_t) slen) {
			stat = true;
		}
	}
	rpc_dplx_rui(rec);
	return (stat);
}

static bool
svc_dg_freeargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr)
{
	return xdr_free(xdr_args, args_ptr);
}

static bool
svc_dg_getargs(struct svc_req *req, xdrproc_t xdr_args, void *args_ptr,
	       void *u_data)
{
	XDR *xdrs = REC_XPRT(req->rq_xprt)->ioq.xdrs;
	bool rslt;

	/* threads u_data for advanced decoders */
	xdrs->x_public = u_data;

	rslt = SVCAUTH_UNWRAP(req->rq_auth, req, xdrs, xdr_args, args_ptr);
	if (!rslt) {
		svc_dg_freeargs(req, xdr_args, args_ptr);
	}

	return (rslt);
}

static void
svc_dg_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct svc_dg_xprt *su = su_data(xprt);

	/* clears xprt from the xprt table (eg, idle scans) */
	svc_rqst_xprt_unregister(xprt);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refs %" PRIu32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_refs, tag, line);

	if ((xprt->xp_flags & SVC_XPRT_FLAG_CLOSE) && xprt->xp_fd != -1)
		(void)close(xprt->xp_fd);

	XDR_DESTROY(REC_XPRT(xprt)->ioq.xdrs);
	mem_free(rpc_buffer(xprt), su->su_iosz);

	if (xprt->xp_tp)
		mem_free(xprt->xp_tp, 0);
	if (xprt->xp_netid)
		mem_free(xprt->xp_netid, 0);

	if (xprt->xp_ops->xp_free_user_data) {
		/* call free hook */
		xprt->xp_ops->xp_free_user_data(xprt);
	}
	svc_dg_xprt_free(su);
}

extern mutex_t ops_lock;

 /*ARGSUSED*/
static bool
svc_dg_control(SVCXPRT *xprt, const u_int rq, void *in)
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
		return (false);
	}
	return (true);
}

static void
svc_dg_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */
	mutex_lock(&ops_lock);

	/* Fill in type of service */
	xprt->xp_type = XPRT_UDP;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_dg_recv;
		ops.xp_stat = svc_dg_stat;
		ops.xp_getargs = svc_dg_getargs;
		ops.xp_reply = svc_dg_reply;
		ops.xp_freeargs = svc_dg_freeargs;
		ops.xp_destroy = svc_dg_destroy;
		ops.xp_control = svc_dg_control;
		ops.xp_getreq = svc_getreq_default;
		ops.xp_dispatch = svc_dispatch_default;
		ops.xp_recv_user_data = NULL;	/* no default */
		ops.xp_free_user_data = NULL;	/* no default */
	}
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

/*
 * Enable reception of PKTINFO control messages
 */
void
svc_dg_enable_pktinfo(int fd, const struct __rpc_sockinfo *si)
{
	int val = 1;

	switch (si->si_af) {
	case AF_INET:
#ifdef SOL_IP
		(void)setsockopt(fd, SOL_IP, IP_PKTINFO, &val, sizeof(val));
#endif
		break;

	case AF_INET6:
#ifdef SOL_IPV6
		(void)setsockopt(fd, SOL_IP, IP_PKTINFO, &val, sizeof(val));
		(void)setsockopt(fd, SOL_IPV6, IPV6_RECVPKTINFO,
				&val, sizeof(val));
#endif
		break;
	}
}

static int
svc_dg_store_in_pktinfo(struct cmsghdr *cmsg, struct svc_req *req)
{
	if (cmsg->cmsg_level == SOL_IP &&
	    cmsg->cmsg_type == IP_PKTINFO &&
	    cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
		struct in_pktinfo *pkti;
		struct sockaddr_in *daddr;

		pkti = (struct in_pktinfo *)CMSG_DATA(cmsg);
		daddr = (struct sockaddr_in *)&req->rq_daddr;
		daddr->sin_family = AF_INET;
#ifdef __FreeBSD__
		daddr->sin_addr = pkti->ipi_addr;
#else
		daddr->sin_addr.s_addr = pkti->ipi_spec_dst.s_addr;
#endif
		req->rq_daddr_len = sizeof(struct sockaddr_in);
		return 1;
	} else {
		return 0;
	}
}

static int
svc_dg_store_in6_pktinfo(struct cmsghdr *cmsg, struct svc_req *req)
{
	if (cmsg->cmsg_level == SOL_IPV6 &&
	    cmsg->cmsg_type == IPV6_PKTINFO &&
	    cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
		struct in6_pktinfo *pkti;
		struct sockaddr_in6 *daddr;

		pkti = (struct in6_pktinfo *)CMSG_DATA(cmsg);
		daddr = (struct sockaddr_in6 *) &req->rq_daddr;
		daddr->sin6_family = AF_INET6;
		daddr->sin6_addr = pkti->ipi6_addr;
		daddr->sin6_scope_id = pkti->ipi6_ifindex;
		req->rq_daddr_len = sizeof(struct sockaddr_in6);
		return 1;
	} else {
		return 0;
	}
}

/*
 * When given a control message received from the socket
 * layer, check whether it contains valid PKTINFO data.
 * If so, store the data in the request.
 */
static int
svc_dg_store_pktinfo(struct msghdr *msg, struct svc_req *req)
{
	struct cmsghdr *cmsg;

	if (!msg->msg_name)
		return 0;

	if (msg->msg_flags & MSG_CTRUNC)
		return 0;

	cmsg = CMSG_FIRSTHDR(msg);
	if (cmsg == NULL || CMSG_NXTHDR(msg, cmsg) != NULL)
		return 0;

	switch (((struct sockaddr *)msg->msg_name)->sa_family) {
	case AF_INET:
#ifdef SOL_IP
		if (svc_dg_store_in_pktinfo(cmsg, req))
				return 1;
#endif
		break;

	case AF_INET6:
#ifdef SOL_IPV6
		/* Handle IPv4 PKTINFO as well on IPV6 interface */
		if (svc_dg_store_in_pktinfo(cmsg, req))
			return 1;

		if (svc_dg_store_in6_pktinfo(cmsg, req))
			return 1;
#endif
		break;

	default:
		break;
	}

	return 0;
}
