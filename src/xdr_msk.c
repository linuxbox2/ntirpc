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

#define MSK_DUMP_FRAGMENTS 1

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
		printf(#funname " - waiting for buffer\n");     \
		pthread_cond_wait(&mi->cl.cond, &mi->cl.lock); 	\
	} while (1);						\
								\
	return bufs + i;						\
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
	//do we want to lock?
	int i, count;
	count = 0;
	for (i=0; i < mi->credits; i++) {
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
	printf("received something on trans %p!\n", trans);

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
		printf("error callback on buffer %p\n", data);
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

#ifdef MSK_DUMP_FRAGMENTS
static void rpcrdma_dump_msg(msk_data_t *data, char *filebase, uint32_t xid) {
	char filename[255];
	sprintf(filename, "/tmp/rpcrdma-%x-%s", xid, filebase);
	FILE *fd = fopen(filename, "a");

	fwrite(data->data, data->size, 1, fd);
	fwrite("\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11", 0x10, 1, fd);

	fflush(fd);

	fclose(fd);
}
#endif



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

#endif // 0



/****************************/
/****** Main functions ******/
/****************************/



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

#ifdef MSK_DUMP_FRAGMENTS
		rpcrdma_dump_msg(mi->curbuf, "clntrplyhdr", ntohl(xid));
#endif
 
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

#ifdef MSK_DUMP_FRAGMENTS
				rpcrdma_dump_msg(tmp_buf, "clntrplybody", ntohl(xid));
#endif

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

#ifdef MSK_DUMP_FRAGMENTS
		rpcrdma_dump_msg(mi->curbuf, "call", ((struct rpcrdma_msg *)mi->curbuf->data)->rm_xid);
#endif

		read_chunk = rpcrdma_get_read_list((struct rpcrdma_msg*)mi->curbuf->data);
		prev_buf = NULL;
		while (read_chunk->rc_discrim != 0) {
			printf("got something to read :D\n");

			tmp_buf = xdrmsk_getfreebuf(mi->rdmabufs, mi);

			tmp_buf->size = ntohl(read_chunk->rc_target.rs_length);
			rpcrdma_rloc_from_segment(&rloc, &read_chunk->rc_target);
			//FIXME: get them only when needed in xdrmsk_getnextbuf or at least post all the reads and wait only at the end...
			msk_wait_read(mi->trans, tmp_buf, &rloc);

#ifdef MSK_DUMP_FRAGMENTS
			rpcrdma_dump_msg(tmp_buf, "svcreaddata", ((struct rpcrdma_msg *)mi->curbuf->data)->rm_xid);
#endif


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
				printf("More than one chunk in list\n");
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

#ifdef MSK_DUMP_FRAGMENTS
		rpcrdma_dump_msg(hdr_buf, "clntcall", msg->rm_xid);
		rpcrdma_dump_msg(mi->curbuf, "clntcall", msg->rm_xid);
#endif

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


#ifdef MSK_DUMP_FRAGMENTS
			rpcrdma_dump_msg(hdr_buf, "rplyhdr", msg->rm_xid);
			rpcrdma_dump_msg(mi->curbuf, "rplybody", msg->rm_xid);
#endif

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


#ifdef MSK_DUMP_FRAGMENTS
				rpcrdma_dump_msg(mi->curbuf, "rplybody", msg->rm_xid);
#endif
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

#ifdef MSK_DUMP_FRAGMENTS
			rpcrdma_dump_msg(hdr_buf, "rplyhdr", msg->rm_xid);
#endif

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
