/*
 * Copyright (c) 2003-2007 Network Appliance, Inc. All rights reserved.
 *
 * BSD-type license:
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
 *	from: @(#)rpc_msg.h 1.7 86/07/16 SMI
 *	from: @(#)rpc_msg.h	2.1 88/07/29 4.0 RPCSRC
 * $FreeBSD: src/include/rpc/rpc_msg.h,v 1.15 2003/01/01 18:48:42 schweikh Exp $
 */

/*
 * rpc_rdma.h
 * rpc rdma message definition
 *
 * Copyright (c) 2003-2007 Network Appliance, Inc. All rights reserved.
 */

#ifndef _TIRPC_RPC_RDMA_H
#define _TIRPC_RPC_RDMA_H

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
	bool_t frag_sent;	/* true if buffer sent in middle of record */
	/*
	 * in-coming bits
	 */
	msk_data_t *in_buffers;
	u_long in_size;	/* fixed size of the input buffer */
	char *in_base;
	char *in_finger;	/* location of next byte to be had */
	char *in_boundry;	/* can read up to this location */
	long fbtbc;		/* fragment bytes to be consumed */
	bool_t last_frag;
	u_int sendsize;
	u_int recvsize;

	bool_t nonblock;
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



/* XDR functions */

/* XDR using msk */
int    xdrmsk_create(XDR *, msk_trans_t *,
                            u_int, u_int, u_int,
			    void (*)(void*), void*);

int    rpcrdma_svc_setbuf(XDR *, u_int32_t, enum xdr_op);
bool_t rpcrdma_svc_flushout(XDR *);

int    rpcrdma_clnt_setbuf(XDR *, u_int32_t, enum xdr_op);
bool_t rpcrdma_clnt_flushout(XDR *);


#endif /* !_TIRPC_RPC_RDMA_H */
