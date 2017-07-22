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
#include <stdint.h>
#include <assert.h>
#if !defined(_WIN32)
#include <err.h>
#endif
#include <errno.h>
#include <rpc/types.h>
#include <reentrant.h>
#include <misc/portable.h>
#include <signal.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include "rpc_com.h"
#include <misc/rbtree_x.h>
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"

#define tv_to_ms(tv) (1000 * ((tv)->tv_sec) + (tv)->tv_usec/1000)

int
call_xid_cmpf(const struct opr_rbtree_node *lhs,
	      const struct opr_rbtree_node *rhs)
{
	rpc_ctx_t *lk, *rk;

	lk = opr_containerof(lhs, rpc_ctx_t, node_k);
	rk = opr_containerof(rhs, rpc_ctx_t, node_k);

	if (lk->xid < rk->xid)
		return (-1);

	if (lk->xid == rk->xid)
		return (0);

	return (1);
}

/*
 * On success, returns with RPC_CTX_FLAG_LOCKED followed by RPC_DPLX_FLAG_LOCKED
 */
rpc_ctx_t *
rpc_ctx_alloc(CLIENT *clnt, struct timeval timeout)
{
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	rpc_ctx_t *ctx = mem_alloc(sizeof(rpc_ctx_t));

	rpc_msg_init(&ctx->cc_msg);

	/* protects this */
	mutex_init(&ctx->we.mtx, NULL);
	mutex_lock(&ctx->we.mtx);
	cond_init(&ctx->we.cv, 0, NULL);
	ctx->flags = RPC_CTX_FLAG_NONE;
	ctx->refcount = 1;
	ctx->refreshes = 2;

	/* some of this looks like overkill;  it's here to support future,
	 * fully async calls */
	ctx->ctx_u.clnt.clnt = clnt;
	ctx->ctx_u.clnt.timeout.tv_sec = 0;
	ctx->ctx_u.clnt.timeout.tv_nsec = 0;
	timespec_addms(&ctx->ctx_u.clnt.timeout, tv_to_ms(&timeout));

	/* this lock protects both xid and rbtree */
	rpc_dplx_rli(rec);
	ctx->xid = ++(rec->call_xid);

	if (opr_rbtree_insert(&rec->call_replies, &ctx->node_k)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d call ctx insert failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd, ctx->xid);
		rpc_ctx_release(ctx);
		rpc_dplx_rui(rec);
		return (NULL);
	}
	return (ctx);
}

/* unlocked
 */
enum xprt_stat
rpc_ctx_xfer_replymsg(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	SVCXPRT *xprt = req->rq_xprt;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	rpc_ctx_t ctx_k, *ctx;
	struct opr_rbtree_node *nv;

	rpc_dplx_rli(rec);
	ctx_k.xid = req->rq_msg.rm_xid;
	nv = opr_rbtree_lookup(&rec->call_replies, &ctx_k.node_k);
	if (!nv) {
		/* release internal locks */
		rpc_dplx_rui(rec);
		return SVC_STAT(xprt);
	}

	ctx = opr_containerof(nv, rpc_ctx_t, node_k);

	_seterr_reply(&req->rq_msg, &(ctx->error));
	if (ctx->error.re_status == RPC_SUCCESS) {
		if (!AUTH_VALIDATE(ctx->cc_auth,
				   &(ctx->cc_msg.RPCM_ack.ar_verf))) {
			ctx->error.re_status = RPC_AUTHERROR;
			ctx->error.re_why = AUTH_INVALIDRESP;
		} else if (ctx->cc_xdr.proc
			   && !AUTH_UNWRAP(ctx->cc_auth, xdrs,
					   ctx->cc_xdr.proc,
					   ctx->cc_xdr.where)) {
			if (ctx->error.re_status == RPC_SUCCESS)
				ctx->error.re_status = RPC_CANTDECODERES;
		}
		ctx->refreshes = 0;
	} else if (ctx->refreshes-- > 0
		   && AUTH_REFRESH(ctx->cc_auth, &(ctx->cc_msg))) {
		/* maybe our credentials need to be refreshed ... */
		opr_rbtree_remove(&rec->call_replies, &ctx->node_k);
		ctx->xid = ++(rec->call_xid);

		if (opr_rbtree_insert(&rec->call_replies, &ctx->node_k)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d call ctx insert failed xid %" PRIu32,
				__func__, xprt, xprt->xp_fd, ctx->xid);
			ctx->error.re_status = RPC_TLIERROR;
			ctx->refreshes = 0;
		}
	}
	atomic_set_uint16_t_bits(&ctx->flags, RPC_CTX_FLAG_ACKSYNC);

	/* signal the specific ctx  */
	cond_signal(&ctx->we.cv);

	__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
		"%s: %p fd %d call ctx acknowledged xid %" PRIu32,
		__func__, xprt, xprt->xp_fd, ctx->xid);

	/* release internal locks */
	rpc_dplx_rui(rec);
	return SVC_STAT(xprt);
}

/* RPC_CTX_FLAG_LOCKED, RPC_DPLX_FLAG_LOCKED
 */
int
rpc_ctx_wait_reply(rpc_ctx_t *ctx)
{
	CLIENT *clnt = ctx->ctx_u.clnt.clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct timespec ts;
	int code;

	/* no loop, signaled directly */
	rpc_dplx_rui(rec);

	__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
		"%s: %p fd %d call ctx xid %" PRIu32,
		__func__, &rec->xprt, rec->xprt.xp_fd, ctx->xid);

	(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
	timespecadd(&ts, &ctx->ctx_u.clnt.timeout);
	code = cond_timedwait(&ctx->we.cv, &ctx->we.mtx, &ts);

	__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
		"%s: %p fd %d call ctx replied xid %" PRIu32,
		__func__, &rec->xprt, rec->xprt.xp_fd, ctx->xid);

	rpc_dplx_rli(rec);

	/* it is possible for rpc_ctx_xfer_replymsg() to complete
	 * in the window between cond_timedwait() and rpc_dplx_rli()
	 */
	if (!(atomic_fetch_uint16_t(&ctx->flags) & RPC_CTX_FLAG_ACKSYNC)
	 && (code == ETIMEDOUT)) {
		if (rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
			/* XXX should also set error.re_why, but the
			 * facility is not well developed. */
			ctx->error.re_status = RPC_TIMEDOUT;
		}
	}

	return (code);
}

/* RPC_CTX_FLAG_LOCKED, RPC_DPLX_FLAG_LOCKED
 */
void
rpc_ctx_release(rpc_ctx_t *ctx)
{
	CLIENT *clnt = ctx->ctx_u.clnt.clnt;
	struct cx_data *cx = CX_DATA(clnt);

	if (atomic_dec_uint32_t(&ctx->refcount))
		return;

	opr_rbtree_remove(&cx->cx_rec->call_replies, &ctx->node_k);
	mutex_unlock(&ctx->we.mtx);
	mutex_destroy(&ctx->we.mtx);
	cond_destroy(&ctx->we.cv);
	mem_free(ctx, sizeof(*ctx));
}
