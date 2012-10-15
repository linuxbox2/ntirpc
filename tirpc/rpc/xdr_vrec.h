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

struct v_rec
{
    TAILQ_ENTRY(v_rec) ioq;
    uint32_t refcnt;
    void *base;
    u_int off;
    u_int len;
    u_int size;
    u_int flags;
};

#define VQSIZE 2 /* 64 */

struct v_rec_pos_t
{
    struct v_rec *vrec;
    int32_t loff; /* logical byte offset (convenience?) */
    int32_t bpos; /* buffer index (offset) in the current stream */
    int32_t boff; /* byte offset in buffer bix */
};

#define VREC_QFLAG_NONE      0x0000

struct v_rec_queue
{
    TAILQ_HEAD(vrq_tailq, v_rec) q;
    struct v_rec_pos_t fpos; /* fill position */
    struct v_rec_pos_t lpos; /* logical position, GET|SETPOS */
    int size; /* count of buffer segments */
    u_int flags;
};

/* preallocate memory */
struct vrec_prealloc
{
    struct v_rec_queue v_req;
    struct v_rec_queue v_req_buf;
};

/* streams are unidirectional */
enum xdr_vrec_direction {
    XDR_VREC_IN,
    XDR_VREC_OUT,
};

#define VREC_NSINK 2
#define VREC_NSTATIC (VREC_NSINK + 1)
#define VREC_STATIC_FRAG (VREC_NSTATIC-1)
#define VREC_DISCARD_BUFSZ 8192

struct v_rec_strm
{
    enum xdr_vrec_direction direction;

    /* XDR */
    XDR *xdrs;

    /* buffer queues */
    struct v_rec_queue ioq;

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
            u_long buflen; /* logical input buffer length */
            u_long fbtbc; /* fragment bytes to be consumed */
            u_long rbtbc; /* readahead bytes to be consumed */
            long readahead_bytes; /* bytes to read ahead across fragments */
            bool last_frag; /* private */
            bool next_frag; /* private */
            bool haveheader;
        } in;
        struct {
            u_int32_t frag_header; /* beginning of outgoing fragment */
            u_long frag_len; /* fragment length */
            bool frag_sent; /* true if buffer sent in middle of record */
        } out;
    } st_u;

    /* free lists */
    struct vrec_prealloc prealloc;

    /* discard buffers */
    struct iovec iovsink[VREC_NSTATIC];
};

typedef struct v_rec_strm V_RECSTREAM;

/* provider inteface */

#define VREC_FLAG_NONE          0x0000
#define VREC_FLAG_RECLAIM       0x0001
#define VREC_FLAG_BUFQ          0x0002
#define VREC_FLAG_XTENDQ        0x0004
#define VREC_FLAG_BALLOC        0x0008

/* control interface */
#define VREC_GET_READAHEAD      0x0001
#define VREC_SET_READAHEAD      0x0002

/* vector equivalents */

extern void xdr_vrec_create(XDR *, enum xdr_vrec_direction, void *,
                            size_t (*)(XDR *, void *, struct iovec *, int,
                                       u_int),
                            size_t (*)(XDR *, void *, struct iovec *, int,
                                       u_int),
                            u_int, u_int);

/* make end of xdr record */
extern bool xdr_vrec_endofrecord(XDR *, bool);

/* move to beginning of next record */
extern bool xdr_vrec_skiprecord(XDR *);

/* true if no more input */
extern bool xdr_vrec_eof(XDR *);
