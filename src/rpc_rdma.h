/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
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
 *
 */

/*
 * rpc_rdma.h
 * rpc rdma message definition
 */

#ifndef _TIRPC_RPC_RDMA_H
#define _TIRPC_RPC_RDMA_H

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#define MOOSHIKA_API_VERSION 5

typedef struct msk_trans msk_trans_t;
typedef struct msk_trans_attr msk_trans_attr_t;

/**
 * \struct msk_data
 * data size and content to send/just received
 */
typedef struct msk_data {
	uint32_t max_size; /**< size of the data field */
	uint32_t size; /**< size of the data to actually send/read */
	uint8_t *data; /**< opaque data */
	struct msk_data *next; /**< For recv/sends with multiple elements, used as a linked list */
	struct ibv_mr *mr;
} msk_data_t;

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

struct msk_pd {
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_srq *srq;
	struct msk_ctx *rctx;
	void *private;
	uint32_t refcnt;
	uint32_t used;
};
#define PD_GUARD ((void*)-1)

typedef void (*disconnect_callback_t) (msk_trans_t *trans);

#define MSK_CLIENT 0
#define MSK_SERVER_CHILD -1

/**
 * \struct msk_trans
 * RDMA transport instance
 */
struct msk_trans {
	enum msk_state {
		MSK_INIT,
		MSK_LISTENING,
		MSK_ADDR_RESOLVED,
		MSK_ROUTE_RESOLVED,
		MSK_CONNECT_REQUEST,
		MSK_CONNECTED,
		MSK_CLOSING,
		MSK_CLOSED,
		MSK_ERROR
	} state;			/**< tracks the transport state machine for connection setup and tear down */
	struct rdma_cm_id *cm_id;	/**< The RDMA CM ID */
	struct rdma_event_channel *event_channel;
	struct ibv_comp_channel *comp_channel;
	struct msk_pd *pd;		/**< Protection Domain pointer list */
	struct ibv_qp *qp;		/**< Queue Pair pointer */
	struct ibv_srq *srq;		/**< Shared Receive Queue pointer */
	struct ibv_cq *cq;		/**< Completion Queue pointer */
	disconnect_callback_t disconnect_callback;
	void *private_data;
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	char *node;			/**< The remote peer's hostname */
	char *port;			/**< The service port (or name) */
	int conn_type;			/**< RDMA Port space, probably RDMA_PS_TCP */
	int server;			/**< 0 if client, connection backlog on server, -1 (MSK_SERVER_CHILD) if server's accepted connection */
	int destroy_on_disconnect;      /**< set to 1 if mooshika should perform cleanup */
	uint32_t debug;
	struct rdma_cm_id **conn_requests; /**< temporary child cm_id, only used for server */
	struct msk_ctx *wctx;		/**< pointer to actual context data */
	struct msk_ctx *rctx;		/**< pointer to actual context data */
	pthread_mutex_t cm_lock;	/**< lock for connection events */
	pthread_cond_t cm_cond;		/**< cond for connection events */
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
	struct msk_stats stats;
	char *stats_prefix;
	int stats_sock;
};

struct msk_trans_attr {
	disconnect_callback_t disconnect_callback;
	int debug;			/**< verbose output to stderr if set */
	int server;			/**< 0 if client, connection backlog on server */
	int destroy_on_disconnect;      /**< set to 1 if mooshika should perform cleanup */
	long timeout;			/**< Number of mSecs to wait for connection management events */
	int sq_depth;			/**< The depth of the Send Queue */
	int max_send_sge;		/**< Maximum number of s/g elements per send */
	int use_srq;			/**< Does the server use srq? */
	int rq_depth;			/**< The depth of the Receive Queue. */
	int max_recv_sge;		/**< Maximum number of s/g elements per recv */
	int worker_count;		/**< Number of worker threads - works only for the first init */
	int worker_queue_size;		/**< Size of the worker data queue - works only for the first init */
	int conn_type;			/**< RDMA Port space, probably RDMA_PS_TCP */
	char *node;			/**< The remote peer's hostname */
	char *port;			/**< The service port (or name) */
	struct msk_pd *pd;		/**< Protection Domain pointer */
	char *stats_prefix;
};

#define MSK_DEBUG_EVENT 0x0001
#define MSK_DEBUG_SETUP 0x0002
#define MSK_DEBUG_SEND  0x0004
#define MSK_DEBUG_RECV  0x0008
#define MSK_DEBUG_WORKERS (MSK_DEBUG_SEND | MSK_DEBUG_RECV)
#define MSK_DEBUG_CM_LOCKS   0x0010
#define MSK_DEBUG_CTX   0x0020
#define MSK_DEBUG_SPEED 0x8000

typedef void (*ctx_callback_t)(msk_trans_t *trans, msk_data_t *data, void *arg);

/**
 * \struct msk_rloc
 * stores one remote address to write/read at
 */
typedef struct msk_rloc {
	uint64_t raddr; /**< remote memory address */
	uint32_t rkey; /**< remote key */
	uint32_t size; /**< size of the region we can write/read */
} msk_rloc_t;

typedef struct rec_rdma_strm {
	msk_trans_t *trans;
	/*
	 * out-goung bits
	 */
	int (*writeit)(void *, void *, int);
	msk_data_t *out_buffers;
	char *out_base;		/* output buffer (points to frag header) */
	char *out_finger;	/* next output position */
	char *out_boundry;	/* data cannot up to this address */
	u_int32_t *frag_header;	/* beginning of curren fragment */
	bool frag_sent;	/* true if buffer sent in middle of record */
	/*
	 * in-coming bits
	 */
	msk_data_t *in_buffers;
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

	pthread_cond_t cond;
	pthread_mutex_t lock;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	int credits;
} RECRDMA;

/* what follows is rcp_rdma.h from kernel, translated to uint* types */

struct rpcrdma_segment {
	uint32_t rs_handle;	/* Registered memory handle */
	uint32_t rs_length;	/* Length of the chunk in bytes */
	uint64_t rs_offset;	/* Chunk virtual address or offset */
};

/*
 * read chunk(s), encoded as a linked list.
 */
struct rpcrdma_read_chunk {
	uint32_t rc_discrim;	/* 1 indicates presence */
	uint32_t rc_position;	/* Position in XDR stream */
	struct rpcrdma_segment rc_target;
};

/*
 * write chunk, and reply chunk.
 */
struct rpcrdma_write_chunk {
	struct rpcrdma_segment wc_target;
};

/*
 * write chunk(s), encoded as a counted array.
 */
struct rpcrdma_write_array {
	uint32_t wc_discrim;	/* 1 indicates presence */
	uint32_t wc_nchunks;	/* Array count */
	struct rpcrdma_write_chunk wc_array[0];
};

struct rpcrdma_msg {
	uint32_t rm_xid;	/* Mirrors the RPC header xid */
	uint32_t rm_vers;	/* Version of this protocol */
	uint32_t rm_credit;	/* Buffers requested/granted */
	uint32_t rm_type;	/* Type of message (enum rpcrdma_proc) */
	union {

		struct {			/* no chunks */
			uint32_t rm_empty[3];	/* 3 empty chunk lists */
		} rm_nochunks;

		struct {			/* no chunks and padded */
			uint32_t rm_align;	/* Padding alignment */
			uint32_t rm_thresh;	/* Padding threshold */
			uint32_t rm_pempty[3];	/* 3 empty chunk lists */
		} rm_padded;

		uint32_t rm_chunks[0];		/* read, write and reply chunks */

	} rm_body;
};

enum rpcrdma_proc {
	RDMA_MSG = 0,	/* An RPC call or reply msg */
	RDMA_NOMSG = 1,	/* An RPC call or reply msg - separate body */
	RDMA_MSGP = 2,	/* An RPC call or reply msg with padding */
	RDMA_DONE = 3,	/* Client signals reply completion */
	RDMA_ERROR = 4	/* An RPC RDMA encoding error */
};

static inline void * xdr_encode_hyper(uint32_t *iptr, uint64_t val) {
	*iptr++ = htonl((uint32_t)((val >> 32) & 0xffffffff));
	*iptr++ = htonl((uint32_t)(val & 0xffffffff));
	return iptr;
}

static inline uint64_t xdr_decode_hyper(uint64_t *iptr) {
	return ((uint64_t) ntohl(((uint32_t*)iptr)[0]) << 32) | (ntohl(((uint32_t*)iptr)[1]));
}

//FIXME: "MAX" size for headers doesn't make sense
#define HDR_MAX_SIZE 64

int msk_post_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg);
int msk_post_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void *callback_arg);
int msk_wait_n_recv(msk_trans_t *trans, msk_data_t *data, int num_sge);
int msk_wait_n_send(msk_trans_t *trans, msk_data_t *data, int num_sge);
int msk_post_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg);
int msk_post_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg);
int msk_wait_n_read(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc);
int msk_wait_n_write(msk_trans_t *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc);

int msk_init(msk_trans_t **ptrans, msk_trans_attr_t *attr);

// server specific:
int msk_bind_server(msk_trans_t *trans);
msk_trans_t *msk_accept_one_wait(msk_trans_t *trans, int msleep);
msk_trans_t *msk_accept_one_timedwait(msk_trans_t *trans, struct timespec *abstime);
static inline msk_trans_t *msk_accept_one(msk_trans_t *trans) {
	return msk_accept_one_timedwait(trans, NULL);
}
int msk_finalize_accept(msk_trans_t *trans);
void msk_destroy_trans(msk_trans_t **ptrans);

int msk_connect(msk_trans_t *trans);
int msk_finalize_connect(msk_trans_t *trans);

/* utility functions */

struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access);
int msk_dereg_mr(struct ibv_mr *mr);

msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size);

void msk_print_devinfo(msk_trans_t *trans);

struct sockaddr *msk_get_dst_addr(msk_trans_t *trans);
struct sockaddr *msk_get_src_addr(msk_trans_t *trans);
uint16_t msk_get_src_port(msk_trans_t *trans);
uint16_t msk_get_dst_port(msk_trans_t *trans);

struct msk_pd *msk_getpd(msk_trans_t *trans);

/* XDR functions */

/* XDR using msk */
int    xdrmsk_create(XDR *, msk_trans_t *,
                            u_int, u_int, u_int,
			    void (*)(void*), void*);

int    rpcrdma_svc_setbuf(XDR *, u_int32_t, enum xdr_op);
bool rpcrdma_svc_flushout(XDR *);

int    rpcrdma_clnt_setbuf(XDR *, u_int32_t, enum xdr_op);
bool rpcrdma_clnt_flushout(XDR *);

#endif /* !_TIRPC_RPC_RDMA_H */
