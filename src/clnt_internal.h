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
 * Copyright (c) 1986 - 1991 by Sun Microsystems, Inc.
 */

/*
 * clnt_internal.h  Internal client structures needed by some async
 * svc routines
 */

#ifndef _CLNT_INTERNAL_H
#define	_CLNT_INTERNAL_H

struct ct_wait_entry {
    mutex_t mtx;
    cond_t  cv;
};

#include <misc/rbtree_x.h>

#ifndef HAVE_STRLCPY
extern size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

/* new unified client state */
struct vc_fd_rec
{
    int fd_k;
    int32_t refcount;
    int32_t lock_flag_value; /* state of lock at fd */
    struct opr_rbtree_node node_k;
    mutex_t mtx;
    cond_t  cv;
    struct {
        char file[32];
        uint32_t line;
    } locktrace;
    struct {
        uint32_t xid; /* current xid */
        uint32_t depends_xid;
        struct opr_rbtree t;
    } calls;
};

#define MCALL_MSG_SIZE 24

#define CT_FLAG_NONE              0x0000
#define CT_FLAG_DUPLEX            0x0001
#define CT_FLAG_EVENTS_BLOCKED    0x0002
#define CT_FLAG_EPOLL_ACTIVE      0x0004
#define CT_FLAG_XPRT_DESTROYED    0x0008

/*
 * A client call context.  Intended to enable efficient multiplexing of
 * client calls sharing a client channel.
 */
enum rpc_ctx_state {
    RPC_CTX_START,
    RPC_CTX_REPLY_WAIT,
    RPC_CTX_FINISHED
};

typedef struct __rpc_call_ctx {
    struct opr_rbtree_node node_k;
    struct {
        mutex_t mtx;
        cond_t  cv;
    } we;
    uint32_t xid;
    enum rpc_ctx_state state;
    uint32_t flags;
    struct rpc_msg *msg;
    struct rpc_err error;
    union {
        struct {
            struct __rpc_client *cl;
            rpcproc_t proc;
            xdrproc_t xdr_args;
            void *args_ptr;
            xdrproc_t xdr_results;
            void *results_ptr;
        } clnt;
        struct {
            /* nothing */
        } svc;
    } ctx_u;
    void *u_data[2]; /* caller user data */
} rpc_ctx_t;

#define SetDuplex(cl, xprt) do { \
        struct cx_data *cx = (struct cx_data *) (cl)->cl_private; \
        cx->cx_duplex.xprt = (xprt); \
        cx->cx_duplex.flags |= CT_FLAG_DUPLEX; \
        cx->cx_duplex.flags &= ~CT_FLAG_XPRT_DESTROYED; \
	(xprt)->xp_p4 = (cl); \
    } while (0);

#define SetDestroyed(cl) do { \
    struct cx_data *cx = (struct cx_data *) (cl)->cl_private; \
    cx->cx_duplex.flags &= ~CT_FLAG_DUPLEX; \
    cx->cx_duplex.flags |= CT_FLAG_XPRT_DESTROYED; \
    cx->cx_duplex.xprt = NULL; \
    } while (0);

static inline int
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

/* unify client private data  */

struct cu_data {
    int cu_fd;          /* connections fd */
    bool_t cu_closeit;	/* opened by library */
    struct sockaddr_storage cu_raddr; /* remote address */
    int	cu_rlen;
    struct timeval cu_wait; /* retransmit interval */
    struct timeval cu_total; /* total time for the call */
    struct rpc_err cu_error;
    XDR	cu_outxdrs;
    u_int cu_xdrpos;
    u_int cu_sendsz;	/* send size */
    u_int cu_recvsz;	/* recv size */
    int	cu_async;
    int	cu_connect;	/* Use connect(). */
    int	cu_connected;	/* Have done connect(). */
    /* formerly, buffers were tacked onto the end */
    char *cu_inbuf;
    char *cu_outbuf;
};

struct ct_data {
    bool_t		ct_closeit; /* close it on destroy */
    struct timeval	ct_wait; /* wait interval in milliseconds */
    bool_t          ct_waitset;	/* wait set by clnt_control? */
    struct netbuf	ct_addr; /* remote addr */
#if 0
    struct rpc_err	ct_error; /* no. */
#endif
    union {
        char	ct_mcallc[MCALL_MSG_SIZE]; /* marshalled callmsg */
        u_int32_t ct_mcalli;
    } ct_u;
    u_int ct_mpos;      /* pos after marshal */
    XDR	ct_xdrs;        /* XDR stream */
    uint32_t ct_xid;
    struct rpc_msg	ct_reply; /* async reply */
    struct ct_wait_entry ct_sync; /* wait for completion */
};

enum CX_TYPE
{
    CX_DG_DATA,
    CX_VC_DATA
};

struct cx_data
{
    enum CX_TYPE type;
    union {
        struct cu_data cu;
        struct ct_data ct;
    } c_u;
    int	cx_fd;                  /* connection's fd */
    struct vc_fd_rec *cx_crec;  /* unified sync */
    struct {
        void *xprt;             /* duplex integration */
        uint32_t flags;
    } cx_duplex;
};

#define CU_DATA(cx) (& (cx)->c_u.cu)
#define CT_DATA(cx) (& (cx)->c_u.ct)

/* compartmentalize a bit */
static inline struct cx_data *
alloc_cx_data(enum CX_TYPE type, uint32_t sendsz, uint32_t recvsz)
{
    struct cx_data *cx = mem_alloc(sizeof(struct cx_data));
    cx->type = type;
    switch (type) {
    case CX_DG_DATA:
        cx->c_u.cu.cu_inbuf = mem_alloc(recvsz);
        cx->c_u.cu.cu_outbuf = mem_alloc(sendsz);
    case CX_VC_DATA:
        break;
    default:
        /* err */
        __warnx(TIRPC_DEBUG_FLAG_MEM,
                "%s: asked to allocate cx_data of unknown type (BUG)",
                __func__);
        break;
    };
    return (cx);
}

static inline void
free_cx_data(struct cx_data *cx)
{
    switch (cx->type) {
    case CX_DG_DATA:
        mem_free(cx->c_u.cu.cu_inbuf, cx->c_u.cu.cu_recvsz);
        mem_free(cx->c_u.cu.cu_outbuf, cx->c_u.cu.cu_sendsz);
    case CX_VC_DATA:
        break;
    default:
        /* err */
        __warnx(TIRPC_DEBUG_FLAG_MEM,
                "%s: asked to free cx_data of unknown type (BUG)",
                __func__);
        break;
    };
    mem_free(cx, sizeof(struct cx_data));
}

/* events */
bool_t cond_block_events_client(CLIENT *cl);
void cond_unblock_events_client(CLIENT *cl);

#endif /* _CLNT_INTERNAL_H */
