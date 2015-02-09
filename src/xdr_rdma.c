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
 */

#include <config.h>
#include <sys/cdefs.h>

#include "namespace.h"
#include <sys/types.h>

#include <netinet/in.h>
#include <mooshika.h>

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include "un-namespace.h"

#include "rpc_rdma.h"

static void xdrmsk_destroy(XDR *);
static bool xdrmsk_getlong_aligned(XDR *, long *);
static bool xdrmsk_putlong_aligned(XDR *, const long *);
static bool xdrmsk_getlong_unaligned(XDR *, long *);
static bool xdrmsk_putlong_unaligned(XDR *, const long *);
static bool xdrmsk_getbytes(XDR *, char *, u_int);
static bool xdrmsk_putbytes(XDR *, const char *, u_int);
/* XXX: w/64-bit pointers, u_int not enough! */
static u_int xdrmsk_getpos(XDR *);
static bool xdrmsk_setpos(XDR *, u_int);
static int32_t *xdrmsk_inline_aligned(XDR *, u_int);
static int32_t *xdrmsk_inline_unaligned(XDR *, u_int);

static const struct	xdr_ops xdrmsk_ops_aligned = {
	xdrmsk_getlong_aligned,
	xdrmsk_putlong_aligned,
	xdrmsk_getbytes,
	xdrmsk_putbytes,
	xdrmsk_getpos,
	xdrmsk_setpos,
	xdrmsk_inline_aligned,
	xdrmsk_destroy
};

static const struct	xdr_ops xdrmsk_ops_unaligned = {
	xdrmsk_getlong_unaligned,
	xdrmsk_putlong_unaligned,
	xdrmsk_getbytes,
	xdrmsk_putbytes,
	xdrmsk_getpos,
	xdrmsk_setpos,
	xdrmsk_inline_unaligned,
	xdrmsk_destroy
};

struct condlock {
	pthread_cond_t cond;
	pthread_mutex_t lock;
};

struct mskinfo {
	msk_trans_t *trans;
	char *pos; /* xdrmem's x_private */
	msk_data_t *xdrbuf;
	msk_data_t *curbuf;
	msk_data_t *callbuf;
	u_int sendsz;
	u_int recvsz;
	u_int credits;
	struct ibv_mr *mr;
	u_int8_t *mskbase;
	msk_data_t *inbufs;
	msk_data_t *outbufs;
	msk_data_t *hdrbufs;
	msk_data_t *rdmabufs;
	struct condlock cl;
	void (*callback)(void*);
	void *callbackarg;
};

#define priv(xdrs) ((struct mskinfo *)((xdrs)->x_private))
#define cl_of_xdrs(xdrs) (&priv(xdrs)->cl)
#define callback_of_xdrs(xdrs) (priv(xdrs)->callback)
#define callbackarg_of_xdrs(xdrs) (priv(xdrs)->callbackarg)

#define rpcrdma_dump_msg(data, base, xid)

#ifndef rpcrdma_dump_msg
static void rpcrdma_dump_msg(msk_data_t *data, char *base, uint32_t xid)
{
	char *buffer;
	int sized = data->size;
	int buffered = (((sized / 16) + 1) * (12 + (9 * 4))) + 1;
	int i = 0;
	int m = 0;

	if (sized == 0) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "rpcrdma %x %s?", xid, base);
		return;
	}
	buffer = (char *)mem_alloc(buffered);

	while (sized > i) {
		int j = sized - i;
		int k = j < 16 ? j : 16;
		int l = 0;
		int r = sprintf(&buffer[m], "\n%10d:", i);

		if (r < 0)
			goto quit;
		m += r;

		for (; l < k; l++) {
			if (l % 4 == 0)
				buffer[m++] = ' ';

			r = sprintf(&buffer[m], "%02X", data->data[i++]);
			if (r < 0)
				goto quit;
			m += r;
		}
	}
quit:
	buffer[m] = '\0';	/* in case of error */
	__warnx(TIRPC_DEBUG_FLAG_XDR, "rpcrdma %x %s:%s\n",
		xid, base, buffer);
	mem_free(buffer, buffered);
}
#endif /* rpcrdma_dump_msg */

/***********************************/
/****** Utilities for buffers ******/
/***********************************/

/* assume lock is taken */
#define XDRMSK_GETBUF(funname, condition, optargs...)		\
static msk_data_t*						\
funname(msk_data_t *bufs, struct mskinfo *mi, ##optargs) {	\
	int i;							\
								\
	do {							\
		for (i=0; i < mi->credits; i++) {		\
			if (condition)				\
				break;				\
		}						\
		if (i != mi->credits)				\
			break;					\
								\
		/* INFO LOG */					\
		__warnx(TIRPC_DEBUG_FLAG_XDR,			\
			#funname ": waiting for buffer\n");	\
		pthread_cond_wait(&mi->cl.cond, &mi->cl.lock); 	\
	} while (1);						\
								\
	return bufs + i;					\
}

/* condition can use:
	loop index int i,
	and function arguments:
	    msk_data_t *buf, struct mskinfo *mi,
	    and whatever optional argument is added (c.f. xid) */
XDRMSK_GETBUF(xdrmsk_getfreebuf, bufs[i].size == 0)
XDRMSK_GETBUF(xdrmsk_getusedbuf, bufs[i].size != 0)
XDRMSK_GETBUF(xdrmsk_getxidbuf,
	      ntohl(((struct rpcrdma_msg *)bufs[i].data)->rm_xid) == xid,
	      u_int32_t xid)
XDRMSK_GETBUF(xdrmsk_getaddrbuf, bufs[i].data == addr, uint8_t *addr)

static int
xdrmsk_countusedbufs(struct mskinfo *mi) {
	int i = 0;
	int count = 0;

	/* do we want to lock? */
	for (; i < mi->credits; i++) {
		if (mi->inbufs[i].size != 0)
			count++;
	}

	return count;
}

/***********************/
/****** Callbacks ******/
/***********************/

static void rpcrdma_signal(msk_trans_t *trans) {
	struct condlock *cl = cl_of_xdrs((XDR*)trans->private_data);
	pthread_mutex_lock(&cl->lock);
	pthread_cond_broadcast(&cl->cond);
	pthread_mutex_unlock(&cl->lock);
}

static void signal_callback(msk_trans_t *trans, msk_data_t *data, void *arg) {
	XDR *xdrs = trans->private_data;
	rpcrdma_signal(trans);
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"received something on trans %p!\n", trans);

	if (callback_of_xdrs(xdrs))
		(callback_of_xdrs(xdrs))(callbackarg_of_xdrs(xdrs));
}

static void setunused_callback(msk_trans_t *trans, msk_data_t *data, void *arg) {
	msk_data_t *prevdata;
	prevdata = NULL;
	do {
		data->size = 0;
		if (prevdata)
			prevdata->next = NULL;
		prevdata = data;
	} while ((data = data->next) != NULL);

	rpcrdma_signal(trans);
}

static void err_callback(msk_trans_t *trans, msk_data_t *data, void *arg) {
	if(trans->state != MSK_CLOSING && trans->state != MSK_CLOSED)
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"error callback on buffer %p\n", data);
}

/**
 * msk_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void msk_wait_callback(struct msk_trans *trans, msk_data_t *data, void *arg) {
	pthread_mutex_t *lock = arg;
	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, lock);
}

/***********************************/
/***** Utilities from Mooshika *****/
/***********************************/

/**
 * msk_post_n_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param data         [OUT] the data buffer to be filled with received data
 * @param num_sge      [IN]  the number of elements in data to register
 * @param callback     [IN]  function that'll be called when done
 * @param err_callback [IN]  function that'll be called on error
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_recv(struct msk_trans *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	struct msk_ctx *rctx;
	int i, ret;

	if (!trans || (trans->state != MSK_CONNECTED && trans->state != MSK_ROUTE_RESOLVED && trans->state != MSK_CONNECT_REQUEST)) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans (%p) state: %d", trans, trans->state);
		return EINVAL;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_RECV, "posting recv");

	i = 0;
	rctx = trans->rctx;
	do {
		if (i == trans->rq_depth) {
			INFO_LOG(trans->debug & MSK_DEBUG_CTX, "Waiting for rctx");
			usleep(250);
			i = 0;
			rctx = trans->rctx;
		}

		while (i < trans->rq_depth && rctx->used != MSK_CTX_FREE) {
			rctx = msk_next_ctx(rctx, trans->max_recv_sge);
			i++;
		}
	} while ( i == trans->rq_depth || !(atomic_bool_compare_and_swap(&rctx->used, MSK_CTX_FREE, MSK_CTX_PENDING)) );
	INFO_LOG(trans->debug & MSK_DEBUG_RECV, "got a free context");

	rctx->callback = callback;
	rctx->err_callback = err_callback;
	rctx->callback_arg = callback_arg;
	rctx->data = data;

	for (i=0; i < num_sge; i++) {
		if (!data || !data->mr) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "You said to recv %d elements (num_sge), but we only found %d! Not requesting.", num_sge, i);
			return EINVAL;
		}
		rctx->sg_list[i].addr = (uintptr_t) data->data;
		INFO_LOG(trans->debug & MSK_DEBUG_RECV, "addr: %lx\n", rctx->sg_list->addr);
		rctx->sg_list[i].length = data->max_size;
		rctx->sg_list[i].lkey = data->mr->lkey;
		if (i != num_sge-1)
			data = data->next;
	}

	rctx->wr.rwr.next = NULL;
	rctx->wr.rwr.wr_id = (uint64_t)rctx;
	rctx->wr.rwr.sg_list = rctx->sg_list;
	rctx->wr.rwr.num_sge = num_sge;

	if (trans->srq)
		ret = ibv_post_srq_recv(trans->srq, &rctx->wr.rwr, &trans->bad_recv_wr);
	else
		ret = ibv_post_recv(trans->qp, &rctx->wr.rwr, &trans->bad_recv_wr);

	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_post_recv failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

static int msk_post_send_generic(struct msk_trans *trans, enum ibv_wr_opcode opcode, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	struct msk_ctx *wctx;
	int i, ret;
	uint32_t totalsize = 0;

	if (!trans || trans->state != MSK_CONNECTED) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans (%p) state: %d", trans, trans->state);
		return EINVAL;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_SEND, "posting a send with op %d", opcode);

	// opcode-specific checks:
	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		if (!rloc) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Cannot do rdma without a remote location!");
			return EINVAL;
		}
	} else if (opcode == IBV_WR_SEND || opcode == IBV_WR_SEND_WITH_IMM) {
	} else {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unsupported op code: %d", opcode);
		return EINVAL;
	}

	i = 0;
	wctx = trans->wctx;
	do {
		if (i == trans->sq_depth) {
			INFO_LOG(trans->debug & MSK_DEBUG_CTX, "waiting for wctx");
			usleep(250);
			i = 0;
			wctx = trans->wctx;
		}

		while (i < trans->sq_depth && wctx->used != MSK_CTX_FREE) {
			wctx = msk_next_ctx(wctx, trans->max_send_sge);
			i++;
		}
	} while ( i == trans->sq_depth || !(atomic_bool_compare_and_swap(&wctx->used, MSK_CTX_FREE, MSK_CTX_PENDING)) );
	INFO_LOG(trans->debug & MSK_DEBUG_SEND, "got a free context");

	wctx->callback = callback;
	wctx->err_callback = err_callback;
	wctx->callback_arg = callback_arg;
	wctx->data = data;

	for (i=0; i < num_sge; i++) {
		if (!data || !data->mr) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "You said to send %d elements (num_sge), but we only found %d! Not sending.", num_sge, i);
			// or send up to previous one? It's probably an error though...
			return EINVAL;
		}
		if (data->size == 0) {
			num_sge = i; // only send up to previous sg, do we want to warn about this?
			break;
		}

		wctx->sg_list[i].addr = (uintptr_t)data->data;
		INFO_LOG(trans->debug & MSK_DEBUG_SEND, "addr: %lx\n", wctx->sg_list[i].addr);
		wctx->sg_list[i].length = data->size;
		wctx->sg_list[i].lkey = data->mr->lkey;
		totalsize += data->size;

		if (i != num_sge-1)
			data = data->next;
	}

	if (rloc && totalsize > rloc->size) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "trying to send or read a buffer bigger than the remote buffer (shall we truncate?)");
		return EMSGSIZE;
	}

	wctx->wr.wwr.next = NULL;
	wctx->wr.wwr.wr_id = (uint64_t)wctx;
	wctx->wr.wwr.opcode = opcode;
//FIXME	wctx->wr.wwr.imm_data = htonl(data->imm_data);
	wctx->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	wctx->wr.wwr.sg_list = wctx->sg_list;
	wctx->wr.wwr.num_sge = num_sge;
	if (rloc) {
		wctx->wr.wwr.wr.rdma.rkey = rloc->rkey;
		wctx->wr.wwr.wr.rdma.remote_addr = rloc->raddr;
	}

	ret = ibv_post_send(trans->qp, &wctx->wr.wwr, &trans->bad_send_wr);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_post_send failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * Post a send buffer.
 *
 * @param trans        [IN]
 * @param data         [IN] the data buffer to be sent
 * @param num_sge      [IN] the number of elements in data to send
 * @param callback     [IN] function that'll be called when done
 * @param err_callback [IN] function that'll be called on error
 * @param callback_arg [IN] argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_send(struct msk_trans *trans, msk_data_t *data, int num_sge, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_SEND, data, num_sge, NULL, callback, err_callback, callback_arg);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param trans   [IN]
 * @param data    [OUT] the data buffer to be filled with the received data
 * @param num_sge [IN]  the number of elements in data to register
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_recv(struct msk_trans *trans, msk_data_t *data, int num_sge) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
	ret = msk_post_n_recv(trans, data, num_sge, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans   [IN]
 * @param data    [IN] the data to send
 * @param num_sge [IN] the number of elements in data to send
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_send(struct msk_trans *trans, msk_data_t *data, int num_sge) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
	ret = msk_post_n_send(trans, data, num_sge, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

// callbacks would all be run in a big send/recv_thread


// server specific:


int msk_post_n_read(struct msk_trans *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_READ, data, num_sge, rloc, callback, err_callback, callback_arg);
}

int msk_post_n_write(struct msk_trans *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_WRITE, data, num_sge, rloc, callback, err_callback, callback_arg);
}

int msk_wait_n_read(struct msk_trans *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
	ret = msk_post_n_read(trans, data, num_sge, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


int msk_wait_n_write(struct msk_trans *trans, msk_data_t *data, int num_sge, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
	ret = msk_post_n_write(trans, data, num_sge, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


struct sockaddr *msk_get_dst_addr(struct msk_trans *trans) {
	return rdma_get_peer_addr(trans->cm_id);
}

struct sockaddr *msk_get_src_addr(struct msk_trans *trans) {
	return rdma_get_local_addr(trans->cm_id);
}

uint16_t msk_get_src_port(struct msk_trans *trans) {
	return rdma_get_src_port(trans->cm_id);
}

uint16_t msk_get_dst_port(struct msk_trans *trans) {
	return rdma_get_dst_port(trans->cm_id);
}

/***********************************/
/****** Utilities for rpcrdma ******/
/***********************************/

#define rc(ptr) ((struct rpcrdma_read_chunk*)ptr)
#define wa(ptr) ((struct rpcrdma_write_array*)ptr)
#define wc(ptr) ((struct rpcrdma_write_chunk*)ptr)
static inline void rpcrdma_skip_read_list(uint32_t **pptr) {
	while (rc(*pptr)->rc_discrim) {
		*pptr += sizeof(struct rpcrdma_read_chunk)/sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void rpcrdma_skip_write_array(uint32_t **pptr) {
	if (wa(*pptr)->wc_discrim) {
		*pptr += (sizeof(struct rpcrdma_write_array) + sizeof(struct rpcrdma_write_chunk) * ntohl(wa(*pptr)->wc_nchunks))/sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void rpcrdma_skip_reply_array(uint32_t **pptr) {
	if (wa(*pptr)->wc_discrim) {
		*pptr += (sizeof(struct rpcrdma_write_array) + sizeof(struct rpcrdma_write_chunk) * ntohl(wa(*pptr)->wc_nchunks))/sizeof(**pptr);
	} else {
		(*pptr)++;
	}
}

static inline struct rpcrdma_read_chunk *rpcrdma_get_read_list(struct rpcrdma_msg *rmsg) {
	return rc(rmsg->rm_body.rm_chunks);
}

/*static inline struct rpcrdma_write_array *rpcrdma_get_write_array(struct rpcrdma_msg *rmsg) {
	uint32_t *ptr = rmsg->rm_body.rm_chunks;

	rpcrdma_skip_read_list(&ptr);

	return wa(ptr);
}*/

static inline struct rpcrdma_write_array *rpcrdma_get_reply_array(struct rpcrdma_msg *rmsg) {
	uint32_t *ptr = rmsg->rm_body.rm_chunks;

	rpcrdma_skip_read_list(&ptr);
	rpcrdma_skip_write_array(&ptr);

	return wa(ptr);
}

static inline uint32_t *rpcrdma_skip_header(struct rpcrdma_msg *rmsg) {
	uint32_t *ptr = rmsg->rm_body.rm_chunks;

	rpcrdma_skip_read_list(&ptr);
	rpcrdma_skip_write_array(&ptr);
	rpcrdma_skip_reply_array(&ptr);

	return ptr;
}

static inline uint32_t rpcrdma_get_call(struct rpcrdma_msg *rmsg, char **call_p) {

	assert(call_p != NULL);

	*call_p = (char*)rpcrdma_skip_header(rmsg);

	return ((uint64_t)(*call_p) - (uint64_t)rmsg);
}

static inline uint32_t rpcrdma_hdr_len(struct rpcrdma_msg *rmsg) {
	uint32_t *ptr = rpcrdma_skip_header(rmsg);

	return ((uint64_t)ptr - (uint64_t)rmsg);
}

static inline void rpcrdma_rloc_from_segment(msk_rloc_t *rloc, struct rpcrdma_segment *seg) {
	rloc->rkey = ntohl(seg->rs_handle);
	rloc->size = ntohl(seg->rs_length);
	rloc->raddr = xdr_decode_hyper(&seg->rs_offset);
}

#if 0
int svc_rdma_xdr_encode_error(struct svcxprt_rdma *xprt,
			      struct rpcrdma_msg *rmsgp,
			      enum rpcrdma_errcode err, u32 *va)
{
	u32 *startp = va;

	*va++ = htonl(rmsgp->rm_xid);
	*va++ = htonl(rmsgp->rm_vers);
	*va++ = htonl(xprt->sc_max_requests);
	*va++ = htonl(RDMA_ERROR);
	*va++ = htonl(err);
	if (err == ERR_VERS) {
		*va++ = htonl(RPCRDMA_VERSION);
		*va++ = htonl(RPCRDMA_VERSION);
	}

	return (int)((unsigned long)va - (unsigned long)startp);
}

void svc_rdma_xdr_encode_reply_array(struct rpcrdma_write_array *ary,
				 int chunks)
{
	ary->wc_discrim = xdr_one;
	ary->wc_nchunks = htonl(chunks);
}

void svc_rdma_xdr_encode_array_chunk(struct rpcrdma_write_array *ary,
				     int chunk_no,
				     u32 rs_handle, u64 rs_offset,
				     u32 write_len)
{
	struct rpcrdma_segment *seg = &ary->wc_array[chunk_no].wc_target;
	seg->rs_handle = htonl(rs_handle);
	seg->rs_length = htonl(write_len);
	xdr_encode_hyper((u32 *) &seg->rs_offset, rs_offset);
}

void svc_rdma_xdr_encode_reply_header(struct svcxprt_rdma *xprt,
				  struct rpcrdma_msg *rdma_argp,
				  struct rpcrdma_msg *rdma_resp,
				  enum rpcrdma_proc rdma_type)
{
	rdma_resp->rm_xid = htonl(rdma_argp->rm_xid);
	rdma_resp->rm_vers = htonl(rdma_argp->rm_vers);
	rdma_resp->rm_credit = htonl(xprt->sc_max_requests);
	rdma_resp->rm_type = htonl(rdma_type);

	/* Encode <nul> chunks lists */
	rdma_resp->rm_body.rm_chunks[0] = xdr_zero;
	rdma_resp->rm_body.rm_chunks[1] = xdr_zero;
	rdma_resp->rm_body.rm_chunks[2] = xdr_zero;
}

#endif /* 0 */

/****************************/
/****** Main functions ******/
/****************************/

/*ARGSUSED*/
static void
xdrmsk_destroy(XDR *xdrs)
{
	struct mskinfo *mi;
	if (xdrs->x_private) {
		mi = xdrs->x_private;
		if (mi->mr) {
			msk_dereg_mr(mi->mr);
			mi->mr = NULL;
		}
		if (mi->inbufs) {
			mem_free(mi->inbufs, sizeof(msk_data_t)*mi->credits);
			mi->inbufs = NULL;
		}
		if (mi->outbufs) {
			mem_free(mi->outbufs, sizeof(msk_data_t)*mi->credits);
			mi->outbufs = NULL;
		}
		if (mi->hdrbufs) {
			mem_free(mi->hdrbufs, sizeof(msk_data_t)*mi->credits);
			mi->hdrbufs = NULL;
		}
		if (mi->rdmabufs) {
			mem_free(mi->rdmabufs, sizeof(msk_data_t)*mi->credits);
			mi->rdmabufs = NULL;
		}
		if (mi->mskbase) {
			mem_free(mi->mskbase, (mi->sendsz + mi->recvsz) * mi->credits);
			mi->mskbase = NULL;
		}
		mem_free(xdrs->x_private, sizeof(struct mskinfo));
		xdrs->x_private = NULL;
	}
}

/*
 * The procedure xdrmsk_create initializes a stream descriptor for a
 * memory buffer.
 */
int
xdrmsk_create(XDR *xdrs,
	      msk_trans_t *trans,
	      u_int sendsz,
	      u_int recvsz,
	      u_int credits,
	      void (*callback)(void*),
	      void *callbackarg)
{
	int i;
	struct mskinfo *mi;

	xdrs->x_private = mem_alloc(sizeof(struct mskinfo));
	if (xdrs->x_private == NULL)
		goto err;

	mi = priv(xdrs);

	mi->mskbase = mem_alloc((sendsz+2*recvsz+HDR_MAX_SIZE)*credits);
	if (mi->mskbase == NULL)
		goto err;

	mi->sendsz = sendsz;
	mi->recvsz = recvsz;
	mi->credits = credits;
	mi->trans = trans;
	mi->callback = callback;
	mi->callbackarg = callbackarg;

	//TODO: alloc 'em all in a single mem_alloc?
	mi->inbufs = mem_alloc(sizeof(msk_data_t)*mi->credits);
	if (!mi->inbufs)
		goto err;

	mi->outbufs = mem_alloc(sizeof(msk_data_t)*mi->credits);
	if (!mi->outbufs)
		goto err;

	mi->hdrbufs = mem_alloc(sizeof(msk_data_t)*mi->credits);
	if (!mi->hdrbufs)
		goto err;

	mi->rdmabufs = mem_alloc(sizeof(msk_data_t)*mi->credits);
	if (!mi->rdmabufs)
		goto err;

	trans->private_data = xdrs;

	mi->mr = msk_reg_mr(mi->trans, mi->mskbase, credits*(mi->sendsz + 2*mi->recvsz + HDR_MAX_SIZE),
			    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ); // register it in two chunks for read and write?

	pthread_cond_init(&mi->cl.cond, NULL);
	pthread_mutex_init(&mi->cl.lock, NULL);

	/* init data buffers (note that setting size to zero is important) and post recv buffers */
	for (i=0; i < credits; i++) {
		mi->outbufs[i].max_size = sendsz;
		mi->outbufs[i].size = 0;
		mi->outbufs[i].data = mi->mskbase + i*sendsz;
		mi->outbufs[i].mr = mi->mr;
		mi->hdrbufs[i].max_size = HDR_MAX_SIZE;
		mi->hdrbufs[i].size = 0;
		mi->hdrbufs[i].data = mi->mskbase + credits*sendsz + i*HDR_MAX_SIZE;
		mi->hdrbufs[i].mr = mi->mr;
		mi->rdmabufs[i].max_size = recvsz;
		mi->rdmabufs[i].size = 0;
		mi->rdmabufs[i].data = mi->mskbase + credits*(sendsz + HDR_MAX_SIZE) + i * recvsz;
		mi->rdmabufs[i].mr = mi->mr;
		mi->inbufs[i].max_size = recvsz;
		mi->inbufs[i].size = 0;
		mi->inbufs[i].data = mi->mskbase + credits*(sendsz + HDR_MAX_SIZE + recvsz) + i*recvsz;
		mi->inbufs[i].mr = mi->mr;
		msk_post_recv(mi->trans, mi->inbufs + i, signal_callback, err_callback, NULL);
	}

	mi->pos = xdrs->x_base = NULL;
	xdrs->x_handy = 0;

	/* set the op to free since we don't know what to do until we get our first recv/send anyway. Let's hope that's ok */
	xdrs->x_op = XDR_FREE;

	return 0;

err:
	__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: out of memory", __func__);
	xdrmsk_destroy(xdrs);
	return ENOMEM;
}

/* returns length available to use */
int
rpcrdma_clnt_setbuf(XDR *xdrs, u_int32_t xid, enum xdr_op op) {
	int i;
	struct mskinfo *mi;
	struct rpcrdma_write_array *reply_array;
	msk_data_t *prev_buf, *tmp_buf;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: no xdrs?", __func__);
		return 0;
	}

	mi = priv(xdrs);

	/* free old buffers */
	switch(xdrs->x_op) {
	    case XDR_ENCODE:
		/* FIXME: only clears the ones associated with last request */
		for(i=0; i<mi->credits; i++) {
			mi->rdmabufs[i].size = 0;
			mi->rdmabufs[i].next = NULL;
		}
		break;
	    case XDR_DECODE:
		mi->curbuf->size = 0; // set this for countusedbuf
		msk_post_recv(mi->trans, mi->curbuf, signal_callback, err_callback, NULL);
		break;
	    default:
		break;
	}

	xdrs->x_op = op;

	/* get new buffer */
	switch (xdrs->x_op) {
	// Client encodes a call request
	case XDR_ENCODE:
		pthread_mutex_lock(&mi->cl.lock);
		mi->xdrbuf = mi->curbuf = xdrmsk_getfreebuf(mi->outbufs, mi);

		mi->curbuf->size = 1; // just saying it's used.

		xdrs->x_handy = mi->curbuf->max_size;
		mi->pos = xdrs->x_base = (char*)mi->curbuf->data;
		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

		pthread_mutex_unlock(&mi->cl.lock);

		break;

	// Client decodes a reply buffer
	case XDR_DECODE:
		pthread_mutex_lock(&mi->cl.lock);

		mi->curbuf = xdrmsk_getxidbuf(mi->inbufs, mi, xid);

		rpcrdma_dump_msg(mi->curbuf, "clntrplyhdr", ntohl(xid));

		//handy = length, base = address
		reply_array = rpcrdma_get_reply_array((struct rpcrdma_msg*)mi->curbuf->data);
		if (reply_array->wc_discrim == 0) {
		        __warnx(TIRPC_DEBUG_FLAG_XDR, "No reply/read array, failing miserably till writes/inlines are handled");
			return 0;
		} else {
			prev_buf = NULL;
			for (i=0; i < ntohl(reply_array->wc_nchunks); i++) {
				/* FIXME: xdrmsk_getaddrbuf hangs instead of failing if no match. add a zero timeout when implemented */
				tmp_buf = xdrmsk_getaddrbuf(mi->rdmabufs, mi, (uint8_t*) xdr_decode_hyper(&reply_array->wc_array[i].wc_target.rs_offset));

				/* rs_length < max_size if the protocol works out... FIXME: check anyway? */
				tmp_buf->size = ntohl(reply_array->wc_array[i].wc_target.rs_length);

				rpcrdma_dump_msg(tmp_buf, "clntrplybody", ntohl(xid));

				if (prev_buf)
					prev_buf->next = tmp_buf;
				else
					mi->xdrbuf = tmp_buf;

				prev_buf = tmp_buf;
			}
		}

		pthread_mutex_unlock(&mi->cl.lock);

		xdrs->x_handy = mi->xdrbuf->size;
		mi->pos = xdrs->x_base = (char*)mi->xdrbuf->data;

		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

		break;

	// XDR_FREE
	default:
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: unknown op", __func__);
	}

	return xdrs->x_handy;
}

int
rpcrdma_svc_setbuf(XDR *xdrs, u_int32_t xid, enum xdr_op op) {

	int i;
	struct mskinfo *mi;
	struct rpcrdma_write_array *call_array;
	msk_data_t *prev_buf, *tmp_buf;
	struct rpcrdma_read_chunk *read_chunk;
	msk_rloc_t rloc;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: no xdrs?", __func__);
		return -1;
	}

	mi = priv(xdrs);

	/* free old buffers */
	switch(xdrs->x_op) {
	    case XDR_DECODE:
		break;
	    case XDR_ENCODE:
		break;
	    default:
		break;
	}

	xdrs->x_op = op;

	/* get new buffer */
	switch (xdrs->x_op) {
	// Server decodes a call request
	case XDR_DECODE:
		pthread_mutex_lock(&mi->cl.lock);
		mi->callbuf = mi->curbuf = xdrmsk_getusedbuf(mi->inbufs, mi);

		pthread_mutex_unlock(&mi->cl.lock);

		rpcrdma_dump_msg(mi->curbuf, "call", ((struct rpcrdma_msg *)mi->curbuf->data)->rm_xid);

		read_chunk = rpcrdma_get_read_list((struct rpcrdma_msg*)mi->curbuf->data);
		prev_buf = NULL;
		while (read_chunk->rc_discrim != 0) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"got something to read :D\n");

			tmp_buf = xdrmsk_getfreebuf(mi->rdmabufs, mi);

			tmp_buf->size = ntohl(read_chunk->rc_target.rs_length);
			rpcrdma_rloc_from_segment(&rloc, &read_chunk->rc_target);
			//FIXME: get them only when needed in xdrmsk_getnextbuf or at least post all the reads and wait only at the end...
			msk_wait_read(mi->trans, tmp_buf, &rloc);

			rpcrdma_dump_msg(tmp_buf, "svcreaddata", ((struct rpcrdma_msg *)mi->curbuf->data)->rm_xid);

			if (prev_buf)
				prev_buf->next = tmp_buf;
			else
				mi->curbuf->next = tmp_buf;

			prev_buf = tmp_buf;
			read_chunk++;
		}

		i = rpcrdma_get_call((struct rpcrdma_msg *)mi->curbuf->data, (char **)&xdrs->x_base);
		if (i != 0) {
			//handy = length, base = address
			mi->pos = xdrs->x_base;
			xdrs->x_handy = mi->curbuf->size - i; //FIXME: check this matches read_chunk position
			mi->xdrbuf = mi->curbuf;
		}

		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

		break;

	// Server encodes a reply
	case XDR_ENCODE:
		call_array = rpcrdma_get_reply_array((struct rpcrdma_msg*)mi->callbuf->data);

		pthread_mutex_lock(&mi->cl.lock);

		if (call_array->wc_discrim == 0) {
			// no reply array to write to, replying inline an' hope it works (OK on RPC/RDMA Read)
			mi->curbuf = xdrmsk_getfreebuf(mi->outbufs, mi);
			mi->curbuf->size = mi->curbuf->max_size;
			xdrs->x_handy = mi->curbuf->size;
			mi->pos = xdrs->x_base = (char*)mi->curbuf->data;
			mi->xdrbuf = mi->curbuf;
			xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
			    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;
		} else {
			prev_buf = NULL;

			if (ntohl(call_array->wc_nchunks) != 1) {
				__warnx(TIRPC_DEBUG_FLAG_XDR,
					"More than one chunk in list\n");
			}

			for (i=0; i < ntohl(call_array->wc_nchunks); i++) {
				tmp_buf = xdrmsk_getfreebuf(mi->outbufs, mi);
				tmp_buf->size = MIN(tmp_buf->max_size, ntohl(call_array->wc_array[i].wc_target.rs_length));
				if (prev_buf)
					prev_buf->next = tmp_buf;
				else
					mi->curbuf = tmp_buf; /* that's the first of the list, we'll use it first */

				prev_buf = tmp_buf;
			}
		}

		xdrs->x_handy = mi->curbuf->size;
		mi->pos = xdrs->x_base = (char*)mi->curbuf->data;
		mi->xdrbuf = mi->curbuf;
		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

		pthread_mutex_unlock(&mi->cl.lock);

		break;

	default:
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: unknown op", __func__);
	}
	return 0;
}

/* true is message sent, false otherwise */
bool
rpcrdma_clnt_flushout(XDR * xdrs) {
/* FIXME: decide how many buffers we use in argument!!!!!! */
#define num_chunks (mi->credits-1)

	int i;
	struct rpc_msg *msg = (struct rpc_msg*)xdrs->x_base;
	msk_data_t *hdr_buf, *rdma_buf, *prev_buf, *tmp_buf;
	struct mskinfo *mi;
	struct rpcrdma_msg *rmsg;
	struct rpcrdma_write_array *w_array;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: no xdrs?", __func__);
		return 0;
	}

	mi = priv(xdrs);

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		pthread_mutex_lock(&mi->cl.lock);
		hdr_buf = xdrmsk_getfreebuf(mi->hdrbufs, mi);
		hdr_buf->size = 1; // just say it's taken, will be updated later

		prev_buf = NULL;
		for (i=0; i<num_chunks; i++) {
			tmp_buf = xdrmsk_getfreebuf(mi->rdmabufs, mi);
			tmp_buf->size = tmp_buf->max_size; // just say it's taken
			if (prev_buf)
				prev_buf->next = tmp_buf;
			else
				rdma_buf = tmp_buf;

			prev_buf = tmp_buf;
		}

		pthread_mutex_unlock(&mi->cl.lock);

		rmsg = (struct rpcrdma_msg*)hdr_buf->data;
		rmsg->rm_xid = msg->rm_xid;
		rmsg->rm_vers = htonl(1);
		rmsg->rm_credit = htonl(mi->credits - xdrmsk_countusedbufs(mi));
		rmsg->rm_type = htonl(RDMA_MSG);

		/* no read, write chunks. */
		rmsg->rm_body.rm_chunks[0] = htonl(0);
		rmsg->rm_body.rm_chunks[1] = htonl(0);

		/* reply chunk */
		w_array = (struct rpcrdma_write_array*)&rmsg->rm_body.rm_chunks[2];
		w_array->wc_discrim = htonl(1);
		w_array->wc_nchunks = htonl(num_chunks);

		for (i=0; i<num_chunks; i++) {
			w_array->wc_array[i].wc_target.rs_handle = htonl(mi->mr->rkey);
			w_array->wc_array[i].wc_target.rs_length = htonl(rdma_buf->size);
			xdr_encode_hyper((uint32_t*)&w_array->wc_array[i].wc_target.rs_offset, (uint64_t)rdma_buf->data);
			rdma_buf = rdma_buf->next;
		}

		hdr_buf->size = rpcrdma_hdr_len(rmsg);
		hdr_buf->next = mi->curbuf;
		mi->curbuf->size = xdrmsk_getpos(xdrs);

		rpcrdma_dump_msg(hdr_buf, "clntcall", msg->rm_xid);
		rpcrdma_dump_msg(mi->curbuf, "clntcall", msg->rm_xid);

		/* actual send, callback will take care of cleanup */
		msk_post_n_send(mi->trans, hdr_buf, 2, setunused_callback, err_callback, hdr_buf);
		break;
	    case REPLY:
		break;
	}

	return TRUE;
}

bool
rpcrdma_svc_flushout(XDR * xdrs) {
	struct rpc_msg *msg;
	int i;
	struct mskinfo *mi;
	struct rpcrdma_msg *rmsg;
	struct rpcrdma_write_array *w_array;
	struct rpcrdma_write_array *call_array;
	msk_data_t *prev_buf, *hdr_buf;
	msk_rloc_t rloc;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_XDR, "%s: no xdrs?", __func__);
		return 0;
	}

	mi = priv(xdrs);
	msg = (struct rpc_msg*)mi->curbuf->data;

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		break;
	    case REPLY:
		pthread_mutex_lock(&mi->cl.lock);
		hdr_buf = xdrmsk_getfreebuf(mi->hdrbufs, mi);
		hdr_buf->size = 1; // just say it's taken, will be updated later
		pthread_mutex_unlock(&mi->cl.lock);

		/* CHECKS HERE */

		call_array = rpcrdma_get_reply_array((struct rpcrdma_msg*)mi->callbuf->data);

		/* build the header that goes with the data as we post it for writes */
		rmsg = (struct rpcrdma_msg*)hdr_buf->data;
		rmsg->rm_xid = msg->rm_xid;				/* TODO: check it matches mi->hdrbuf xid */
		rmsg->rm_vers = htonl(1);
		rmsg->rm_credit = htonl(mi->credits - xdrmsk_countusedbufs(mi));

		/* no read, write chunks. */
		rmsg->rm_body.rm_chunks[0] = htonl(0);
		rmsg->rm_body.rm_chunks[1] = htonl(0);

		if (call_array->wc_discrim == 0) {
			rmsg->rm_type = htonl(RDMA_MSG);

			/* no reply chunk either */
			rmsg->rm_body.rm_chunks[2] = htonl(0);

			hdr_buf->next = mi->curbuf;
			hdr_buf->size = rpcrdma_hdr_len(rmsg);

			mi->xdrbuf->size = xdrmsk_getpos(xdrs);

			rpcrdma_dump_msg(hdr_buf, "rplyhdr", msg->rm_xid);
			rpcrdma_dump_msg(mi->curbuf, "rplybody", msg->rm_xid);

			/* actual send, callback will take care of cleanup */
			/* TODO: make it work with not just one outbuf, but get more outbufs as needed in xdrmsk_getnextbuf?
			   add something in mi instead of checking call_array->wc_discrim everytime... */
			msk_post_n_send(mi->trans, hdr_buf, 2, setunused_callback, err_callback, hdr_buf);

		} else {
			rmsg->rm_type = htonl(RDMA_NOMSG);

			/* reply chunk */
			w_array = (struct rpcrdma_write_array*)&rmsg->rm_body.rm_chunks[2];
			w_array->wc_discrim = htonl(1);

			i = 0;
			while (i < ntohl(call_array->wc_nchunks)) {
				/* This is checked in msk_post_write already
				if (mi->curbuf->size < ntohl(call_array->wc_array[i].wc_target.rs_length))
					return FALSE; */

				rpcrdma_rloc_from_segment(&rloc, &call_array->wc_array[i].wc_target);
				mi->xdrbuf->size = xdrmsk_getpos(xdrs);

				/** @todo: check if remote addr + size = next remote addr and send in a single write if so */

				msk_wait_write(mi->trans, mi->curbuf, &rloc); /* FIXME: change to msk_post_write and wait somehow */

				w_array->wc_array[i].wc_target.rs_handle = call_array->wc_array[i].wc_target.rs_handle;
				w_array->wc_array[i].wc_target.rs_length = htonl(mi->curbuf->size);
				w_array->wc_array[i].wc_target.rs_offset = call_array->wc_array[i].wc_target.rs_offset;

				rpcrdma_dump_msg(mi->curbuf, "rplybody", msg->rm_xid);

				mi->curbuf->size = 0; /* FIXME: _not_ MT-safe without lock */

				if (mi->curbuf == mi->xdrbuf)
					break;

				i++;
				prev_buf = mi->curbuf;
				mi->curbuf = mi->curbuf->next; /* FIXME: this is safe if it was constructed properly... check anyway? */
				prev_buf->next = NULL;
			}

			while (mi->curbuf->next) {
				prev_buf = mi->curbuf;
				mi->curbuf = mi->curbuf->next;
				prev_buf->next = NULL;
				mi->curbuf->size = 0;
			}

			w_array->wc_nchunks = htonl(i+1);

			hdr_buf->size = rpcrdma_hdr_len(rmsg);

			rpcrdma_dump_msg(hdr_buf, "rplyhdr", msg->rm_xid);

			/* actual send, callback will take care of cleanup */
			msk_post_send(mi->trans, hdr_buf, setunused_callback, err_callback, hdr_buf);

		}

		/* free the old inbuf we only kept for header, and repost it. */
		mi->callbuf->size = 0; // set this for countusedbuf
		msk_post_recv(mi->trans, mi->callbuf, signal_callback, err_callback, NULL);

		break;
	}

	return TRUE;
}

static u_int
xdrmsk_getpos(XDR *xdrs)
{
	/* XXX w/64-bit pointers, u_int not enough! */
	return (u_int)((u_long)priv(xdrs)->pos - (u_long)xdrs->x_base);
}

/* FIXME: Completely broken with buffer cut in parts... */
static bool
xdrmsk_setpos(XDR *xdrs, u_int pos)
{
	char *newaddr = xdrs->x_base + pos;
	char *lastaddr = (char *)priv(xdrs)->pos + xdrs->x_handy;

	if (newaddr > lastaddr)
		return (FALSE);
	priv(xdrs)->pos = newaddr;
	xdrs->x_handy = (u_int)(lastaddr - newaddr); /* XXX sizeof(u_int) <? sizeof(ptrdiff_t) */
	return (TRUE);
}

static bool
xdrmsk_getnextbuf(XDR *xdrs)
{
	msk_data_t *data;

	if (priv(xdrs)->xdrbuf == NULL)
		return FALSE;

	data = priv(xdrs)->xdrbuf;

	if (data->next == NULL)
		return FALSE;

	data->size = xdrmsk_getpos(xdrs);

	data = priv(xdrs)->xdrbuf = priv(xdrs)->xdrbuf->next;
	xdrs->x_handy = data->size;
	xdrs->x_base = priv(xdrs)->pos = (char*)data->data;
	xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
	    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

	return TRUE;
}

static uint32_t
xdrmsk_getsizeleft(XDR *xdrs)
{
	uint32_t count = xdrs->x_handy;
	msk_data_t *data = priv(xdrs)->xdrbuf;

	while (data->next) {
		data=data->next;
		count += data->size;
	}

	return count;
}

static bool
xdrmsk_getlong_aligned(XDR *xdrs, long *lp)
{

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_getlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	*lp = ntohl(*(u_int32_t *)priv(xdrs)->pos);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_putlong_aligned(XDR *xdrs, const long *lp)
{

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_putlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	*(u_int32_t *)priv(xdrs)->pos = htonl((u_int32_t)*lp);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_getlong_unaligned(XDR *xdrs, long *lp)
{
	u_int32_t l;

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_getlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	memmove(&l, priv(xdrs)->pos, sizeof(int32_t));
	*lp = ntohl(l);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_putlong_unaligned(XDR *xdrs, const long *lp)
{
	u_int32_t l;

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_putlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	l = htonl((u_int32_t)*lp);
	memmove(priv(xdrs)->pos, &l, sizeof(int32_t));
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_getbytes(XDR *xdrs, char *addr, u_int len)
{
	u_int size;

	if (xdrmsk_getsizeleft(xdrs) < len)
			return (FALSE);

	while (len > 0) {
		if (xdrs->x_handy == 0)
			if (xdrmsk_getnextbuf(xdrs) == FALSE)
				return (FALSE);
		size = MIN(len, xdrs->x_handy);
		memmove(addr, priv(xdrs)->pos, size);
		addr += size;
		len -= size;
		xdrs->x_handy -= size;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + size;
	}
	return (TRUE);
}

static bool
xdrmsk_putbytes(XDR *xdrs, const char *addr, u_int len)
{
	u_int size;

	if (xdrmsk_getsizeleft(xdrs) < len)
		return (FALSE);

	while (len > 0) {
		if (xdrs->x_handy == 0)
			if (xdrmsk_getnextbuf(xdrs) == FALSE)
				return (FALSE);
		size = MIN(len, xdrs->x_handy);
		memmove(priv(xdrs)->pos, addr, size);
		addr += size;
		len -= size;
		xdrs->x_handy -= size;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + size;
	}
	return (TRUE);
}

static int32_t *
xdrmsk_inline_aligned(XDR *xdrs, u_int len)
{
	int32_t *buf = NULL;

	if (xdrs->x_handy >= len) {
		xdrs->x_handy -= len;
		buf = (int32_t *)priv(xdrs)->pos;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + len;
	} else {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_inline(xdrs, len);
		else
			return NULL;
	}
	return (buf);
}

/* ARGSUSED */
static int32_t *
xdrmsk_inline_unaligned(XDR *xdrs, u_int len)
{
	return (NULL);
}
