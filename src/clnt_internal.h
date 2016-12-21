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
#define _CLNT_INTERNAL_H

struct ct_wait_entry
{
	mutex_t mtx;
	cond_t  cv;
};

#include <misc/rbtree_x.h>
#include <rpc/work_pool.h>
#include <rpc/xdr_ioq.h>
#include <misc/wait_queue.h>

typedef struct rpc_dplx_lock {
	struct wait_entry we;
	int32_t lock_flag_value;	/* XXX killme */
	struct {
		const char *func;
		int line;
	} locktrace;
} rpc_dplx_lock_t;

#define MCALL_MSG_SIZE 24

#define CT_NONE                 0x0000
#define CT_EVENTS_BLOCKED       0x0002
#define CT_EPOLL_ACTIVE         0x0004
#define CT_XPRT_DESTROYED       0x0008

/*
 * A client call context.  Intended to enable efficient multiplexing of
 * client calls sharing a client channel.
 */
typedef struct rpc_call_ctx {
	struct opr_rbtree_node node_k;
	struct wait_entry we;
	uint32_t xid;
	uint32_t flags;
	struct rpc_err error;
	union {
		struct {
			struct rpc_client *clnt;
			struct x_vc_data *xd;
			struct timespec timeout;
		} clnt;
		struct {
			/* nothing */
		} svc;
	} ctx_u;
	struct rpc_msg cc_msg;
} rpc_ctx_t;

static inline int call_xid_cmpf(const struct opr_rbtree_node *lhs,
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
	XDR cu_outxdrs;
	int cu_fd;		/* connections fd */
	bool cu_closeit;	/* opened by library */
	struct sockaddr_storage cu_raddr;	/* remote address */
	int cu_rlen;
	struct timeval cu_wait;	/* retransmit interval */
	struct timeval cu_total;	/* total time for the call */
	struct rpc_err cu_error;
	u_int cu_xdrpos;
	u_int cu_sendsz;	/* send size */
	u_int cu_recvsz;	/* recv size */
	int cu_async;
	int cu_connect;		/* Use connect(). */
	int cu_connected;	/* Have done connect(). */
	/* formerly, buffers were tacked onto the end */
	char *cu_inbuf;
	char *cu_outbuf;
};

struct ct_serialized {
	union {
		char ct_mcallc[MCALL_MSG_SIZE];	/* marshalled callmsg */
		u_int32_t ct_mcalli;
	} ct_u;
	u_int ct_mpos;		/* pos after marshal */
};

struct ct_data {
	int ct_fd;
	bool ct_closeit;	/* close it on destroy */
	struct timeval ct_wait;	/* wait interval in milliseconds */
	bool ct_waitset;	/* wait set by clnt_control? */
	struct netbuf ct_addr;	/* remote addr */
	struct wait_entry ct_sync;	/* wait for completion */
};

#ifdef USE_RPC_RDMA
struct cm_data {
	XDR cm_xdrs;
	char *buffers;
	struct timeval cm_wait; /* wait interval in milliseconds */
	struct timeval cm_total; /* total time for the call */
	struct rpc_err cm_error;
	struct rpc_msg call_msg;
	//add a lastreceive?
	u_int cm_xdrpos;
	bool cm_closeit; /* close it on destroy */
};
#endif

enum CX_TYPE
{
	CX_DG_DATA,
	CX_VC_DATA,
	CX_MSK_DATA
};

#define X_VC_DATA_FLAG_NONE             0x0000
#define X_VC_DATA_FLAG_SVC_DESTROYED    0x0001

/* new unified state */
struct rpc_dplx_rec {
	int fd_k;
#if 0
	mutex_t mtx;
#else
	struct {
		mutex_t mtx;
		const char *func;
		int line;
	} locktrace;
#endif
	struct opr_rbtree_node node_k;
	uint32_t refcnt;
	struct {
		rpc_dplx_lock_t lock;
	} send;
	struct {
		rpc_dplx_lock_t lock;
	} recv;
	struct {
		struct x_vc_data *xd;
		SVCXPRT *xprt;
	} hdl;
};

#define REC_LOCK(rec) \
	do { \
		mutex_lock(&((rec)->locktrace.mtx)); \
		(rec)->locktrace.func = __func__; \
		(rec)->locktrace.line = __LINE__; \
	} while (0)

#define REC_UNLOCK(rec) mutex_unlock(&((rec)->locktrace.mtx))

struct cx_data {
	enum CX_TYPE type;
	union {
		struct cu_data cu;
		struct ct_data ct;
#ifdef USE_RPC_RDMA
		struct cm_data cm;
#endif
	} c_u;
	int cx_fd;		/* connection's fd */
	struct rpc_dplx_rec *cx_rec;	/* unified sync */
};

struct x_vc_data {
	struct work_pool_entry wpe;	/*** 1st ***/
	struct rpc_dplx_rec *rec;	/* unified sync */
	uint32_t flags;
	uint32_t refcnt;
	struct {
		struct ct_data data;
		struct {
			uint32_t xid;	/* current xid */
			struct opr_rbtree t;
		} calls;
	} cx;
	struct {
		enum xprt_stat strm_stat;
		struct timespec last_recv;	/* XXX move to shared? */
		int32_t maxrec;
	} sx;
	struct {
		struct poolq_head ioq;
		bool active;
		bool nonblock;
		u_int sendsz;
		u_int recvsz;
		XDR xdrs_in;	/* send queue */
		XDR xdrs_out;	/* recv queue */
	} shared;
};

#define CU_DATA(cx) (&(cx)->c_u.cu)
#define CT_DATA(cx) (&(cx)->c_u.ct)
#define CM_DATA(cx) (&(cx)->c_u.cm)

/* compartmentalize a bit */
static inline struct x_vc_data *
alloc_x_vc_data(void)
{
	struct x_vc_data *xd = mem_zalloc(sizeof(struct x_vc_data));

	TAILQ_INIT(&xd->shared.ioq.qh);
	return (xd);
}

static inline void
free_x_vc_data(struct x_vc_data *xd)
{
	mem_free(xd, sizeof(struct x_vc_data));
}

static inline struct cx_data *
alloc_cx_data(enum CX_TYPE type, uint32_t sendsz,
	      uint32_t recvsz)
{
	struct cx_data *cx = mem_zalloc(sizeof(struct cx_data));
	cx->type = type;
	switch (type) {
	case CX_DG_DATA:
		cx->c_u.cu.cu_inbuf = mem_alloc(recvsz);
		cx->c_u.cu.cu_outbuf = mem_alloc(sendsz);
	case CX_VC_DATA:
	case CX_MSK_DATA:
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
	case CX_MSK_DATA:
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

void vc_shared_destroy(struct x_vc_data *xd);

#endif				/* _CLNT_INTERNAL_H */
