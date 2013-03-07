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

#include <config.h>

#include <sys/types.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#include <err.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <rpc/types.h>
#include <sys/types.h>
#include <reentrant.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include <assert.h>

#include <intrinsic.h>
#include "rpc_com.h"

#include <rpc/xdr_vout.h>

static bool xdr_vout_getlong(XDR *, long *);
static bool xdr_vout_putlong(XDR *, const long *);
static bool xdr_vout_getbytes(XDR *, char *, u_int);
static bool xdr_vout_putbytes(XDR *, const char *, u_int);
static bool xdr_vout_getbufs(XDR *, xdr_uio *, u_int, u_int);
static bool xdr_vout_putbufs(XDR *, xdr_uio *, u_int);
static u_int xdr_vout_getpos(XDR *);
static bool xdr_vout_setpos(XDR *, u_int);
static int32_t *xdr_vout_inline(XDR *, u_int);
static void xdr_vout_destroy(XDR *);
static bool xdr_vout_control(XDR *, /* const */ int, void *);
static bool xdr_vout_noop(void);

typedef bool (* dummyfunc3)(XDR *, int, void *);
typedef bool (* dummyfunc4)(XDR *, const char *, u_int, u_int);

static const struct  xdr_ops xdr_vout_ops = {
    xdr_vout_getlong,
    xdr_vout_putlong,
    xdr_vout_getbytes,
    xdr_vout_putbytes,
    xdr_vout_getpos,
    xdr_vout_setpos,
    xdr_vout_inline,
    xdr_vout_destroy,
    xdr_vout_control,
    xdr_vout_getbufs,
    xdr_vout_putbufs
};

#define LAST_FRAG ((u_int32_t)(1 << 31))

#define reset_pos(pos) \
    do { \
        (pos)->loff = 0; \
        (pos)->bpos = 0; \
        (pos)->boff = 0; \
    } while (0);

/* not intended to be general--we know fpos->vout is head of queue */
#define init_lpos(lpos, fpos) \
    do { \
        (lpos)->vout = (fpos)->vout; \
        (lpos)->loff = 0; \
        (lpos)->bpos = 0; \
        (lpos)->boff = 0; \
    } while (0);

static bool vout_flush_out(V_OUTSTREAM *, bool);

static inline struct v_out *
vout_get_vout(V_OUTSTREAM *vstrm)
{
    struct v_out *vout = mem_alloc(sizeof(struct v_out));
    return (vout);
}

static inline void
vout_put_vout(V_OUTSTREAM *vstrm, struct v_out *vout)
{
    mem_free(vout, sizeof(struct v_out));
}

/* XXX fix */
#define vout_alloc_buffer(size) mem_alloc((size))
#define vout_free_buffer(addr) mem_free((addr), 0)

#define VOUT_NFILL 6
#define VOUT_MAXBUFS 24

#define vout_qlen(q) ((q)->size)
#define vout_fpos(vstrm) (&vstrm->ioq.fpos)
#define vout_lpos(vstrm) (&vstrm->ioq.lpos)

static inline void vout_append_rec(struct v_out_queue *q, struct v_out *vout)
{
    TAILQ_INSERT_TAIL(&q->q, vout, ioq);
    (q->size)++;
}

static inline void
vout_init_ioq(V_OUTSTREAM *vstrm)
{
    struct v_out *vout = vout_get_vout(vstrm);
    vout->refcnt = 0;
    vout->size = vstrm->def_bsize;
    vout->base = vout_alloc_buffer(vout->size);
    vout->off = 0;
    vout->len = 0;
    vout->flags = VOUT_FLAG_RECLAIM;
    vout_append_rec(&vstrm->ioq, vout);
}

static inline void
vout_rele(V_OUTSTREAM *vstrm, struct v_out *vout)
{
    (vout->refcnt)--;
    if (unlikely(vout->refcnt == 0)) {
        if (vout->flags & VOUT_FLAG_RECLAIM) {
            vout_free_buffer(vout->base);
        }
        /* return to freelist */
        vout_put_vout(vstrm, vout);
    }
}

enum vout_cursor
{
    VOUT_FPOS,
    VOUT_LPOS,
    VOUT_RESET_POS
};

/*
 * Set initial read/insert or fill position.
 */
static inline void
vout_stream_reset(V_OUTSTREAM *vstrm, enum vout_cursor wh_pos)
{
    struct v_out_pos_t *pos;

    switch (wh_pos) {
    case VOUT_FPOS:
        pos = vout_fpos(vstrm);
        break;
    case VOUT_LPOS:
        pos = vout_lpos(vstrm);
        break;
    case VOUT_RESET_POS:
        vout_stream_reset(vstrm, VOUT_FPOS);
        vout_stream_reset(vstrm, VOUT_LPOS);
        /* XXX */
        pos = vout_fpos(vstrm);
        pos->vout->off = 0;
        pos->vout->len = 0;
        return;
        break;
    default:
        abort();
        break;
    }

    reset_pos(pos);
    pos->vout = TAILQ_FIRST(&vstrm->ioq.q);
}

static inline void
vout_truncate_output_q(V_OUTSTREAM *vstrm, int max)
{
    struct v_out *vout;

    /* the ioq queue can contain shared and special segments (eg, mapped
     * buffers).  when a segment is shared with an upper layer, 
     * vout->refcnt is increased.  if a buffer should be freed by this
     * module, bit VOUT_FLAG_RECLAIM is set in vout->flags.
     */

    /* ideally, the first read on the stream */
    if (unlikely(vstrm->ioq.size == 0))
        vout_init_ioq(vstrm);
    else {
        /* enforce upper bound on ioq size.
         */
        while (unlikely(vstrm->ioq.size > max)) {
            vout = TAILQ_LAST(&vstrm->ioq.q, vrq_tailq);
            TAILQ_REMOVE(&vstrm->ioq.q, vout, ioq);
            (vstrm->ioq.size)--;
            /* almost certainly recycles */
            vout_rele(vstrm, vout);
        }
    }

    vstrm->st_u.out.frag_len = 0;

    /* stream reset */
    vout_stream_reset(vstrm, VOUT_RESET_POS);

    return;
}

/*
 * Advance read/insert or fill position.
 */
static inline bool
vout_next(V_OUTSTREAM *vstrm, enum vout_cursor wh_pos, u_int flags)
{
    struct v_out_pos_t *pos;
    struct v_out *vout;

    switch (wh_pos) {
    case VOUT_FPOS:
        pos = vout_fpos(vstrm);
        /* re-use following buffers */
        vout = TAILQ_NEXT(pos->vout, ioq);
        /* append new segments, iif requested */
        if (likely(! vout) && (flags & VOUT_FLAG_XTENDQ)) {
            vout  = vout_get_vout(vstrm);
            /* alloc a buffer, iif requested */
            if (flags & VOUT_FLAG_BALLOC) {
                vout->size =  vstrm->def_bsize;
                vout->base = vout_alloc_buffer(vout->size);
                vout->flags = VOUT_FLAG_RECLAIM;
            }
            vout_append_rec(&vstrm->ioq, vout);
            (vstrm->ioq.size)++;
        }
        vout->refcnt = 0;
        vout->off = 0;
        vout->len = 0;
        pos->vout = vout;
        (pos->bpos)++;
        pos->boff = 0;
        /* pos->loff is unchanged */
        *(vout_lpos(vstrm)) = *pos; /* XXXX for OUT queue? */
        break;
    case VOUT_LPOS:
        pos = vout_lpos(vstrm);
        vout = TAILQ_NEXT(pos->vout, ioq);
        if (unlikely(! vout))
            return (FALSE);
        pos->vout = vout;
        (pos->bpos)++;
        pos->boff = 0;
        /* pos->loff is unchanged */
        break;
    default:
        abort();
        break;
    }
    return (TRUE);
}

/*
 * Create an xdr handle
 */
void
xdr_out_create(XDR *xdrs, void *xhandle,
               size_t (*xwritev)(XDR *, void *, struct iovec *, int, u_int),
               u_int def_bsize, u_int flags)
{
    V_OUTSTREAM *vstrm = mem_alloc(sizeof(V_OUTSTREAM));

    if (vstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_vout_create: out of memory");
        return;
    }

    xdrs->x_ops = &xdr_vout_ops;
    xdrs->x_lib[0] = NULL;
    xdrs->x_lib[1] = NULL;
    xdrs->x_public = NULL;
    xdrs->x_private = vstrm;

    vstrm->vp_handle = xhandle;

    /* init queues */
    vout_init_queue(&vstrm->ioq);

    /* buffer tuning */
    vstrm->def_bsize = def_bsize;

    vstrm->ops.writev = xwritev;
    vout_init_ioq(vstrm);
    vout_truncate_output_q(vstrm, 8);
    vstrm->st_u.out.frag_len = 0;
    vstrm->st_u.out.frag_sent = FALSE;

    return;
}

/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdr_vout_create.
 */
static bool
xdr_vout_getlong(XDR *xdrs,  long *lp)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *pos = vout_lpos(vstrm);
    int32_t *buf = NULL;

    switch (xdrs->x_op) {
    case XDR_ENCODE:
        /* CASE 1:  we can only be re-consuming bytes in a stream
         * (after SETPOS/rewind) */
        if (pos->loff < vstrm->st_u.out.frag_len) {
        restart_2:
            /* CASE 1.1: first try the inline, fast case */
            if ((pos->boff + sizeof(int32_t)) <= pos->vout->len) {
                buf = (int32_t *)(void *)(pos->vout->base + pos->boff);
                *lp = (long)ntohl(*buf);
                pos->boff += sizeof(int32_t);
                pos->loff += sizeof(int32_t);
            } else {
                /* CASE 1.2: vout_next */
                (void) vout_next(vstrm, VOUT_LPOS, VOUT_FLAG_NONE);
                goto restart_2;
            }
        } else
            return (FALSE);
        break;
    case XDR_DECODE:
    default:
        abort();
        break;
    }

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_vout_putlong(XDR *xdrs, const long *lp)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *pos;

    pos = vout_fpos(vstrm);
    if (unlikely((pos->vout->size - pos->vout->len)
                 < sizeof(int32_t))) {
        /* advance fill pointer */
        if (! vout_next(vstrm, VOUT_FPOS, VOUT_FLAG_XTENDQ|VOUT_FLAG_BALLOC))
            return (FALSE);
    }

    *((int32_t *)(pos->vout->base + pos->vout->off)) =
        (int32_t)htonl((u_int32_t)(*lp));

    pos->vout->off += sizeof(int32_t);
    pos->vout->len += sizeof(int32_t);
    pos->boff += sizeof(int32_t);
    vstrm->st_u.out.frag_len += sizeof(int32_t);

    return (TRUE);
}

static bool
xdr_vout_getbytes(XDR *xdrs, char *addr, u_int len)
{
    switch (xdrs->x_op) {
    case XDR_ENCODE:
        /* TODO: implement */
        break;
    case XDR_DECODE:
    default:
        abort();
        break;
    }

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_vout_putbytes(XDR *xdrs, const char *addr, u_int len)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *pos;
    int delta;

    while (len > 0) {
        pos = vout_fpos(vstrm);
        delta = MIN(len,  pos->vout->size - pos->vout->len);
        if (unlikely(! delta)) {
            /* advance fill pointer */
            if (! vout_next(vstrm, VOUT_FPOS,
                            VOUT_FLAG_XTENDQ|VOUT_FLAG_BALLOC))
                return (FALSE);
            continue;
        }
        /* ibid */
        memcpy((pos->vout->base + pos->vout->off), addr, delta);
        pos->vout->off += delta;
        pos->vout->len += delta;
        pos->boff += delta;
        pos->loff += delta;
        vstrm->st_u.out.frag_len += delta;
        len -= delta;
    }
 
    return (TRUE);
}

/* Get buffers from the queue.
 * XXX do we need len? */
static bool
xdr_vout_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *pos = vout_lpos(vstrm);
    int ix;

    /* allocate sufficient slots to empty the queue, else MAX */
    uio->xbs_cnt = MIN(VOUT_MAXBUFS, (vout_qlen(&vstrm->ioq) - pos->bpos));

    /* fail if no segments available */
    if (unlikely(! uio->xbs_cnt))
        return (FALSE);

    uio->xbs_buf = mem_alloc(uio->xbs_cnt);
    uio->xbs_resid = 0;
    ix = 0;

    switch (xdrs->x_op) {
    case XDR_ENCODE:
        /* TODO: implement */
        break;
    case XDR_DECODE:
    default:
        abort();
        break;
    }


    /* assert(len == 0); */
    return (TRUE);
}

/* Post buffers on the queue, or, if indicated in flags, return buffers
 * referenced with getbufs. */
static bool
xdr_vout_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *fpos = vout_fpos(vstrm);
    xdr_buffer *xbuf;
    int ix;

    switch (flags & XDR_PUTBUFS_FLAG_BRELE) {
    case TRUE:
        /* the caller is returning buffers */
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            struct v_out *vout = (struct v_out *)(uio->xbs_buf[ix]).xb_p1;
            vout_rele(vstrm, vout);
            mem_free(uio->xbs_buf, 0);
        }
        break;
    case FALSE:
    default:
        /* XXX potentially we should give the upper layer control over
         * whether buffers are spliced or recycled */
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            /* advance fill pointer, do not allocate buffers */
            if (! vout_next(vstrm, VOUT_FPOS, VOUT_FLAG_XTENDQ))
                return (FALSE);
            xbuf = &(uio->xbs_buf[ix]);
            vstrm->st_u.out.frag_len += xbuf->xb_len;
            fpos->loff += xbuf->xb_len;
            fpos->vout->flags = VOUT_FLAG_NONE; /* !RECLAIM */
            fpos->vout->refcnt = (xbuf->xb_flags & XBS_FLAG_GIFT) ? 0 : 1;
            fpos->vout->base = xbuf->xb_base;
            fpos->vout->size = xbuf->xb_len;
            fpos->vout->len = xbuf->xb_len;
            fpos->vout->off = 0;
        }
        break;
    }
    return (TRUE);
}

static u_int
xdr_vout_getpos(XDR *xdrs)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)(xdrs->x_private);
    struct v_out_pos_t *lpos = vout_lpos(vstrm);

    return (lpos->loff);
}

static bool
xdr_vout_setpos(XDR *xdrs, u_int pos)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)(xdrs->x_private);
    struct v_out_pos_t *lpos = vout_lpos(vstrm);
    struct v_out *vout;
    u_int resid;
    int ix;

    ix = 0;
    resid = 0;
    TAILQ_FOREACH(vout, &(vstrm->ioq.q), ioq) {
        if ((vout->len + resid) > pos) {
            lpos->vout = vout;
            lpos->bpos = ix;
            lpos->loff = pos;
            lpos->boff = (pos-resid);
            return (TRUE);
        }
        resid += vout->len;
        ++ix;
    }
    return (FALSE);
}

static int32_t *
xdr_vout_inline(XDR *xdrs, u_int len)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out_pos_t *pos;
    int32_t *buf = NULL;

    /* we keep xdrrec's inline concept mostly intact.  the function
     * returns the address of the current logical offset in the stream
     * buffer, iff no less than len contiguous bytes are available at
     * the current logical offset in the stream. */

    switch (xdrs->x_op) {
    case XDR_ENCODE:
        pos = vout_fpos(vstrm);
        if ((pos->boff + len) <= pos->vout->size) {
            buf = (int32_t *)(void *)(pos->vout->base + pos->boff);
            vstrm->st_u.out.frag_len += len;
            pos->boff += len;
        }
        init_lpos(vout_lpos(vstrm), pos);
            break;
    case XDR_DECODE:
    default:
        abort();
        break;
    }

    return (buf);
}

static void
xdr_vout_destroy(XDR *xdrs)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;
    struct v_out *vout;

    /* release queued buffers */
    while (vstrm->ioq.size > 0) {
        vout = TAILQ_FIRST(&vstrm->ioq.q);
        vout_rele(vstrm, vout);
        TAILQ_REMOVE(&vstrm->ioq.q, vout, ioq);
        (vstrm->ioq.size)--;
    }
    mem_free(vstrm, sizeof(V_OUTSTREAM));
}

static bool
xdr_vout_control(XDR *xdrs, /* const */ int rq, void *in)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)xdrs->x_private;

    switch (rq) {
    default:
        return (FALSE);
    }
    return (TRUE);
}

static bool xdr_vout_noop(void) __attribute__((unused));

static bool
xdr_vout_noop(void)
{
    return (FALSE);
}

/*
 * Exported routines to manage xdr records
 */

/*
 * Before reading (deserializing from the stream), one should always call
 * this procedure to guarantee proper record alignment.
 */
bool
xdr_vout_skiprecord(XDR *xdrs)
{
    return (TRUE);
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_vout_eof(XDR *xdrs)
{
    return (FALSE);
}

/*
 * The client must tell the package when an end-of-record has occurred.
 * The second paramter tells whether the record should be flushed to the
 * (output) tcp stream.  (This let's the package support batched or
 * pipelined procedure calls.)  TRUE => immmediate flush to tcp connection.
 */
bool
xdr_vout_endofrecord(XDR *xdrs, bool flush)
{
    V_OUTSTREAM *vstrm = (V_OUTSTREAM *)(xdrs->x_private);

    /* XXXX atm, frag_sent can never be true... */

    /* flush, resetting stream (sends LAST_FRAG) */
    if (flush || vstrm->st_u.out.frag_sent) {
        vstrm->st_u.out.frag_sent = FALSE;
        return (vout_flush_out(vstrm, TRUE));
    }

    return (TRUE);
}

/*
 * Internal useful routines
 */

#define VOUT_NIOVS 8

static inline void
vout_flush_segments(V_OUTSTREAM *vstrm, struct iovec *iov, int iovcnt,
                    u_int resid)
{
    uint32_t nbytes = 0;
    struct iovec *tiov;
    int ix;

    while (resid > 0) {
        /* advance iov */
        for (ix = 0, tiov = iov; ((nbytes > 0) && (ix < iovcnt)); ++ix) {
            tiov = iov+ix;
            if (tiov->iov_len > nbytes) {
                tiov->iov_base += nbytes;
                tiov->iov_len -= nbytes;
            } else {
                nbytes -= tiov->iov_len;
                iovcnt--;
                continue;
            }
        }
        /* blocking write */
        nbytes = vstrm->ops.writev(
            vstrm->xdrs, vstrm->vp_handle, tiov, iovcnt, VOUT_FLAG_NONE);
        if (unlikely(nbytes < 0)) {
            __warnx(TIRPC_DEBUG_FLAG_XDRREC, "%s ops.writev failed %d\n",
                    __func__, errno);
            return; /* XXX is there any way to recover? */
        }
        resid -= nbytes;
    }
}

static bool
vout_flush_out(V_OUTSTREAM *vstrm, bool eor)
{
    u_int32_t eormask = (eor == TRUE) ? LAST_FRAG : 0;
    struct iovec iov[VOUT_NIOVS];
    struct v_out *vout;
    u_int resid;
    int ix;

    /* update fragment header */
    vstrm->st_u.out.frag_header =
        htonl((u_int32_t)(vstrm->st_u.out.frag_len | eormask));

    iov[0].iov_base = &(vstrm->st_u.out.frag_header);
    iov[0].iov_len = sizeof(u_int32_t);

    ix = 1;
    resid = sizeof(u_int32_t);
    TAILQ_FOREACH(vout, &(vstrm->ioq.q), ioq) {
        iov[ix].iov_base = vout->base;
        iov[ix].iov_len = vout->len;
        resid += vout->len;
        if (unlikely((vout == TAILQ_LAST(&(vstrm->ioq.q), vrq_tailq)) ||
                (ix == (VOUT_NIOVS-1)))) {
            vout_flush_segments(vstrm, iov, ix+1 /* iovcnt */, resid);
            resid = 0;
            ix = 0;
            continue;
        }
        ++ix;
    }

    vout_truncate_output_q(vstrm, 8); /* calls vout_stream_reset */

    return (TRUE);
}

#if defined(__MINGW32__)
/* XXX Ick. */
#undef readv
#undef writev
#endif
