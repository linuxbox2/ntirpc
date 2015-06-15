/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <rpc/types.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc_rqst.h>
#include "rpc_com.h"
#include <misc/rbtree_x.h>
#include "clnt_internal.h"
#include "svc_internal.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"

static inline int
clnt_read_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctp;
	struct ct_data *ct = &xd->cx.data;
	rpc_ctx_t *ctx = (rpc_ctx_t *) xdrs->x_lib[1];
	struct pollfd fd;
	int milliseconds =
	    (int)((ct->ct_wait.tv_sec * 1000) + (ct->ct_wait.tv_usec / 1000));

	if (len == 0)
		return (0);

	fd.fd = xd->cx.data.ct_fd;
	fd.events = POLLIN;
	for (;;) {
		switch (poll(&fd, 1, milliseconds)) {
		case 0:
			ctx->error.re_status = RPC_TIMEDOUT;
			return (-1);

		case -1:
			if (errno == EINTR)
				continue;
			ctx->error.re_status = RPC_CANTRECV;
			ctx->error.re_errno = errno;
			return (-1);
		}
		break;
	}

	len = read(xd->cx.data.ct_fd, buf, (size_t) len);

	switch (len) {
	case 0:
		/* premature eof */
		ctx->error.re_errno = ECONNRESET;
		ctx->error.re_status = RPC_CANTRECV;
		len = -1;	/* it's really an error */
		break;

	case -1:
		ctx->error.re_errno = errno;
		ctx->error.re_status = RPC_CANTRECV;
		break;
	}
	return (len);
}

static inline int
clnt_write_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctp;
	rpc_ctx_t *ctx = (rpc_ctx_t *) xdrs->x_lib[1];

	int i = 0, cnt;

	for (cnt = len; cnt > 0; cnt -= i, buf += i) {
		i = write(xd->cx.data.ct_fd, buf, (size_t) cnt);
		if (i == -1) {
			ctx->error.re_errno = errno;
			ctx->error.re_status = RPC_CANTSEND;
			return (-1);
		}
	}
	return (len);
}

static inline void
cfconn_set_dead(SVCXPRT *xprt, struct x_vc_data *xd)
{
	mutex_lock(&xprt->xp_lock);
	xd->sx.strm_stat = XPRT_DIED;
	mutex_unlock(&xprt->xp_lock);
}

/*
 * reads data from the tcp or udp connection.
 * any error is fatal and the connection is closed.
 * (And a read of zero bytes is a half closed stream => error.)
 * All read operations timeout after 35 seconds.  A timeout is
 * fatal for the connection.
 */
#define EARLY_DEATH_DEBUG 1

static inline int
svc_read_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	SVCXPRT *xprt;
	int milliseconds = 35 * 1000;	/* XXX configurable? */
	struct pollfd pollfd;
	struct x_vc_data *xd;

	xd = (struct x_vc_data *)ctp;
	xprt = xd->rec->hdl.xprt;

	if (xd->shared.nonblock) {
		len = read(xprt->xp_fd, buf, (size_t) len);
		if (len < 0) {
			if (errno == EAGAIN)
				len = 0;
			else
				goto fatal_err;
		}
		if (len != 0)
			(void)clock_gettime(CLOCK_MONOTONIC_FAST,
					    &xd->sx.last_recv);
		return len;
	}

	do {
		pollfd.fd = xprt->xp_fd;
		pollfd.events = POLLIN;
		pollfd.revents = 0;
		switch (poll(&pollfd, 1, milliseconds)) {
		case -1:
			if (errno == EINTR)
				continue;
		 /*FALLTHROUGH*/ case 0:
			__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
				"%s: poll returns 0 (will set dead)", __func__);
			goto fatal_err;

		default:
			break;
		}
	} while ((pollfd.revents & POLLIN) == 0);

	len = read(xprt->xp_fd, buf, (size_t) len);
	if (len > 0) {
		(void) clock_gettime(CLOCK_MONOTONIC_FAST, &xd->sx.last_recv);
		return (len);
	}

 fatal_err:
	cfconn_set_dead(xprt, xd);
	return (-1);
}

/*
 * writes data to the tcp connection.
 * Any error is fatal and the connection is closed.
 */
static inline int
svc_write_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	SVCXPRT *xprt;
	struct x_vc_data *xd;
	struct timespec ts0, ts1;
	int i, cnt;

	xd = (struct x_vc_data *)ctp;
	xprt = xd->rec->hdl.xprt;

	if (xd->shared.nonblock)
		(void)clock_gettime(CLOCK_MONOTONIC_FAST, &ts0);

	for (cnt = len; cnt > 0; cnt -= i, buf += i) {
		i = write(xprt->xp_fd, buf, (size_t) cnt);
		if (i < 0) {
			if (errno != EAGAIN || !xd->shared.nonblock) {
				__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
					"%s: short write !EAGAIN (will set dead)",
					__func__);
				cfconn_set_dead(xprt, xd);
				return (-1);
			}
			if (xd->shared.nonblock && i != cnt) {
				/*
				 * For non-blocking connections, do not
				 * take more than 2 seconds writing the
				 * data out.
				 *
				 * XXX 2 is an arbitrary amount.
				 */
				(void)clock_gettime(CLOCK_MONOTONIC_FAST, &ts1);
				if (ts1.tv_sec - ts0.tv_sec >= 2) {
					__warnx(TIRPC_DEBUG_FLAG_SVC_VC,
						"%s: short write !EAGAIN (will set dead)",
						__func__);
					cfconn_set_dead(xprt, xd);
					return (-1);
				}
			}
		}
	}

	return (len);
}

/* generic read and write callbacks */
int
generic_read_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	switch ((enum rpc_duplex_callpath)xdrs->x_lib[0]) {
	case RPC_DPLX_CLNT:
		return (clnt_read_vc(xdrs, ctp, buf, len));
		break;
	case RPC_DPLX_SVC:
		return (svc_read_vc(xdrs, ctp, buf, len));
		break;
	default:
		/* better not */
		abort();
	}
}

int
generic_write_vc(XDR *xdrs, void *ctp, void *buf, int len)
{
	switch ((enum rpc_duplex_callpath)xdrs->x_lib[0]) {
	case RPC_DPLX_CLNT:
		return (clnt_write_vc(xdrs, ctp, buf, len));
		break;
	case RPC_DPLX_SVC:
		return (svc_write_vc(xdrs, ctp, buf, len));
		break;
	default:
		/* better not */
		abort();
	}
}

void
vc_shared_destroy(struct x_vc_data *xd)
{
	struct rpc_dplx_rec *rec = xd->rec;
	struct ct_data *ct = &xd->cx.data;
	SVCXPRT *xprt;
	bool closed = false;
	bool xdrs_destroyed = false;

	/* RECLOCKED */

	if (ct->ct_closeit && ct->ct_fd != RPC_ANYFD) {
		(void)close(ct->ct_fd);
		closed = true;
	}

	/* destroy shared XDR record streams (once) */
	XDR_DESTROY(&xd->shared.xdrs_in);
	XDR_DESTROY(&xd->shared.xdrs_out);
	xdrs_destroyed = true;

	if (ct->ct_addr.buf)
		mem_free(ct->ct_addr.buf, 0);	/* XXX */

	/* svc_vc */
	xprt = rec->hdl.xprt;
	if (xprt) {

		XPRT_TRACE_RADDR(xprt, __func__, __func__, __LINE__);

		rec->hdl.xprt = NULL;	/* unreachable */

		if (!closed) {
			if (xprt->xp_fd != RPC_ANYFD)
				(void)close(xprt->xp_fd);
		}

		/* request socket */
		if (!xdrs_destroyed) {
			XDR_DESTROY(&(xd->shared.xdrs_in));
			XDR_DESTROY(&(xd->shared.xdrs_out));
		}

		if (xprt->xp_rtaddr.buf)
			mem_free(xprt->xp_rtaddr.buf, xprt->xp_rtaddr.maxlen);
		if (xprt->xp_ltaddr.buf)
			mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);
		if (xprt->xp_tp)
			mem_free(xprt->xp_tp, 0);
		if (xprt->xp_netid)
			mem_free(xprt->xp_netid, 0);

		if (xprt->xp_ops->xp_free_user_data) {
			/* call free hook */
			xprt->xp_ops->xp_free_user_data(xprt);
		}

		mem_free(xprt, sizeof(SVCXPRT));
	}

	rec->hdl.xd = NULL;

	/* unref shared */
	REC_UNLOCK(rec);

	if (xprt)
		rpc_dplx_unref(rec, RPC_DPLX_FLAG_NONE);

	/* free xd itself */
	mem_free(xd, sizeof(struct x_vc_data));
}
