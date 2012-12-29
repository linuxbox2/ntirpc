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

#ifndef __APPLE__
struct cmessage {
    struct cmsghdr cmsg;
    struct cmsgcred cmcred;
};
#endif

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
clnt_vc_ncreate(int fd,      /* open file descriptor */
                const struct netbuf *raddr, /* servers address */
                const rpcprog_t prog,    /* program number */
                const rpcvers_t vers,    /* version number */
                u_int sendsz,     /* buffer recv size */
                u_int recvsz     /* buffer send size */)
{
    return (clnt_vc_ncreate2(fd, raddr, prog, vers, sendsz, recvsz,
                             CLNT_CREATE_FLAG_CONNECT));
}

CLIENT *
clnt_vc_ncreate2(int fd,       /* open file descriptor */
                 const struct netbuf *raddr, /* servers address */
                 const rpcprog_t prog,     /* program number */
                 const rpcvers_t vers,     /* version number */
                 u_int sendsz,      /* buffer recv size */
                 u_int recvsz,      /* buffer send size */
                 u_int flags)
{
    CLIENT *clnt = NULL;
    struct rpc_dplx_rec *rec = NULL;
    struct x_vc_data *xd = NULL;
    struct ct_data *ct = NULL;
    struct rpc_msg call_msg;
    sigset_t mask, newmask;
    struct __rpc_sockinfo si;
    struct sockaddr_storage ss;
    XDR ct_xdrs[1]; /* temp XDR stream */
    uint32_t oflags;
    socklen_t slen;

    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

    if (flags & CLNT_CREATE_FLAG_CONNECT) {
        slen = sizeof ss;
        if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
            if (errno != ENOTCONN) {
                rpc_createerr.cf_stat = RPC_SYSTEMERROR;
                rpc_createerr.cf_error.re_errno = errno;
                goto err;
            }
            if (connect(fd, (struct sockaddr *)raddr->buf, raddr->len) < 0){
                rpc_createerr.cf_stat = RPC_SYSTEMERROR;
                rpc_createerr.cf_error.re_errno = errno;
                goto err;
            }
        }
    } /* connect */

    if (!__rpc_fd2sockinfo(fd, &si))
        goto err;

    /* atomically find or create shared fd state */
    rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
    if (! rec) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "clnt_vc_ncreate2: rpc_dplx_lookup_rec failed");
        goto err;
    }

    /* if a clnt handle exists, return it ref'd */
    if (! (oflags & RPC_DPLX_LKP_OFLAG_ALLOC)) {
        if (rec->hdl.clnt) {
            xd = (struct x_vc_data *) rec->hdl.clnt->cl_p1;
            /* dont return destroyed clients */
            if (! (xd->flags & X_VC_DATA_FLAG_CLNT_DESTROYED)) {
                clnt = rec->hdl.clnt;
                CLNT_REF(clnt, CLNT_REF_FLAG_NONE);
            }
            /* return extra ref */
            if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
                mutex_unlock(&rec->mtx);
            goto done;
        }
    }

    clnt = (CLIENT *) mem_alloc(sizeof(CLIENT));
    if (! clnt) {
        (void) syslog(LOG_ERR, clnt_vc_errstr,
                      clnt_vc_str, __no_mem_str);
        rpc_createerr.cf_stat = RPC_SYSTEMERROR;
        rpc_createerr.cf_error.re_errno = errno;
        goto err;
    }

    mutex_init(&clnt->cl_lock, NULL);
    clnt->cl_flags = CLNT_FLAG_NONE;
    clnt->cl_refcnt = 1;

    /* other-direction shared state? */
    if (rec->hdl.xprt) {
        /* XXX check subtype of xprt handle? */
        xd = (struct x_vc_data *) rec->hdl.xprt->xp_p1;
        /* inc shared refcnt */
        ++(xd->refcnt);
    } else {
        xd = alloc_x_vc_data();
        if (! xd) {
            (void) syslog(LOG_ERR, clnt_vc_errstr,
                          clnt_vc_str, __no_mem_str);
            rpc_createerr.cf_stat = RPC_SYSTEMERROR;
            rpc_createerr.cf_error.re_errno = errno;
            goto err;
        }
        xd->rec = rec;
        /* XXX tracks outstanding calls */
        opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
        xd->cx.calls.xid = 0; /* next call xid is 1 */
        xd->refcnt = 1;
    }

    /* private data struct */
    xd->cx.data.ct_fd = fd;
    ct = &xd->cx.data;
    ct->ct_closeit = FALSE;
    ct->ct_wait.tv_usec = 0;
    ct->ct_waitset = FALSE;
    ct->ct_addr.buf = mem_alloc(raddr->maxlen);
    if (ct->ct_addr.buf == NULL)
        goto err;
    memcpy(ct->ct_addr.buf, raddr->buf, raddr->len);
    ct->ct_addr.len = raddr->len;
    ct->ct_addr.maxlen = raddr->maxlen;
    clnt->cl_netid = NULL;
    clnt->cl_tp = NULL;

    /*
     * initialize call message
     */
    call_msg.rm_xid = 1;
    call_msg.rm_direction = CALL;
    call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
    call_msg.rm_call.cb_prog = (u_int32_t)prog;
    call_msg.rm_call.cb_vers = (u_int32_t)vers;

    /*
     * pre-serialize the static part of the call msg and stash it away
     */
    xdrmem_create(ct_xdrs, ct->ct_u.ct_mcallc, MCALL_MSG_SIZE, XDR_ENCODE);
    if (! xdr_callhdr(ct_xdrs, &call_msg)) {
        if (ct->ct_closeit) {
            (void)close(fd);
        }
        goto err;
    }
    ct->ct_mpos = XDR_GETPOS(ct_xdrs);
    XDR_DESTROY(ct_xdrs);

    /*
     * Create a client handle which uses xdrrec for serialization
     * and authnone for authentication.
     */
    clnt->cl_ops = clnt_vc_ops();
    clnt->cl_p1 = xd;
    clnt->cl_p2 = rec;

    if (! rec->hdl.xprt) {

        xd->shared.sendsz =
            __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
        xd->shared.recvsz =
            __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);

#if XDR_VREC
    /* duplex streams, plus buffer sharing, readv/writev */
    xdr_vrec_create(&(cd->xdrs_in),
                    XDR_VREC_IN, xprt, readv_vc, NULL, xd->shared.recvsz,
                    VREC_FLAG_NONE);

    xdr_vrec_create(&(cd->xdrs_out),
                    XDR_VREC_OUT, xprt, NULL, writev_vc, xd->shared.sendsz,
                    VREC_FLAG_NONE);
#else
    /* duplex streams */
    xdrrec_create(&(xd->shared.xdrs_in), sendsz, xd->shared.recvsz, xd,
                  generic_read_vc,
                  generic_write_vc);
    xd->shared.xdrs_in.x_op = XDR_DECODE;

    xdrrec_create(&(xd->shared.xdrs_out), sendsz, xd->shared.recvsz, xd,
                  generic_read_vc,
                  generic_write_vc);
    xd->shared.xdrs_out.x_op = XDR_ENCODE;
#endif
    } /* XPRT */

    /* make reachable from rec */
    rec->hdl.clnt = clnt;

    /* release rec */
    mutex_unlock(&rec->mtx);

done:
    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
    return (clnt);

err:
    if (clnt) {
        /* XXX fix */
        if (xd) {
            if (ct->ct_addr.len)
                mem_free(ct->ct_addr.buf,
                         ct->ct_addr.len);
            free_x_vc_data(xd);
        }
        mem_free(clnt, sizeof (CLIENT));
    }
    if (rec) {
        if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
            mutex_unlock(&rec->mtx);
    }
    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
    return (NULL);
}

#define vc_call_return_slocked(r) \
    do { \
	result=(r);							\
	rpc_dplx_suc(clnt);						\
        goto out; \
    } while (0);

#define vc_call_return_rlocked(r) \
    do { \
        result=(r); \
        rpc_dplx_ruc(clnt); \
        goto out; \
    } while (0);

static enum clnt_stat
clnt_vc_call(CLIENT *clnt,
             AUTH *auth,
             rpcproc_t proc,
             xdrproc_t xdr_args,
             void *args_ptr,
             xdrproc_t xdr_results,
             void *results_ptr,
             struct timeval timeout)
{
    struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
    struct ct_data *ct = &(xd->cx.data);
    struct rpc_dplx_rec *rec = xd->rec;
    enum clnt_stat result = RPC_SUCCESS;
    rpc_ctx_t *ctx = NULL;
    XDR *xdrs;
    int code, refreshes = 2;
    bool ctx_needack = false;
    bool shipnow;

    /* Create a call context.  A lot of TI-RPC decisions need to be
     * looked at, including:
     *
     * 1. the client has a serialized call.  This looks harmless, so long
     * as the xid is adjusted.
     *
     * 2. the last xid used is now saved in handle shared private data, it
     * will be incremented by rpc_call_create (successive calls).  There's no
     * more reason to use the old time-dependent xid logic.  It should be
     * preferable to count atomically from 1.
     *
     * 3. the client has an XDR structure, which contains the initialied
     * xdrrec stream.  Since there is only one physical byte stream, it
     * would potentially be worse to do anything else?  The main issue which
     * will arise is the need to transition the stream between calls--which
     * may require adjustment to xdrrec code.  But on review it seems to
     * follow that one xdrrec stream would be parameterized by different call
     * contexts.  We'll keep the call parameters, control transfer machinery,
     * etc, in an rpc_ctx_t, to permit this.
     */
    ctx = alloc_rpc_call_ctx(clnt, proc, xdr_args, args_ptr, xdr_results,
                             results_ptr, timeout); /*add total timeout? */

    if (! ct->ct_waitset) {
        /* If time is not within limits, we ignore it. */
        if (time_not_ok(&timeout) == FALSE)
            ct->ct_wait = timeout;
    }

    shipnow =
        (xdr_results == NULL && timeout.tv_sec == 0
         && timeout.tv_usec == 0) ? FALSE : TRUE;

call_again:
    rpc_dplx_slc(clnt);

    xdrs = &(xd->shared.xdrs_out);

    xdrs->x_lib[0] = (void *) RPC_DPLX_CLNT;
    xdrs->x_lib[1] = (void *) ctx; /* transiently thread call ctx */

    ctx->error.re_status = RPC_SUCCESS;
    ct->ct_u.ct_mcalli = ntohl(ctx->xid);

    if ((! XDR_PUTBYTES(xdrs, ct->ct_u.ct_mcallc, ct->ct_mpos)) ||
        (! XDR_PUTINT32(xdrs, (int32_t *)&proc)) ||
        (! AUTH_MARSHALL(auth, xdrs)) ||
        (! AUTH_WRAP(auth, xdrs, xdr_args, args_ptr))) {
        if (ctx->error.re_status == RPC_SUCCESS)
            ctx->error.re_status = RPC_CANTENCODEARGS;
        (void)xdrrec_endofrecord(xdrs, TRUE);
        vc_call_return_slocked(ctx->error.re_status);
    }

    if (! xdrrec_endofrecord(xdrs, shipnow))
        vc_call_return_slocked(ctx->error.re_status = RPC_CANTSEND);

    if (! shipnow)
        vc_call_return_slocked(RPC_SUCCESS);

    /*
     * Hack to provide rpc-based message passing
     */
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
        vc_call_return_slocked(ctx->error.re_status = RPC_TIMEDOUT);

    /* reply */
    rpc_dplx_suc(clnt);
    rpc_dplx_rlc(clnt);

    /* if the channel is bi-directional, then the the shared conn is in a
     * svc event loop, and recv processing decodes reply headers */

    xdrs = &(xd->shared.xdrs_in);

    xdrs->x_lib[0] = (void *) RPC_DPLX_CLNT;
    xdrs->x_lib[1] = (void *) ctx; /* transiently thread call ctx */
    
    if (rec->hdl.xprt) {
        code = rpc_ctx_wait_reply(ctx, RPC_DPLX_FLAG_LOCKED); /* RECV! */
        if (code == ETIMEDOUT) {
            /* UL can retry, we dont.  This CAN indicate xprt destroyed
             * (error status already set). */
            goto unlock;
        }
        /* switch on direction */
        switch (ctx->msg->rm_direction) {
        case REPLY:
            if (ctx->msg->rm_xid == ctx->xid) {
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
        while (TRUE) {

            /* skiprecord */
            if (! xdrrec_skiprecord(xdrs)) {
                __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                        "%s: error at skiprecord", __func__);
                vc_call_return_rlocked(ctx->error.re_status);
            }

            /* now decode and validate the response header */
            if (! xdr_dplx_msg(xdrs, ctx->msg)) {
                __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                        "%s: error at xdr_dplx_msg_start", __func__);
                vc_call_return_rlocked(ctx->error.re_status);
            }

            /* switch on direction */
            switch (ctx->msg->rm_direction) {
            case REPLY:
                if (ctx->msg->rm_xid == ctx->xid)
                    goto replied;
                break;
            case CALL:
                /* in this configuration, we do not expect calls */
                break;
            default:
                break;
            }
        } /* while (TRUE) */
    } /* ! bi-directional */

    /*
     * process header
     */
replied:
    /* XXX move into routine which can be called from rpc_ctx_xfer_replymsg,
     * for (maybe) reduced MP overhead */
    _seterr_reply(ctx->msg, &(ctx->error));
    if (ctx->error.re_status == RPC_SUCCESS) {
        if (! AUTH_VALIDATE(auth, &(ctx->msg->acpted_rply.ar_verf))) {
            ctx->error.re_status = RPC_AUTHERROR;
            ctx->error.re_why = AUTH_INVALIDRESP;
        } else if (! AUTH_UNWRAP(auth, xdrs,
                                 xdr_results, results_ptr)) {
            if (ctx->error.re_status == RPC_SUCCESS)
                ctx->error.re_status = RPC_CANTDECODERES;
        }
        /* free verifier ... */
        if (ctx->msg->acpted_rply.ar_verf.oa_base != NULL) {
            xdrs->x_op = XDR_FREE;
            (void)xdr_opaque_auth(xdrs, &(ctx->msg->acpted_rply.ar_verf));
        }
	if (ctx_needack)
	    rpc_ctx_ack_xfer(ctx);
    }  /* end successful completion */
    else {
        /* maybe our credentials need to be refreshed ... */
        if (refreshes-- && AUTH_REFRESH(auth, &(ctx->msg))) {
            rpc_ctx_next_xid(ctx, RPC_CTX_FLAG_NONE);
            rpc_dplx_ruc(clnt);
            if (ctx_needack)
		rpc_ctx_ack_xfer(ctx);
            goto call_again;
        }
    }  /* end of unsuccessful completion */

unlock:
    vc_call_return_rlocked(ctx->error.re_status);

out:
    if (ctx)
        free_rpc_call_ctx(ctx, RPC_CTX_FLAG_NONE);

    return (result);
}

static void
clnt_vc_geterr(CLIENT *clnt, struct rpc_err *errp)
{
    struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
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
    XDR xdrs = {
        .x_public = NULL,
        .x_lib = { NULL, NULL }
    };
    sigset_t mask, newmask;
    bool rslt;
    xdrmem_create(&xdrs, res_ptr, ~0, XDR_FREE);

    /* XXX is this (legacy) signalling/barrier logic needed? */

    /* Handle our own signal mask here, the signal section is
     * larger than the wait (not 100% clear why) */
    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

    /* barrier recv channel */
    rpc_dplx_rwc(clnt, rpc_flag_clear);

    rslt = (*xdr_res)(&xdrs, res_ptr);

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
    struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
    struct ct_data *ct = &(xd->cx.data);
    void *infop = info;
    bool rslt = TRUE;

    /* always take recv lock first if taking together */
    rpc_dplx_rlc(clnt);
    rpc_dplx_slc(clnt);

    switch (request) {
    case CLSET_FD_CLOSE:
        ct->ct_closeit = TRUE;
        goto unlock;
        return (TRUE);
    case CLSET_FD_NCLOSE:
        ct->ct_closeit = FALSE;
        goto unlock;
        return (TRUE);
    default:
        break;
    }

    /* for other requests which use info */
    if (info == NULL) {
        rslt = FALSE;
        goto unlock;
    }
    switch (request) {
    case CLSET_TIMEOUT:
        if (time_not_ok((struct timeval *)info)) {
            rslt = FALSE;
            goto unlock;
        }
        ct->ct_wait = *(struct timeval *)infop;
        ct->ct_waitset = TRUE;
        break;
    case CLGET_TIMEOUT:
        *(struct timeval *)infop = ct->ct_wait;
        break;
    case CLGET_SERVER_ADDR:
        (void) memcpy(info, ct->ct_addr.buf, (size_t)ct->ct_addr.len);
        break;
    case CLGET_FD:
        *(int *)info = ct->ct_fd;
        break;
    case CLGET_SVC_ADDR:
        /* The caller should not free this memory area */
        *(struct netbuf *)info = ct->ct_addr;
        break;
    case CLSET_SVC_ADDR:  /* set to new address */
        rslt = FALSE;
        goto unlock;
    case CLGET_XID:
        /*
         * use the knowledge that xid is the
         * first element in the call structure
         * This will get the xid of the PREVIOUS call
         */
        *(u_int32_t *)info =
            ntohl(*(u_int32_t *)(void *)&ct->ct_u.ct_mcalli);
        break;
    case CLSET_XID:
        /* This will set the xid of the NEXT call */
        *(u_int32_t *)(void *)&ct->ct_u.ct_mcalli =
            htonl(*((u_int32_t *)info) + 1);
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
            (u_int32_t *)(ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT);
        *(u_int32_t *)info = ntohl(*tmp);
    }
    break;

    case CLSET_VERS:
    {
        u_int32_t tmp = htonl(*(u_int32_t *)info);
        *(ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT) = tmp;
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
            (u_int32_t *)(ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT);
        *(u_int32_t *)info = ntohl(*tmp);
    }
    break;

    case CLSET_PROG:
    {
        u_int32_t tmp = htonl(*(u_int32_t *)info);
        *(ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT) = tmp;
    }
    break;

    default:
        rslt = FALSE;
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

    if (! (flags & CLNT_REF_FLAG_LOCKED))
        mutex_lock(&clnt->cl_lock);

    if (clnt->cl_flags & CLNT_FLAG_DESTROYED) {        
        mutex_unlock(&clnt->cl_lock);
        return (false);
    }
    refcnt = ++(clnt->cl_refcnt);
    mutex_unlock(&clnt->cl_lock);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%d %s: postref %p %u", __tirpc_dcounter, __func__, clnt, refcnt);

    return (true);
}

static void
clnt_vc_release(CLIENT *clnt, u_int flags)
{
    uint32_t cl_refcnt;

    if (! (flags & CLNT_RELEASE_FLAG_LOCKED))
        mutex_lock(&clnt->cl_lock);

    cl_refcnt = --(clnt->cl_refcnt);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%d %s: postunref %p cl_refcnt %u",
            __tirpc_dcounter, __func__, clnt, cl_refcnt);

    /* conditional destroy */
    if ((clnt->cl_flags & CLNT_FLAG_DESTROYED) &&
        (cl_refcnt == 0)) {
 
        struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
        struct rpc_dplx_rec *rec = xd->rec;
        uint32_t xd_refcnt;

        mutex_unlock(&clnt->cl_lock);

        mutex_lock(&rec->mtx);
        xd_refcnt = --(xd->refcnt);

        if (xd_refcnt == 0) {
            __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                    "%d %s: xd_refcnt %u on destroyed %p %u calling "
                    "vc_shared_destroy",
                    __tirpc_dcounter, __func__, clnt, cl_refcnt);
            vc_shared_destroy(xd); /* RECLOCKED */
        } else {
            __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                    "%d %s: xd_refcnt on destroyed %p %u omit "
                    "vc_shared_destroy",
                    __tirpc_dcounter, __func__, clnt, cl_refcnt);
            mutex_unlock(&rec->mtx);
        }
    }
    else
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

    xd = (struct x_vc_data *) clnt->cl_p1;
    rec = xd->rec;

    clnt->cl_flags |= CLNT_FLAG_DESTROYED;
    cl_refcnt = --(clnt->cl_refcnt);
    mutex_unlock(&clnt->cl_lock);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%d %s: cl_destroy %p cl_refcnt %u",
            __tirpc_dcounter, __func__, clnt, cl_refcnt);

    /* bidirectional */
    mutex_lock(&rec->mtx);
    xd->flags |= X_VC_DATA_FLAG_CLNT_DESTROYED; /* destroyed handle is dead */
    xd_refcnt = --(xd->refcnt);

    /* conditional destroy */
    if (xd_refcnt == 0) {
        __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                "%d %s: %p cl_refcnt %u xd_refcnt %u calling vc_shared_destroy",
                __tirpc_dcounter, __func__, clnt, cl_refcnt, xd_refcnt);
        vc_shared_destroy(xd); /* RECLOCKED */
    } else
        mutex_lock(&rec->mtx);

out:
    return;
 }

static struct clnt_ops *
clnt_vc_ops(void)
{
    static struct clnt_ops ops;
    extern mutex_t  ops_lock;
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
static bool
time_not_ok(struct timeval *t)
{
    return (t->tv_sec <= -1 || t->tv_sec > 100000000 ||
            t->tv_usec <= -1 || t->tv_usec > 1000000);
}
