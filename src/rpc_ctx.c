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

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <rpc/types.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>

#include "rpc_com.h"
#include "clnt_internal.h"

#include <misc/rbtree_x.h>
#include "vc_lock.h"
#include "rpc_ctx.h"

rpc_ctx_t *
alloc_rpc_call_ctx(CLIENT *cl, rpcproc_t proc, xdrproc_t xdr_args,
                       void *args_ptr, xdrproc_t xdr_results, void *results_ptr,
                       struct timeval timeout)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    struct vc_fd_rec *crec = cx->cx_crec;
    rpc_ctx_t *ctx = mem_alloc(sizeof(rpc_ctx_t));
    if (! ctx)
        goto out;

    assert(crec);

    /* XXX we hold the client-fd lock */
    ctx->xid = ++(crec->calls.xid);

    /* some of this looks like overkill;  it's here to support future,
     * fully async calls */
    ctx->ctx_u.clnt.cl = cl;
    ctx->ctx_u.clnt.proc = proc;
    ctx->ctx_u.clnt.xdr_args = xdr_args;
    ctx->ctx_u.clnt.args_ptr = args_ptr;
    ctx->ctx_u.clnt.results_ptr = results_ptr;
    ctx->msg = alloc_rpc_msg();
    ctx->u_data[0] = NULL;
    ctx->u_data[1] = NULL;
    ctx->state = RPC_CTX_START;
    ctx->flags = 0;

    /* stash it */
    if (opr_rbtree_insert(&crec->calls.t, &ctx->node_k)) {
        __warnx("%s: call ctx insert failed (xid %d client %p)",
                __func__,
                ctx->xid, cl);
        mem_free(ctx, sizeof(rpc_ctx_t));
        ctx = NULL;
    }

out:
    return (ctx);
}

void rpc_ctx_next_xid(rpc_ctx_t *ctx, uint32_t flags)
{
    struct cx_data *cx = (struct cx_data *) ctx->ctx_u.clnt.cl->cl_private;
    struct vc_fd_rec *crec = cx->cx_crec;

    assert (flags & RPC_CTX_FLAG_LOCKED);

    opr_rbtree_remove(&crec->calls.t, &ctx->node_k);
    ctx->xid = ++(crec->calls.xid);
    if (opr_rbtree_insert(&crec->calls.t, &ctx->node_k)) {
        __warnx("%s: call ctx insert failed (xid %d client %p)",
                __func__,
                ctx->xid,
                ctx->ctx_u.clnt.cl);
    }
}

#if 0
enum clnt_stat
rpc_ctx_wait_reply(rpc_ctx_t *ctx, uint32_t flags)
{
    struct cx_data *cx = (struct cx_data *) ctx->ctx_u.clnt.cl->cl_private;
    struct ct_data *ct = CT_DATA(cx);
    struct vc_fd_rec *crec __attribute__((unused)) = cx->cx_crec;
    XDR *xdrs __attribute__((unused)) = &(ct->ct_xdrs);
    enum clnt_stat stat = RPC_SUCCESS;

    assert (flags & RPC_CTX_FLAG_LOCKED);

    ctx->state = RPC_CTX_REPLY_WAIT;

        /* switch on direction */
    switch (msg->rm_direction) {
    case REPLY:
        if (msg->rm_xid == ctx->xid)
            goto replied;
        break;
    case CALL:
        /* XXX queue or dispatch.  on return from xp_dispatch,
         * duplex_msg points to a (potentially new, junk) rpc_msg
         * object owned by this call path */
        if (duplex) {
            struct cf_conn *cd;
            assert(duplex_xprt);
            cd = (struct cf_conn *) duplex_xprt->xp_p1;
            cd->x_id = msg->rm_xid;
            __warnx("%s: call intercepted, dispatching (x_id == %d)\n",
                    __func__, cd->x_id);
            duplex_xprt->xp_ops2->xp_dispatch(duplex_xprt, &msg);
        }
        break;
    default:
        break;
    }

    return (stat);
}
#endif

void
free_rpc_call_ctx(rpc_ctx_t *ctx, uint32_t flags)
{
    struct cx_data *cx = (struct cx_data *) ctx->ctx_u.clnt.cl->cl_private;
    struct vc_fd_rec *crec = cx->cx_crec;

    assert (flags & RPC_CTX_FLAG_LOCKED);

    opr_rbtree_remove(&crec->calls.t, &ctx->node_k);
    if (ctx->msg)
        free_rpc_msg(ctx->msg);
    mem_free(ctx, sizeof(rpc_ctx_t));
    
}
