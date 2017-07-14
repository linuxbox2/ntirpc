
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

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

static void svc_dg_rendezvous_ops(SVCXPRT *);
static void svc_dg_override_ops(SVCXPRT *, SVCXPRT *);

static void svc_dg_enable_pktinfo(int, const struct __rpc_sockinfo *);
static int svc_dg_store_pktinfo(struct msghdr *, SVCXPRT *);

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
	mem_free(su, sizeof(struct svc_dg_xprt) + su->su_dr.maxrec);
}

static struct svc_dg_xprt *
svc_dg_xprt_zalloc(size_t iosz)
{
	struct svc_dg_xprt *su = mem_zalloc(sizeof(struct svc_dg_xprt) + iosz);

	/* Init SVCXPRT locks, etc */
	mutex_init(&su->su_dr.xprt.xp_lock, NULL);
	mutex_init(&su->su_dr.xprt.xp_auth_lock, NULL);
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
		struct svc_dg_xprt *su = svc_dg_xprt_zalloc(0);

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
	su->su_dr.sendsz = ((sendsize + 3) / 4) * 4;
	su->su_dr.recvsz = ((recvsize + 3) / 4) * 4;
	su->su_dr.maxrec = ((MAX(sendsize, recvsize) + 3) / 4) * 4;
	svc_dg_rendezvous_ops(xprt);

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
	return SVC_STAT(xprt->xp_parent);
}

static enum xprt_stat
svc_dg_rendezvous(SVCXPRT *xprt)
{
	struct svc_dg_xprt *req_su = DG_DR(REC_XPRT(xprt));
	struct svc_dg_xprt *su = svc_dg_xprt_zalloc(req_su->su_dr.maxrec);
	SVCXPRT *newxprt = &su->su_dr.xprt;
	struct sockaddr *sp = (struct sockaddr *)&newxprt->xp_remote.ss;
	struct msghdr *mesgp;
	struct iovec iov;
	ssize_t rlen;

	if (svc_rqst_rearm_events(xprt)){
		svc_dg_xprt_free(su);
		return (XPRT_DIED);
	}
	newxprt->xp_fd = xprt->xp_fd;
	newxprt->xp_flags = SVC_XPRT_FLAG_INITIAL | SVC_XPRT_FLAG_INITIALIZED;

	su->su_dr.sendsz = req_su->su_dr.sendsz;
	su->su_dr.recvsz = req_su->su_dr.recvsz;
	su->su_dr.maxrec = req_su->su_dr.maxrec;
	svc_dg_override_ops(newxprt, xprt);

 again:
	iov.iov_base = &su[1];
	iov.iov_len = su->su_dr.maxrec;
	mesgp = &su->su_msghdr;
	memset(mesgp, 0, sizeof(*mesgp));
	mesgp->msg_iov = &iov;
	mesgp->msg_iovlen = 1;
	mesgp->msg_name = sp;
	sp->sa_family = (sa_family_t) 0xffff;
	mesgp->msg_namelen = sizeof(struct sockaddr_storage);
	mesgp->msg_control = su->su_cmsg;
	mesgp->msg_controllen = sizeof(su->su_cmsg);

	rlen = recvmsg(newxprt->xp_fd, mesgp, 0);

	if (sp->sa_family == (sa_family_t) 0xffff) {
		svc_dg_xprt_free(su);
		return (XPRT_DIED);
	}

	if (rlen == -1 && errno == EINTR)
		goto again;
	if (rlen == -1 || (rlen < (ssize_t) (4 * sizeof(u_int32_t)))) {
		svc_dg_xprt_free(su);
		return (XPRT_DIED);
	}

	__rpc_address_setup(&newxprt->xp_local);
	__rpc_address_setup(&newxprt->xp_remote);
	newxprt->xp_remote.nb.len = mesgp->msg_namelen;

	/* Check whether there's an IP_PKTINFO or IP6_PKTINFO control message.
	 * If yes, preserve it for svc_dg_reply; otherwise just zap any cmsgs */
	if (!svc_dg_store_pktinfo(mesgp, newxprt)) {
		mesgp->msg_control = NULL;
		mesgp->msg_controllen = 0;
		newxprt->xp_local.nb.len = 0;
	}
	XPRT_TRACE(newxprt, __func__, __func__, __LINE__);

#if defined(HAVE_BLKIN)
	__rpc_set_blkin_endpoint(newxprt, "svc_dg");
#endif

	xdrmem_create(su->su_dr.ioq.xdrs, iov.iov_base, iov.iov_len,
		      XDR_DECODE);

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	newxprt->xp_parent = xprt;
	return (xprt->xp_dispatch.rendezvous_cb(newxprt));
}

static enum xprt_stat
svc_dg_recv(SVCXPRT *xprt)
{
	/* Stolen reference! Callee must take a reference to rq_xprt. */
	xprt->xp_refs--;

	/* pass the xdrs to user to store in struct svc_req, as most of
	 * the work has already been done on rendezvous
	 */
	return (__svc_params->request_cb(xprt, REC_XPRT(xprt)->ioq.xdrs));
}

static enum xprt_stat
svc_dg_decode(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;

	xdrs->x_op = XDR_DECODE;
	XDR_SETPOS(xdrs, 0);
	rpc_msg_init(&req->rq_msg);

	if (!xdr_dplx_decode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d failed (will set dead)",
			__func__, req->rq_xprt, req->rq_xprt->xp_fd);
		return (XPRT_DIED);
	}
	return (req->rq_xprt->xp_dispatch.process_cb(req));
}

static enum xprt_stat
svc_dg_checksum(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;

	req->rq_cksum =
#if 1
	/* CithHash64 is -substantially- faster than crc32c from FreeBSD
	 * SCTP, so prefer it until fast crc32c bests it */
		CityHash64WithSeed(xdrs->x_data,
				   MIN(256, xdr_size_inline(xdrs)),
				   103);
#else
		calculate_crc32c(0, xdrs->x_data,
				 MIN(256, xdr_size_inline(xdrs)));
#endif
	return (XPRT_IDLE);
}

static enum xprt_stat
svc_dg_reply(struct svc_req *req)
{
	SVCXPRT *xprt = req->rq_xprt;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	XDR *xdrs = rec->ioq.xdrs;
	struct svc_dg_xprt *su = DG_DR(rec);
	struct msghdr *msg = &su->su_msghdr;
	struct iovec iov;
	size_t slen;

	if (!xprt->xp_remote.nb.len) {
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s: %p fd %d has no remote address",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_IDLE);
	}
	xdrs->x_op = XDR_ENCODE;
	XDR_SETPOS(xdrs, 0);

	if (!xdr_reply_encode(xdrs, &req->rq_msg)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d xdr_reply_encode failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_DIED);
	}

	if (req->rq_msg.rm_reply.rp_stat == MSG_ACCEPTED
	 && req->rq_msg.rm_reply.rp_acpt.ar_stat == SUCCESS
	 && req->rq_auth
	 && !SVCAUTH_WRAP(req->rq_auth, req, xdrs,
			  req->rq_msg.RPCM_ack.ar_results.proc,
			  req->rq_msg.RPCM_ack.ar_results.where)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d SVCAUTH_WRAP failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_DIED);
	}
	iov.iov_base = &su[1];
	iov.iov_len = slen = XDR_GETPOS(xdrs);
	msg->msg_iov = &iov;
	msg->msg_iovlen = 1;
	msg->msg_name = (struct sockaddr *)&xprt->xp_remote.ss;
	msg->msg_namelen = xprt->xp_remote.nb.len;
	/* cmsg already set in svc_dg_rendezvous */

	if (sendmsg(xprt->xp_fd, msg, 0) != (ssize_t) slen) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d sendmsg failed (will set dead)",
			__func__, xprt, xprt->xp_fd);
		return (XPRT_DIED);
	}

	return (XPRT_IDLE);
}

static void
svc_dg_destroy_it(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	struct svc_dg_xprt *su = su_data(xprt);

	if (xprt->xp_parent) {
		SVC_RELEASE(xprt->xp_parent, SVC_RELEASE_FLAG_NONE);
	} else {
		/* only original parent is registered */
		svc_rqst_xprt_unregister(xprt);
	}

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p fd %d xp_refs %" PRIu32
		" should actually destroy things @ %s:%d",
		__func__, xprt, xprt->xp_fd, xprt->xp_refs, tag, line);

	if ((xprt->xp_flags & SVC_XPRT_FLAG_CLOSE) && xprt->xp_fd != -1)
		(void)close(xprt->xp_fd);

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

static void
svc_dg_destroy(SVCXPRT *xprt, u_int flags, const char *tag, const int line)
{
	XDR_DESTROY(REC_XPRT(xprt)->ioq.xdrs);

	svc_dg_destroy_it(xprt, flags, tag, line);
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
	case SVCGET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		*(svc_xprt_fun_t *) in = xprt->xp_ops->xp_free_user_data;
		mutex_unlock(&ops_lock);
		break;
	case SVCSET_XP_FREE_USER_DATA:
		mutex_lock(&ops_lock);
		xprt->xp_ops->xp_free_user_data = *(svc_xprt_fun_t) in;
		mutex_unlock(&ops_lock);
		break;
	default:
		return (false);
	}
	return (true);
}

static void
svc_dg_override_ops(SVCXPRT *xprt, SVCXPRT *rendezvous)
{
	static struct xp_ops ops;

	/* VARIABLES PROTECTED BY ops_lock: ops, xp_type */
	mutex_lock(&ops_lock);

	/* Fill in type of service */
	xprt->xp_type = XPRT_UDP;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_dg_recv;
		ops.xp_stat = svc_dg_stat;
		ops.xp_decode = svc_dg_decode;
		ops.xp_reply = svc_dg_reply;
		ops.xp_checksum = svc_dg_checksum;
		ops.xp_destroy = svc_dg_destroy;
		ops.xp_control = svc_dg_control;
		ops.xp_free_user_data = NULL;	/* no default */
	}
	svc_override_ops(&ops, rendezvous);
	xprt->xp_ops = &ops;
	mutex_unlock(&ops_lock);
}

static void
svc_dg_rendezvous_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;

	mutex_lock(&ops_lock);

	xprt->xp_type = XPRT_UDP_RENDEZVOUS;

	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_dg_rendezvous;
		ops.xp_stat = svc_rendezvous_stat;
		ops.xp_decode = (svc_req_fun_t)abort;
		ops.xp_reply = (svc_req_fun_t)abort;
		ops.xp_checksum = NULL;
		ops.xp_destroy = svc_dg_destroy_it;
		ops.xp_control = svc_dg_control;
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
#ifdef SOL_IP
		(void)setsockopt(fd, SOL_IP, IP_PKTINFO, &val, sizeof(val));
#endif
#ifdef SOL_IPV6
		(void)setsockopt(fd, SOL_IPV6, IPV6_RECVPKTINFO,
				&val, sizeof(val));
#endif
		break;
	}
}

static int
svc_dg_store_in_pktinfo(struct cmsghdr *cmsg, SVCXPRT *xprt)
{
	if (cmsg->cmsg_level == SOL_IP &&
	    cmsg->cmsg_type == IP_PKTINFO &&
	    cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
		struct in_pktinfo *pkti = (struct in_pktinfo *)
						CMSG_DATA(cmsg);
		struct sockaddr_in *daddr = (struct sockaddr_in *)
						&xprt->xp_local.ss;

		daddr->sin_family = AF_INET;
#ifdef __FreeBSD__
		daddr->sin_addr = pkti->ipi_addr;
#else
		daddr->sin_addr.s_addr = pkti->ipi_spec_dst.s_addr;
#endif
		xprt->xp_local.nb.len = sizeof(struct sockaddr_in);
		return 1;
	} else {
		return 0;
	}
}

static int
svc_dg_store_in6_pktinfo(struct cmsghdr *cmsg, SVCXPRT *xprt)
{
	if (cmsg->cmsg_level == SOL_IPV6 &&
	    cmsg->cmsg_type == IPV6_PKTINFO &&
	    cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
		struct in6_pktinfo *pkti = (struct in6_pktinfo *)
						CMSG_DATA(cmsg);
		struct sockaddr_in6 *daddr = (struct sockaddr_in6 *)
						&xprt->xp_local.ss;

		daddr->sin6_family = AF_INET6;
		daddr->sin6_addr = pkti->ipi6_addr;
		daddr->sin6_scope_id = pkti->ipi6_ifindex;
		xprt->xp_local.nb.len = sizeof(struct sockaddr_in6);
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
svc_dg_store_pktinfo(struct msghdr *msg, SVCXPRT *xprt)
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
		if (svc_dg_store_in_pktinfo(cmsg, xprt))
			return 1;
#endif
		break;

	case AF_INET6:
#ifdef SOL_IP
		/* Handle IPv4 PKTINFO as well on IPV6 interface */
		if (svc_dg_store_in_pktinfo(cmsg, xprt))
			return 1;
#endif
#ifdef SOL_IPV6
		if (svc_dg_store_in6_pktinfo(cmsg, xprt))
			return 1;
#endif
		break;

	default:
		break;
	}

	return 0;
}
