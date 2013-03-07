/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <misc/queue.h>

struct v_out
{
    TAILQ_ENTRY(v_out) ioq;
    uint32_t refcnt;
    void *base;
    u_int off;
    u_int len;
    u_int size;
    u_int flags;
};

#define VQSIZE 2 /* 64 */

struct v_out_pos_t
{
    struct v_out *vout;
    int32_t loff; /* logical byte offset (convenience?) */
    int32_t bpos; /* buffer index (offset) in the current stream */
    int32_t boff; /* byte offset in buffer bix */
};

struct v_out_queue
{
    TAILQ_HEAD(vrq_tailq, v_out) q;
    struct v_out_pos_t fpos; /* fill position */
    struct v_out_pos_t lpos; /* logical position, GET|SETPOS */
    int size; /* count of buffer segments */
    u_int flags;
};

struct v_out_strm
{
    /* XDR */
    XDR *xdrs;

    /* OS buffer queue */
    TAILQ_ENTRY(v_out_strm) sendq;

    /* buffer queues */
    struct v_out_queue ioq;

    /* opaque provider handle */
    void *vp_handle;

    /*
     * ops
     */
    union {
        size_t (*readv)(XDR *, void *, struct iovec *, int, u_int);
        size_t (*writev)(XDR *, void *, struct iovec *, int, u_int);
    } ops;

    /* stream params */
    int maxbufs;
    u_int def_bsize; /* def. size of allocated buffers */

    /* stream state */
    union {
        struct {
            u_int32_t frag_header; /* beginning of outgoing fragment */
            u_long frag_len; /* fragment length */
            bool frag_sent; /* true if buffer sent in middle of record */
        } out;
    } st_u;

};

typedef struct v_out_strm V_OUTSTREAM;

/* provider inteface */

#define VOUT_FLAG_NONE          0x0000
#define VOUT_FLAG_RECLAIM       0x0001
#define VOUT_FLAG_BUFQ          0x0002
#define VOUT_FLAG_XTENDQ        0x0004
#define VOUT_FLAG_BALLOC        0x0008

/* control interface */
#define VOUT_GET_READAHEAD      0x0001
#define VOUT_SET_READAHEAD      0x0002

/* vector equivalents */

extern void xdr_vout_create(XDR *,  void *,
                            size_t (*)(XDR *, void *, struct iovec *, int,
                                       u_int), u_int, u_int);

/* make end of xdr record */
extern bool xdr_vout_endofrecord(XDR *, bool);

/* move to beginning of next record */
extern bool xdr_vout_skiprecord(XDR *);

/* true if no more input */
extern bool xdr_vout_eof(XDR *);
