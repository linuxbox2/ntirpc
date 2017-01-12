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

#include "rpc_dplx_internal.h"

#define MCALL_MSG_SIZE 24

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

struct ct_data {
	union {
		char ct_mcallc[MCALL_MSG_SIZE];	/* marshalled callmsg */
		u_int32_t ct_mcalli;
	} ct_u;
	u_int ct_mpos;		/* pos after marshal */
	int ct_fd;
	bool ct_closeit;	/* close it on destroy */
	struct timeval ct_wait;	/* wait interval in milliseconds */
	bool ct_waitset;	/* wait set by clnt_control? */
	struct sockaddr_storage ct_raddr;	/* remote addr */
	struct wait_entry ct_sync;	/* wait for completion */
	int ct_rlen;
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

#define X_VC_DATA_FLAG_NONE             0x0000
#define X_VC_DATA_FLAG_SVC_DESTROYED    0x0001

struct cx_data {
	struct rpc_client cx_c;		/**< Transport Independent handle */
	struct rpc_dplx_rec *cx_rec;	/* unified sync */

	union {
		struct cu_data cu;
		struct ct_data ct;
#ifdef USE_RPC_RDMA
		struct cm_data cm;
#endif
	} c_u;
};
#define CX_DATA(p) (opr_containerof((p), struct cx_data, cx_c))

struct x_vc_data {
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

	return (xd);
}

static inline void
free_x_vc_data(struct x_vc_data *xd)
{
	mem_free(xd, sizeof(struct x_vc_data));
}

static inline struct cx_data *
alloc_cx_data(enum CX_TYPE type, uint32_t sendsz, uint32_t recvsz)
{
	struct cx_data *cx = mem_zalloc(sizeof(struct cx_data));

	mutex_init(&cx->cx_c.cl_lock, NULL);
	cx->cx_c.cl_refcnt = 1;

	cx->cx_c.cl_type = type;
	switch (type) {
	case CX_DG_DATA:
		cx->c_u.cu.cu_recvsz = recvsz;
		cx->c_u.cu.cu_sendsz = sendsz;
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
	mutex_destroy(&cx->cx_c.cl_lock);

	/* note seemingly pointers to constant ""? */
	if (cx->cx_c.cl_netid && cx->cx_c.cl_netid[0])
		mem_free(cx->cx_c.cl_netid, strlen(cx->cx_c.cl_netid) + 1);
	if (cx->cx_c.cl_tp && cx->cx_c.cl_tp[0])
		mem_free(cx->cx_c.cl_tp, strlen(cx->cx_c.cl_tp) + 1);

	switch (cx->cx_c.cl_type) {
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
