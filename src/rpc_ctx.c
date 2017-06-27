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
			"%s: call ctx insert failed (xid %d client %p)",
			__func__, ctx->xid, clnt);
		rpc_ctx_release(ctx);
		rpc_dplx_rui(rec);
		return (NULL);
	}
	return (ctx);
}

/* RPC_CTX_FLAG_LOCKED, RPC_DPLX_FLAG_LOCKED
 */
bool
rpc_ctx_next_xid(rpc_ctx_t *ctx)
{
	CLIENT *clnt = ctx->ctx_u.clnt.clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;

	/* the lock protects both xid and rbtree */
	opr_rbtree_remove(&rec->call_replies, &ctx->node_k);
	ctx->xid = ++(rec->call_xid);
	ctx->flags = RPC_CTX_FLAG_NONE;

	if (opr_rbtree_insert(&rec->call_replies, &ctx->node_k)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: call ctx insert failed (xid %d client %p)",
			__func__, ctx->xid, clnt);
		rpc_ctx_release(ctx);
		rpc_dplx_rui(rec);
		return (false);
	}
	return (true);
}

/* RPC_DPLX_FLAG_LOCKED
 */
bool
rpc_ctx_xfer_replymsg(struct svc_vc_xprt *xd, struct rpc_msg *msg)
{
	rpc_ctx_t ctx_k, *ctx;
	struct opr_rbtree_node *nv;
	struct rpc_dplx_rec *rec = &xd->sx_dr;
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	ctx_k.xid = msg->rm_xid;
	nv = opr_rbtree_lookup(&rec->call_replies, &ctx_k.node_k);
	if (nv) {
		ctx = opr_containerof(nv, rpc_ctx_t, node_k);
		atomic_set_uint16_t_bits(&ctx->flags, RPC_CTX_FLAG_ACKSYNC);
		atomic_inc_uint32_t(&ctx->refcount);
		ctx->cc_msg = *msg;	/* and stash reply header */

		/* signal the specific ctx  */
		cond_signal(&ctx->we.cv);

		/* now, we must ourselves wait for the other side to run */
		while (atomic_fetch_uint16_t(&ctx->flags)
						& RPC_CTX_FLAG_ACKSYNC)
			cond_wait(&lk->we.cv, &lk->we.mtx);

		rpc_ctx_release(ctx);
		return (true);
	}
	return (false);
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
		"%s: call ctx %p (xid %" PRIu32 " client %p)",
		__func__, ctx, ctx->xid, clnt);

	(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
	timespecadd(&ts, &ctx->ctx_u.clnt.timeout);
	code = cond_timedwait(&ctx->we.cv, &ctx->we.mtx, &ts);

	__warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
		"%s: call ctx %p replied (xid %" PRIu32 " client %p)",
		__func__, ctx, ctx->xid, clnt);

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
		return (code);
	}

	/* switch on direction */
	switch (ctx->cc_msg.rm_direction) {
	case REPLY:
		if (ctx->cc_msg.rm_xid == ctx->xid)
			return (RPC_SUCCESS);
		break;
	case CALL:
		/* XXX cond transfer control to svc */
		/* */
		break;
	default:
		break;
	}

	return (code);
}

/* RPC_CTX_FLAG_LOCKED, RPC_DPLX_FLAG_LOCKED
 */
void
rpc_ctx_ack_xfer(rpc_ctx_t *ctx)
{
	CLIENT *clnt = ctx->ctx_u.clnt.clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	uint16_t flags = atomic_postclear_uint16_t_bits(&ctx->flags,
							RPC_CTX_FLAG_ACKSYNC);

	if (flags & RPC_CTX_FLAG_ACKSYNC)
		rpc_dplx_rsi(rec);
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
