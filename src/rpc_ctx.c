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

rpc_ctx_t *
alloc_rpc_call_ctx(CLIENT *clnt, rpcproc_t proc, xdrproc_t xdr_args,
		   void *args_ptr, xdrproc_t xdr_results,
		   void *results_ptr, struct timeval timeout)
{
	struct x_vc_data *xd = (struct x_vc_data *)clnt->cl_p1;
	struct rpc_dplx_rec *rec = xd->rec;
	rpc_ctx_t *ctx = mem_alloc(sizeof(rpc_ctx_t));

	/* protects this */
	mutex_init(&ctx->we.mtx, NULL);
	cond_init(&ctx->we.cv, 0, NULL);

	/* rec->calls and rbtree protected by (adaptive) mtx */
	REC_LOCK(rec);

	/* XXX we hold the client-fd lock */
	ctx->xid = ++(xd->cx.calls.xid);

	/* some of this looks like overkill;  it's here to support future,
	 * fully async calls */
	ctx->ctx_u.clnt.clnt = clnt;
	ctx->ctx_u.clnt.timeout.tv_sec = 0;
	ctx->ctx_u.clnt.timeout.tv_nsec = 0;
	timespec_addms(&ctx->ctx_u.clnt.timeout, tv_to_ms(&timeout));
	ctx->msg = alloc_rpc_msg();
	ctx->flags = 0;

	/* stash it */
	if (opr_rbtree_insert(&xd->cx.calls.t, &ctx->node_k)) {
		__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
			"%s: call ctx insert failed (xid %d client %p)",
			__func__, ctx->xid, clnt);
		REC_UNLOCK(rec);
		mutex_destroy(&ctx->we.mtx);
		cond_destroy(&ctx->we.cv);
		mem_free(ctx, sizeof(*ctx));
		ctx = NULL;
		goto out;
	}

	REC_UNLOCK(rec);

 out:
	return (ctx);
}

void
rpc_ctx_next_xid(rpc_ctx_t *ctx, uint32_t flags)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctx->ctx_u.clnt.clnt->cl_p1;
	struct rpc_dplx_rec *rec = xd->rec;

	assert(flags & RPC_CTX_FLAG_LOCKED);

	REC_LOCK(rec);
	opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
	ctx->xid = ++(xd->cx.calls.xid);
	if (opr_rbtree_insert(&xd->cx.calls.t, &ctx->node_k)) {
		REC_UNLOCK(rec);
		__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
			"%s: call ctx insert failed (xid %d client %p)",
			__func__, ctx->xid, ctx->ctx_u.clnt.clnt);
		goto out;
	}
	REC_UNLOCK(rec);
 out:
	return;
}

bool
rpc_ctx_xfer_replymsg(struct x_vc_data *xd, struct rpc_msg *msg)
{
	rpc_ctx_t ctx_k, *ctx;
	struct opr_rbtree_node *nv;
	rpc_dplx_lock_t *lk = &xd->rec->recv.lock;

	ctx_k.xid = msg->rm_xid;
	REC_LOCK(xd->rec);
	nv = opr_rbtree_lookup(&xd->cx.calls.t, &ctx_k.node_k);
	if (nv) {
		ctx = opr_containerof(nv, rpc_ctx_t, node_k);
		opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
		free_rpc_msg(ctx->msg);	/* free call header */
		ctx->msg = msg;	/* and stash reply header */
		ctx->flags |= RPC_CTX_FLAG_SYNCDONE;
		REC_UNLOCK(xd->rec);
		cond_signal(&lk->we.cv);	/* XXX we hold lk->we.mtx */
		/* now, we must ourselves wait for the other side to run */
		while (!(ctx->flags & RPC_CTX_FLAG_ACKSYNC))
			cond_wait(&lk->we.cv, &lk->we.mtx);

		/* ctx-specific signal--indicates we will make no further
		 * references to ctx whatsoever */
		mutex_lock(&ctx->we.mtx);
		ctx->flags &= ~RPC_CTX_FLAG_WAITSYNC;
		cond_signal(&ctx->we.cv);
		mutex_unlock(&ctx->we.mtx);

		return (true);
	}
	REC_UNLOCK(xd->rec);
	return (false);
}

int
rpc_ctx_wait_reply(rpc_ctx_t *ctx, uint32_t flags)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctx->ctx_u.clnt.clnt->cl_p1;
	struct rpc_dplx_rec *rec = xd->rec;
	rpc_dplx_lock_t *lk = &rec->recv.lock;
	struct timespec ts;
	int code = 0;

	/* we hold recv channel lock */
	ctx->flags |= RPC_CTX_FLAG_WAITSYNC;
	while (!(ctx->flags & RPC_CTX_FLAG_SYNCDONE)) {
		(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
		timespecadd(&ts, &ctx->ctx_u.clnt.timeout);
		code = cond_timedwait(&lk->we.cv, &lk->we.mtx, &ts);
		/* if we timed out, check for xprt destroyed (no more
		 * receives) */
		if (code == ETIMEDOUT) {
			SVCXPRT *xprt = rec->hdl.xprt;
			uint32_t xp_flags;

			/* dequeue the call */
			REC_LOCK(rec);
			opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
			REC_UNLOCK(rec);

			mutex_lock(&xprt->xp_lock);
			xp_flags = xprt->xp_flags;
			mutex_unlock(&xprt->xp_lock);

			if (xp_flags & SVC_XPRT_FLAG_DESTROYED) {
				/* XXX should also set error.re_why, but the
				 * facility is not well developed. */
				ctx->error.re_status = RPC_TIMEDOUT;
			}
			ctx->flags &= ~RPC_CTX_FLAG_WAITSYNC;
			goto out;
		}
	}
	ctx->flags &= ~RPC_CTX_FLAG_SYNCDONE;

	/* switch on direction */
	switch (ctx->msg->rm_direction) {
	case REPLY:
		if (ctx->msg->rm_xid == ctx->xid)
			return (RPC_SUCCESS);
		break;
	case CALL:
		/* XXX cond transfer control to svc */
		/* */
		break;
	default:
		break;
	}

 out:
	return (code);
}

void
rpc_ctx_ack_xfer(rpc_ctx_t *ctx)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctx->ctx_u.clnt.clnt->cl_p1;
	rpc_dplx_lock_t *lk = &xd->rec->recv.lock;

	ctx->flags |= RPC_CTX_FLAG_ACKSYNC;
	cond_signal(&lk->we.cv);	/* XXX we hold lk->we.mtx */
}

void
free_rpc_call_ctx(rpc_ctx_t *ctx, uint32_t flags)
{
	struct x_vc_data *xd = (struct x_vc_data *)ctx->ctx_u.clnt.clnt->cl_p1;
	struct rpc_dplx_rec *rec = xd->rec;
	struct timespec ts;

	/* wait for commit of any xfer (ctx specific) */
	mutex_lock(&ctx->we.mtx);
	if (ctx->flags & RPC_CTX_FLAG_WAITSYNC) {
		/* WAITSYNC is already cleared if the call timed out, but it is
		 * incorrect to wait forever */
		(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
		timespecadd(&ts, &ctx->ctx_u.clnt.timeout);
		(void)cond_timedwait(&ctx->we.cv, &ctx->we.mtx, &ts);
	}

	REC_LOCK(rec);
	opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
	/* interlock */
	mutex_unlock(&ctx->we.mtx);
	REC_UNLOCK(rec);

	if (ctx->msg)
		free_rpc_msg(ctx->msg);
	mutex_destroy(&ctx->we.mtx);
	cond_destroy(&ctx->we.cv);
	mem_free(ctx, sizeof(*ctx));
}
