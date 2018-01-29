/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * contributeur : William Allen Simpson <bill@cohortfs.com>
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

/**
 * \file	rpc_rdma.h
 * \brief	rdma helper include file
 *
 * This was (very) loosely based on the Mooshika library, which in turn
 * was a mix of diod, rping (librdmacm/examples), and Linux kernel's
 * net/9p/trans_rdma.c (dual BSD/GPL license). No vestiges remain.
 */

#ifndef _TIRPC_RPC_RDMA_H
#define _TIRPC_RPC_RDMA_H

#include <rdma/rdma_cma.h>
#include <rpc/svc.h>
#include <rpc/xdr_ioq.h>

#include "rpc_dplx_internal.h"

typedef union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in sa_in;
	struct sockaddr_in6 sa_int6;
	struct sockaddr_storage sa_stor;
} sockaddr_union_t;

struct msk_stats {
	uint64_t rx_bytes;
	uint64_t rx_pkt;
	uint64_t rx_err;
	uint64_t tx_bytes;
	uint64_t tx_pkt;
	uint64_t tx_err;
	/* times only set if debug has MSK_DEBUG_SPEED */
	uint64_t nsec_callback;
	uint64_t nsec_compevent;
};

typedef struct rpc_rdma_xprt RDMAXPRT;

struct rpc_rdma_cbc;
typedef int (*rpc_rdma_callback_t)(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt);

/**
 * \struct rpc_rdma_cbc
 * Context data we can use during recv/send callbacks
 */
struct rpc_rdma_cbc {
	struct xdr_ioq workq;
	struct xdr_ioq holdq;

	struct xdr_ioq_uv *call_uv;
	void *call_head;
	void *read_chunk;	/* current in indexed list of arrays */
	void *write_chunk;	/* current in list of arrays */
	void *reply_chunk;	/* current in array */
	void *call_data;

	struct work_pool_entry wpe;
	rpc_rdma_callback_t positive_cb;
	rpc_rdma_callback_t negative_cb;
	void *callback_arg;

	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;

	enum ibv_wc_opcode opcode;
	enum ibv_wc_status status;

	struct ibv_sge sg_list[0];	/**< this is actually an array.
					note that when you malloc
					you have to add its size */
};

struct rpc_rdma_pd {
	LIST_ENTRY(rpc_rdma_pd) pdl;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_srq *srq;
	struct poolq_head srqh;		/**< shared read contexts */

	uint32_t pd_used;
};

#define RDMAX_CLIENT 0
#define RDMAX_SERVER_CHILD -1

/**
 * \struct rpc_rdma_xprt
 * RDMA transport instance
 */
struct rpc_rdma_xprt {
	struct rpc_dplx_rec sm_dr;

	const struct rpc_rdma_attr *xa;	/**< (shared) configured attributes */

	struct rdma_event_channel *event_channel;
	struct rdma_cm_id *cm_id;	/**< RDMA CM ID */
	struct rpc_rdma_pd *pd;		/**< RDMA PD entry */

	struct ibv_comp_channel *comp_channel;
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_srq *srq;		/**< Shared Receive Queue pointer */

	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;

	struct ibv_mr *mr;
	u_int8_t *buffer_aligned;
	size_t buffer_total;

	struct xdr_ioq_uv_head inbufs;	/* recvsize */
	struct xdr_ioq_uv_head outbufs;	/* sendsz */

	struct poolq_head cbqh;		/**< combined callback contexts */

	mutex_t cm_lock;		/**< lock for connection events */
	cond_t cm_cond;			/**< cond for connection events */

	struct msk_stats stats;
	int stats_sock;

	u_int conn_type;		/**< RDMA Port space (RDMA_PS_TCP) */
	int server;			/**< connection backlog on server,
					 * 0 (RDMAX_CLIENT):
					 * client,
					 * -1 (RDMAX_SERVER_CHILD):
					 * server has accepted connection
					 */

	enum rdma_transport_state {
		RDMAXS_INITIAL, 	/* assumes zero, never set */
		RDMAXS_LISTENING,
		RDMAXS_ADDR_RESOLVED,
		RDMAXS_ROUTE_RESOLVED,
		RDMAXS_CONNECT_REQUEST,
		RDMAXS_CONNECTED,
		RDMAXS_CLOSING,
		RDMAXS_CLOSED,
		RDMAXS_ERROR, 		/* always last */
	} state;			/**< transport state machine */

	/* FIXME why configurable??? */
	bool destroy_on_disconnect;	/**< should perform cleanup */
};
#define RDMA_DR(p) (opr_containerof((p), struct rpc_rdma_xprt, sm_dr))

typedef struct rec_rdma_strm {
	RDMAXPRT *xprt;
	/*
	 * out-going bits
	 */
	int (*writeit)(void *, void *, int);
	TAILQ_HEAD(out_buffers_head, xdr_ioq_uv) out_buffers;
	char *out_base;		/* output buffer (points to frag header) */
	char *out_finger;	/* next output position */
	char *out_boundry;	/* data cannot up to this address */
	u_int32_t *frag_header;	/* beginning of curren fragment */
	bool frag_sent;	/* true if buffer sent in middle of record */
	/*
	 * in-coming bits
	 */
	TAILQ_HEAD(in_buffers_head, xdr_ioq_uv) in_buffers;
	u_long in_size;	/* fixed size of the input buffer */
	char *in_base;
	char *in_finger;	/* location of next byte to be had */
	char *in_boundry;	/* can read up to this location */
	long fbtbc;		/* fragment bytes to be consumed */
	bool last_frag;
	u_int sendsize;
	u_int recvsize;

	bool nonblock;
	u_int32_t in_header;
	char *in_hdrp;
	int in_hdrlen;
	int in_reclen;
	int in_received;
	int in_maxrec;

	cond_t cond;
	mutex_t lock;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	int credits;
} RECRDMA;

static inline void *xdr_encode_hyper(uint32_t *iptr, uint64_t val)
{
	*iptr++ = htonl((uint32_t)((val >> 32) & 0xffffffff));
	*iptr++ = htonl((uint32_t)(val & 0xffffffff));
	return iptr;
}

static inline uint64_t xdr_decode_hyper(uint64_t *iptr)
{
	return ((uint64_t) ntohl(((uint32_t*)iptr)[0]) << 32)
		| (ntohl(((uint32_t*)iptr)[1]));
}

void rpc_rdma_internals_init(void);
void rpc_rdma_internals_fini(void);

/* server specific */
int rpc_rdma_accept_finalize(RDMAXPRT *);
RDMAXPRT *rpc_rdma_accept_wait(RDMAXPRT *, int);
void rpc_rdma_destroy(RDMAXPRT *);

enum xprt_stat svc_rdma_rendezvous(SVCXPRT *);

/* client */
int rpc_rdma_connect(RDMAXPRT *);
int rpc_rdma_connect_finalize(RDMAXPRT *);

/* XDR functions */
int xdr_rdma_create(RDMAXPRT *);
void xdr_rdma_callq(RDMAXPRT *);
void xdr_rdma_destroy(RDMAXPRT *);

bool xdr_rdma_clnt_reply(XDR *, u_int32_t);
bool xdr_rdma_clnt_flushout(struct rpc_rdma_cbc *);

bool xdr_rdma_svc_recv(struct rpc_rdma_cbc *, u_int32_t);
bool xdr_rdma_svc_reply(struct rpc_rdma_cbc *, u_int32_t);
bool xdr_rdma_svc_flushout(struct rpc_rdma_cbc *);

#endif /* !_TIRPC_RPC_RDMA_H */
