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
        struct ct_data *ct = (struct ct_data *) (cl)->cl_private; \
        ct->ct_duplex.ct_xprt = (xprt); \
        ct->ct_duplex.ct_flags |= CT_FLAG_DUPLEX; \
        ct->ct_duplex.ct_flags &= ~CT_FLAG_XPRT_DESTROYED; \
	(xprt)->xp_p4 = (cl); \
    } while (0);

#define SetDestroyed(cl) do { \
    struct ct_data *ct = (struct ct_data *) (cl)->cl_private; \
    ct->ct_duplex.ct_flags &= ~CT_FLAG_DUPLEX; \
    ct->ct_duplex.ct_flags |= CT_FLAG_XPRT_DESTROYED; \
    ct->ct_duplex.ct_xprt = NULL; \
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

struct ct_data {
	int		ct_fd;		/* connection's fd */
	bool_t		ct_closeit;	/* close it on destroy */
	struct timeval	ct_wait;	/* wait interval in milliseconds */
	bool_t          ct_waitset;	/* wait set by clnt_control? */
	struct netbuf	ct_addr;	/* remote addr */
#if 0
	struct rpc_err	ct_error;       /* no. */
#endif
	union {
		char	ct_mcallc[MCALL_MSG_SIZE];	/* marshalled callmsg */
		u_int32_t ct_mcalli;
	} ct_u;
	u_int		ct_mpos;	/* pos after marshal */
	XDR		ct_xdrs;	/* XDR stream */
	uint32_t	ct_xid;
        struct vc_fd_rec *ct_crec;      /* unified sync */
	struct rpc_msg	ct_reply;       /* async reply */
	struct ct_wait_entry ct_sync;   /* wait for completion */
	struct ct_duplex {
		void *ct_xprt; /* duplex integration */
		u_int ct_flags;
	} ct_duplex;
};

/* events */
bool_t cond_block_events_client(CLIENT *cl);
void cond_unblock_events_client(CLIENT *cl);

#endif /* _CLNT_INTERNAL_H */
