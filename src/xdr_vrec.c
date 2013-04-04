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

#include <rpc/xdr_vrec.h>

static bool xdr_vrec_getlong(XDR *, long *);
static bool xdr_vrec_putlong(XDR *, const long *);
static bool xdr_vrec_getbytes(XDR *, char *, u_int);
static bool xdr_vrec_putbytes(XDR *, const char *, u_int);
static bool xdr_vrec_getbufs(XDR *, xdr_uio *, u_int, u_int);
static bool xdr_vrec_putbufs(XDR *, xdr_uio *, u_int);
static u_int xdr_vrec_getpos(XDR *);
static bool xdr_vrec_setpos(XDR *, u_int);
static int32_t *xdr_vrec_inline(XDR *, u_int);
static void xdr_vrec_destroy(XDR *);
static bool xdr_vrec_control(XDR *, /* const */ int, void *);

typedef bool (* dummyfunc3)(XDR *, int, void *);
typedef bool (* dummyfunc4)(XDR *, const char *, u_int, u_int);

static const struct  xdr_ops xdr_vrec_ops = {
    xdr_vrec_getlong,
    xdr_vrec_putlong,
    xdr_vrec_getbytes,
    xdr_vrec_putbytes,
    xdr_vrec_getpos,
    xdr_vrec_setpos,
    xdr_vrec_inline,
    xdr_vrec_destroy,
    xdr_vrec_control,
    xdr_vrec_getbufs,
    xdr_vrec_putbufs
};

#define LAST_FRAG ((u_int32_t)(1 << 31))

static bool vrec_flush_out(V_RECSTREAM *, bool);
static bool vrec_set_input_fragment(V_RECSTREAM *);
static bool vrec_skip_input_bytes(V_RECSTREAM *, long);
static bool vrec_get_input_segments(V_RECSTREAM *, int);

#define reset_pos(pos) \
    do { \
        (pos)->loff = 0; \
        (pos)->bpos = 0; \
        (pos)->boff = 0; \
    } while (0);

/* not intended to be general--we know fpos->vrec is head of queue */
#define init_lpos(lpos, fpos) \
    do { \
        (lpos)->vrec = (fpos)->vrec; \
        (lpos)->loff = 0; \
        (lpos)->bpos = 0; \
        (lpos)->boff = 0; \
    } while (0);


/* XXX might not be faster than jemalloc, &c? */
static inline void
init_prealloc_queues(V_RECSTREAM *vstrm)
{
    int ix;
    struct v_rec *vrec;

    TAILQ_INIT(&vstrm->prealloc.v_req.q);
    vstrm->prealloc.v_req.size = 0;

    TAILQ_INIT(&vstrm->prealloc.v_req_buf.q);
    vstrm->prealloc.v_req_buf.size = 0;

    for (ix = 0; ix < VQSIZE; ++ix) {
        vrec = mem_zalloc(sizeof(struct v_rec));
        TAILQ_INSERT_TAIL(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

static inline void
vrec_init_queue(struct v_rec_queue *q)
{
    TAILQ_INIT(&q->q);
    q->size = 0;
}

static inline struct v_rec *
vrec_get_vrec(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec;
    if (unlikely(vstrm->prealloc.v_req.size == 0)) {
        vrec = mem_alloc(sizeof(struct v_rec));
    } else {
        vrec = TAILQ_FIRST(&vstrm->prealloc.v_req.q);
        TAILQ_REMOVE(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)--;
    }
    return (vrec);
}

static inline void
vrec_put_vrec(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    if (unlikely(vstrm->prealloc.v_req.size > VQSIZE))
        mem_free(vrec, sizeof(struct v_rec));
    else {
        TAILQ_INSERT_TAIL(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

#if 0 /* jemalloc docs warn about reclaim */
#define vrec_alloc_buffer(size) mem_alloc_aligned(0x8, (size))
#else
#define vrec_alloc_buffer(size) mem_alloc((size))
#endif /* 0 */
#define vrec_free_buffer(addr) mem_free((addr), 0)

#define VREC_NFILL 6
#define VREC_MAXBUFS 24

static inline void
init_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    /* this is a placeholder for a "null" mapping--ie, we'd expect
     * readv to drain its socket buffer and never store or cache any
     * value read into the segment */
    for (ix = 0; ix < VREC_NSINK; ix++) {
        iov = &(vstrm->iovsink[ix]);
        iov->iov_base = vrec_alloc_buffer(VREC_DISCARD_BUFSZ);
        iov->iov_len = VREC_DISCARD_BUFSZ;
    }

    /* the last iovec is used to peek into the next fragment */
    iov = &(vstrm->iovsink[VREC_STATIC_FRAG]);
    iov->iov_base = NULL; /* varies */
    iov->iov_len = sizeof(u_int32_t);
}

static inline void
free_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    for (ix = 0; ix < VREC_NSINK; ix++) {
        iov = &(vstrm->iovsink[ix]);
        vrec_free_buffer(iov->iov_base);
    }
}

#define vrec_qlen(q) ((q)->size)
#define vrec_fpos(vstrm) (&vstrm->ioq.fpos)
#define vrec_lpos(vstrm) (&vstrm->ioq.lpos)

static inline void vrec_append_rec(struct v_rec_queue *q, struct v_rec *vrec)
{
    TAILQ_INSERT_TAIL(&q->q, vrec, ioq);
    (q->size)++;
}

static inline void
vrec_init_ioq(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec = vrec_get_vrec(vstrm);
    vrec->refcnt = 0;
    vrec->size = vstrm->def_bsize;
    vrec->base = vrec_alloc_buffer(vrec->size);
    vrec->off = 0;
    vrec->len = 0;
    vrec->flags = VREC_FLAG_RECLAIM;
    vrec_append_rec(&vstrm->ioq, vrec);
}

static inline void
vrec_rele(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    (vrec->refcnt)--;
    if (unlikely(vrec->refcnt == 0)) {
        if (vrec->flags & VREC_FLAG_RECLAIM) {
            vrec_free_buffer(vrec->base);
        }
        /* return to freelist */
        vrec_put_vrec(vstrm, vrec);
    }
}

enum vrec_cursor
{
    VREC_FPOS,
    VREC_LPOS,
    VREC_RESET_POS
};

/*
 * Set initial read/insert or fill position.
 */
static inline void
vrec_stream_reset(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos)
{
    struct v_rec_pos_t *pos;

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(vstrm);
        break;
    case VREC_LPOS:
        pos = vrec_lpos(vstrm);
        break;
    case VREC_RESET_POS:
        vrec_stream_reset(vstrm, VREC_FPOS);
        vrec_stream_reset(vstrm, VREC_LPOS);
        /* XXX */
        pos = vrec_fpos(vstrm);
        pos->vrec->off = 0;
        pos->vrec->len = 0;
        return;
        break;
    default:
        abort();
        break;
    }

    reset_pos(pos);
    pos->vrec = TAILQ_FIRST(&vstrm->ioq.q);
}

static inline void
vrec_truncate_input_q(V_RECSTREAM *vstrm, int max)
{
    struct v_rec *vrec;

    /* the ioq queue can contain shared and special segments (eg, mapped
     * buffers).  when a segment is shared with an upper layer, 
     * vrec->refcnt is increased.  if a buffer should be freed by this
     * module, bit VREC_FLAG_RECLAIM is set in vrec->flags.
     */

    /* ideally, the first read on the stream */
    if (unlikely(vstrm->ioq.size == 0))
        vrec_init_ioq(vstrm);
    else {
        /* enforce upper bound on ioq size.
         */
        while (unlikely(vstrm->ioq.size > max)) {
            vrec = TAILQ_LAST(&vstrm->ioq.q, vrq_tailq);
            TAILQ_REMOVE(&vstrm->ioq.q, vrec, ioq);
            (vstrm->ioq.size)--;
            /* almost certainly recycles */
            vrec_rele(vstrm, vrec);
        }
    }

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        break;
    case XDR_VREC_OUT:
        vstrm->st_u.out.frag_len = 0;
        break;
    default:
        abort();
        break;
    }

    /* stream reset */
    vrec_stream_reset(vstrm, VREC_RESET_POS);

    return;
}

#define vrec_truncate_output_q vrec_truncate_input_q

/*
 * Advance read/insert or fill position.
 */
static inline bool
vrec_next(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos, u_int flags)
{
    struct v_rec_pos_t *pos;
    struct v_rec *vrec;

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(vstrm);
        /* re-use following buffers */
        vrec = TAILQ_NEXT(pos->vrec, ioq);
        /* append new segments, iif requested */
        if (likely(! vrec) && (flags & VREC_FLAG_XTENDQ)) {
            vrec  = vrec_get_vrec(vstrm);
            /* alloc a buffer, iif requested */
            if (flags & VREC_FLAG_BALLOC) {
                vrec->size = vstrm->def_bsize;
                vrec->base = vrec_alloc_buffer(vrec->size);
                vrec->flags = VREC_FLAG_RECLAIM;
            }
            vrec_append_rec(&vstrm->ioq, vrec);
            (vstrm->ioq.size)++;
        }
        vrec->refcnt = 0;
        vrec->off = 0;
        vrec->len = 0;
        pos->vrec = vrec;
        (pos->bpos)++;
        pos->boff = 0;
        /* pos->loff is unchanged */
        *(vrec_lpos(vstrm)) = *pos; /* XXXX for OUT queue? */
        break;
    case VREC_LPOS:
        pos = vrec_lpos(vstrm);
        vrec = TAILQ_NEXT(pos->vrec, ioq);
        if (unlikely(! vrec))
            return (FALSE);
        pos->vrec = vrec;
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
xdr_vrec_create(XDR *xdrs,
                enum xdr_vrec_direction direction, void *xhandle,
                size_t (*xreadv)(XDR *, void *, struct iovec *, int, u_int),
                size_t (*xwritev)(XDR *, void *, struct iovec *, int, u_int),
                u_int def_bsize, u_int flags)
{
    V_RECSTREAM *vstrm = mem_alloc(sizeof(V_RECSTREAM));

    if (vstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_vrec_create: out of memory");
        return;
    }

    init_prealloc_queues(vstrm);
    init_discard_buffers(vstrm);

    xdrs->x_ops = &xdr_vrec_ops;
    xdrs->x_lib[0] = NULL;
    xdrs->x_lib[1] = NULL;
    xdrs->x_public = NULL;
    xdrs->x_private = vstrm;

    vstrm->direction = direction;
    vstrm->vp_handle = xhandle;

    /* init queues */
    vrec_init_queue(&vstrm->ioq);

    /* buffer tuning */
    vstrm->def_bsize = def_bsize;

    switch (direction) {
    case XDR_VREC_IN:
        vstrm->ops.readv = xreadv;
        vrec_init_ioq(vstrm);
        vstrm->st_u.in.readahead_bytes = 1200; /* XXX PMTU? */
        vstrm->st_u.in.fbtbc = 0;
        vstrm->st_u.in.buflen = 0;
        vstrm->st_u.in.haveheader = FALSE;
        vstrm->st_u.in.last_frag = FALSE; /* do NOT use if !haveheader */
        vstrm->st_u.in.next_frag = FALSE;
        vrec_truncate_input_q(vstrm, 8);
        break;
    case XDR_VREC_OUT:
        vstrm->ops.writev = xwritev;
        vrec_init_ioq(vstrm);
        vrec_truncate_output_q(vstrm, 8);
        vstrm->st_u.out.frag_len = 0;
        vstrm->st_u.out.frag_sent = FALSE;
        break;
    default:
        abort();
        break;
    }

    return;
}

/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdr_vrec_create.
 */
static bool
xdr_vrec_getlong(XDR *xdrs,  long *lp)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos = vrec_lpos(vstrm);
    int32_t *buf = NULL;

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            /* CASE 1:  re-consuming bytes in a stream (after SETPOS/rewind) */
            if (pos->loff < vstrm->st_u.in.buflen) {
            restart_1:
                /* CASE 1.1: first try the inline, fast case */
                if ((pos->boff + sizeof(int32_t)) <= pos->vrec->len) {
                    buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                    *lp = (long)ntohl(*buf);
                    pos->boff += sizeof(int32_t);
                    pos->loff += sizeof(int32_t);
                    /* next vrec? */
                    if (pos->boff >= pos->vrec->len) /* XXX == */
                        (void) vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE);
                } else {
                    /* CASE 1.2: vrec_next */
                    (void) vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE);
                    goto restart_1;
                }
            } else {
                /* CASE 2: reading into the stream */
                /* CASE 2.1: first try the inline, fast case */
                if ((pos->boff + sizeof(int32_t)) <= pos->vrec->len) {
                    buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                    *lp = (long)ntohl(*buf);
                    pos->boff += sizeof(int32_t);
                    pos->loff += sizeof(int32_t);
                } else {
                    /* CASE 2.2: need getbytes */
                    if (! xdr_vrec_getbytes(
                            xdrs, (char *)(void *)buf, sizeof(int32_t)))
                        return (FALSE);
                    *lp = (long)ntohl(*buf);
                }
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            /* CASE 1:  we can only be re-consuming bytes in a stream
             * (after SETPOS/rewind) */
            if (pos->loff < vstrm->st_u.out.frag_len) {
            restart_2:
                /* CASE 1.1: first try the inline, fast case */
                if ((pos->boff + sizeof(int32_t)) <= pos->vrec->len) {
                    buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                    *lp = (long)ntohl(*buf);
                    pos->boff += sizeof(int32_t);
                    pos->loff += sizeof(int32_t);
                } else {
                    /* CASE 1.2: vrec_next */
                    (void) vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE);
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
        break;
    default:
        abort();
        break;
    }

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_vrec_putlong(XDR *xdrs, const long *lp)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;

    pos = vrec_fpos(vstrm);
    if (unlikely((pos->vrec->size - pos->vrec->len)
                 < sizeof(int32_t))) {
        /* advance fill pointer */
        if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ|VREC_FLAG_BALLOC))
            return (FALSE);
    }

    *((int32_t *)(pos->vrec->base + pos->vrec->off)) =
        (int32_t)htonl((u_int32_t)(*lp));

    pos->vrec->off += sizeof(int32_t);
    pos->vrec->len += sizeof(int32_t);
    pos->boff += sizeof(int32_t);
    vstrm->st_u.out.frag_len += sizeof(int32_t);

    return (TRUE);
}

static bool
xdr_vrec_getbytes(XDR *xdrs, char *addr, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    u_long cbtbc;

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            pos = vrec_lpos(vstrm);
        restart:
            /* CASE 1: consuming bytes in a stream (after SETPOS/rewind) */
            while ((len > 0) &&
                   (pos->loff < vstrm->st_u.in.buflen)) {
                u_int delta = MIN(len, (pos->vrec->len - pos->boff));
                if (unlikely(! delta)) {
                    if (! vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE))
                        return (FALSE);
                    goto restart;
                }
                /* in glibc 2.14+ x86_64, memcpy no longer tries to handle
                 * overlapping areas, see Fedora Bug 691336 (NOTABUG);
                 * we dont permit overlapping segments, so memcpy may be a
                 * small win over memmove */
                memcpy(addr, (pos->vrec->base + pos->boff), delta);
                pos->loff += delta;
                pos->boff += delta;
                len -= delta;
            }
            /* CASE 2: reading into the stream */
            while (len > 0) {
                cbtbc = vstrm->st_u.in.fbtbc;
                if (unlikely(! cbtbc)) {
                    if (vstrm->st_u.in.last_frag)
                        return (FALSE);
                    if (! vrec_set_input_fragment(vstrm))
                        return (FALSE);
                    continue;
                }
                cbtbc = (len < cbtbc) ? len : cbtbc;
                if (! vrec_get_input_segments(vstrm, cbtbc))
                    return (FALSE);
                /* now we have CASE 1 */
                goto restart;
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            /* TODO: implement */
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
    default:
        abort();
        break;
    }

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_vrec_putbytes(XDR *xdrs, const char *addr, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    int delta;

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        abort();
        break;
    case XDR_VREC_OUT:
        while (len > 0) {
            pos = vrec_fpos(vstrm);
            delta = MIN(len,  pos->vrec->size - pos->vrec->len);
            if (unlikely(! delta)) {
                /* advance fill pointer */
                if (! vrec_next(vstrm, VREC_FPOS,
                                VREC_FLAG_XTENDQ|VREC_FLAG_BALLOC))
                    return (FALSE);
                continue;
            }
            /* ibid */
            memcpy((pos->vrec->base + pos->vrec->off), addr, delta);
            pos->vrec->off += delta;
            pos->vrec->len += delta;
            pos->boff += delta;
            pos->loff += delta;
            vstrm->st_u.out.frag_len += delta;
            len -= delta;
        }
        break;
    default:
        abort();
        break;
    }
    return (TRUE);
}

/* Get buffers from the queue.
 * XXX do we need len? */
static bool
xdr_vrec_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos = vrec_lpos(vstrm);;
    u_long cbtbc;
    int ix;

    /* allocate sufficient slots to empty the queue, else MAX */
    uio->xbs_cnt = MIN(VREC_MAXBUFS, (vrec_qlen(&vstrm->ioq) - pos->bpos));

    /* fail if no segments available */
    if (unlikely(! uio->xbs_cnt))
        return (FALSE);

    uio->xbs_buf = mem_alloc(uio->xbs_cnt);
    uio->xbs_resid = 0;
    ix = 0;

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
        restart:
            /* CASE 1:  re-consuming bytes in a stream (after SETPOS/rewind) */
            while ((len > 0) &&
                   (pos->loff < vstrm->st_u.in.buflen)) {
                u_int delta = MIN(len, (pos->vrec->len - pos->boff));
                if (unlikely(! delta)) {
                    if (! vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE))
                        return (FALSE);
                        goto restart;
                }
                (uio->xbs_buf[ix]).xb_p1 = pos->vrec;
                (pos->vrec->refcnt)++;
                (uio->xbs_buf[ix]).xb_base = (pos->vrec->base + pos->boff);
                pos->loff += delta;
                pos->boff += delta;
                len -= delta;
            }
            /* CASE 2: reading into the stream */
            while (len > 0) {
                cbtbc = vstrm->st_u.in.fbtbc;
                if (unlikely(! cbtbc)) {
                    if (vstrm->st_u.in.last_frag)
                        return (FALSE);
                    if (! vrec_set_input_fragment(vstrm))
                        return (FALSE);
                    continue;
                }
                cbtbc = (len < cbtbc) ? len : cbtbc;
                if (! vrec_get_input_segments(vstrm, cbtbc))
                    return (FALSE);
                /* now we have CASE 1 */
                goto restart;
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            /* TODO: implement */
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
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
xdr_vrec_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *fpos = vrec_fpos(vstrm);
    xdr_buffer *xbuf;
    int ix;

    switch (flags & XDR_PUTBUFS_FLAG_BRELE) {
    case TRUE:
        /* the caller is returning buffers */
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            struct v_rec *vrec = (struct v_rec *)(uio->xbs_buf[ix]).xb_p1;
            vrec_rele(vstrm, vrec);
            mem_free(uio->xbs_buf, 0);
        }
        break;
    case FALSE:
    default:
        /* XXX potentially we should give the upper layer control over
         * whether buffers are spliced or recycled */
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            /* advance fill pointer, do not allocate buffers */
            if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ))
                return (FALSE);
            xbuf = &(uio->xbs_buf[ix]);
            vstrm->st_u.out.frag_len += xbuf->xb_len;
            fpos->loff += xbuf->xb_len;
            fpos->vrec->flags = VREC_FLAG_NONE; /* !RECLAIM */
            fpos->vrec->refcnt = (xbuf->xb_flags & XBS_FLAG_GIFT) ? 0 : 1;
            fpos->vrec->base = xbuf->xb_base;
            fpos->vrec->size = xbuf->xb_len;
            fpos->vrec->len = xbuf->xb_len;
            fpos->vrec->off = 0;
        }
        break;
    }
    return (TRUE);
}

static u_int
xdr_vrec_getpos(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);

    return (lpos->loff);
}

static bool
xdr_vrec_setpos(XDR *xdrs, u_int pos)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);
    struct v_rec *vrec;
    u_int resid;
    int ix;

    ix = 0;
    resid = 0;
    TAILQ_FOREACH(vrec, &(vstrm->ioq.q), ioq) {
        if ((vrec->len + resid) > pos) {
            lpos->vrec = vrec;
            lpos->bpos = ix;
            lpos->loff = pos;
            lpos->boff = (pos-resid);
            return (TRUE);
        }
        resid += vrec->len;
        ++ix;
    }
    return (FALSE);
}

static int32_t *
xdr_vrec_inline(XDR *xdrs, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    int32_t *buf = NULL;

    /* we keep xdrrec's inline concept mostly intact.  the function
     * returns the address of the current logical offset in the stream
     * buffer, iff no less than len contiguous bytes are available at
     * the current logical offset in the stream. */

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            pos = vrec_lpos(vstrm);
            if ((vstrm->st_u.in.buflen - pos->loff) >= len) {
                if ((pos->boff + len) <= pos->vrec->size) {
                    buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                    pos->boff += len;
                    pos->loff += len;
                }
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            pos = vrec_fpos(vstrm);
            if ((pos->boff + len) <= pos->vrec->size) {
                buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                vstrm->st_u.out.frag_len += len;
                pos->boff += len;
            }
            init_lpos(vrec_lpos(vstrm), pos);
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
    default:
        abort();
        break;
    }

    return (buf);
}

static void
xdr_vrec_destroy(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec *vrec;

    /* release queued buffers */
    while (vstrm->ioq.size > 0) {
        vrec = TAILQ_FIRST(&vstrm->ioq.q);
        vrec_rele(vstrm, vrec);
        TAILQ_REMOVE(&vstrm->ioq.q, vrec, ioq);
        (vstrm->ioq.size)--;
    }
    free_discard_buffers(vstrm);
    mem_free(vstrm, sizeof(V_RECSTREAM));
}

static bool
xdr_vrec_control(XDR *xdrs, /* const */ int rq, void *in)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;

    switch (rq) {
    case VREC_GET_READAHEAD:
        if (vstrm->direction != XDR_VREC_IN)
            return (FALSE);
        *(u_int *)in = vstrm->st_u.in.readahead_bytes;
        break;
    case VREC_SET_READAHEAD:
        if (vstrm->direction != XDR_VREC_IN)
            return (FALSE);
        vstrm->st_u.in.readahead_bytes = *(u_int *)in;
        break;
    default:
        return (FALSE);
    }
    return (TRUE);
}

static bool xdr_vrec_noop(void) __attribute__((unused));

static bool
xdr_vrec_noop(void)
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
xdr_vrec_skiprecord(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_DECODE:
        restart:
            /* CASE 1: xdr_vrec_eof read the next header */
            if (vstrm->st_u.in.next_frag) {
                vstrm->st_u.in.next_frag = FALSE; /* clear flag */
            } else { /* nothing */
                /* CASE 2: we have nothing */
                if (! vstrm->st_u.in.haveheader) {
                    if (! vrec_set_input_fragment(vstrm))
                        return (FALSE);
                } else {
                    /* haveheader=TRUE */
                    if (vstrm->st_u.in.fbtbc) {
                        /* CASE 3: have fbtbc (may be last_frag) */
                        (void) vrec_skip_input_bytes(vstrm,
                                                     vstrm->st_u.in.fbtbc);
                        goto restart;
                    }
                    /* CASE 4: fbtbc==0, last_frag==TRUE */
                    if ( vstrm->st_u.in.last_frag) {
                        if (! vrec_set_input_fragment(vstrm))
                            return (FALSE);
                    } else {
                        /* CASE 5: fbtbc==0, last_frag==FALSE */
                        (void) vrec_skip_input_bytes(vstrm,
                                                     vstrm->st_u.in.fbtbc);
                        goto restart;
                    }
                } /* ! haveheader */
                break;
            } /* ! next_frag */
            /* bound queue size and support future mapped reads */
            vrec_truncate_input_q(vstrm, 8);
            init_lpos(vrec_lpos(vstrm), vrec_fpos(vstrm));
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
    default:
        abort();
        break;
    }
    return (TRUE);
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_vrec_eof(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *pos __attribute__((unused));

    /* XXX a potential open issue here is if readahead could have
     * consumed a fragment ahead in the stream.  How do we recognize and
     * deal with this? */

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_DECODE:
            pos = vrec_lpos(vstrm); /* XXX not sure req. yet... */
        restart:
            if (vstrm->st_u.in.last_frag) {
                if (vstrm->st_u.in.fbtbc) {
                    if (vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc)) {
                        /* CASE 1: not eof, and warn skiprecord that
                         * we have the next header */
                        vstrm->st_u.in.next_frag = TRUE;
                        return (FALSE);
                    }
                }
                /* CASE 2: eof */
                return (TRUE);
            }
            /* CASE 3: more fragments */
            if (vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc))
                goto restart;
            else
                return (FALSE); /* XXX error condition, prefer a timeout to
                                 * a stalled queue */
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
    default:
        abort();
        break;
    }
    return (FALSE);
}

/*
 * The client must tell the package when an end-of-record has occurred.
 * The second paramter tells whether the record should be flushed to the
 * (output) tcp stream.  (This let's the package support batched or
 * pipelined procedure calls.)  TRUE => immmediate flush to tcp connection.
 */
bool
xdr_vrec_endofrecord(XDR *xdrs, bool flush)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    /* XXXX atm, frag_sent can never be true... */

    /* flush, resetting stream (sends LAST_FRAG) */
    if (flush || vstrm->st_u.out.frag_sent) {
        vstrm->st_u.out.frag_sent = FALSE;
        return (vrec_flush_out(vstrm, TRUE));
    }

    return (TRUE);
}

/*
 * Internal useful routines
 */

#define VREC_NIOVS 8

static inline void
vrec_flush_segments(V_RECSTREAM *vstrm, struct iovec *iov, int iovcnt,
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
            vstrm->xdrs, vstrm->vp_handle, tiov, iovcnt, VREC_FLAG_NONE);
        if (unlikely(nbytes < 0)) {
            __warnx(TIRPC_DEBUG_FLAG_XDRREC, "%s ops.writev failed %d\n",
                    __func__, errno);
            return; /* XXX is there any way to recover? */
        }
        resid -= nbytes;
    }
}

static bool
vrec_flush_out(V_RECSTREAM *vstrm, bool eor)
{
    u_int32_t eormask = (eor == TRUE) ? LAST_FRAG : 0;
    struct iovec iov[VREC_NIOVS];
    struct v_rec *vrec;
    u_int resid;
    int ix;

    /* update fragment header */
    vstrm->st_u.out.frag_header =
        htonl((u_int32_t)(vstrm->st_u.out.frag_len | eormask));

    iov[0].iov_base = &(vstrm->st_u.out.frag_header);
    iov[0].iov_len = sizeof(u_int32_t);

    ix = 1;
    resid = sizeof(u_int32_t);
    TAILQ_FOREACH(vrec, &(vstrm->ioq.q), ioq) {
        iov[ix].iov_base = vrec->base;
        iov[ix].iov_len = vrec->len;
        resid += vrec->len;
        if (unlikely((vrec == TAILQ_LAST(&(vstrm->ioq.q), vrq_tailq)) ||
                (ix == (VREC_NIOVS-1)))) {
            vrec_flush_segments(vstrm, iov, ix+1 /* iovcnt */, resid);
            resid = 0;
            ix = 0;
            continue;
        }
        ++ix;
    }

    vrec_truncate_output_q(vstrm, 8); /* calls vrec_stream_reset */

    return (TRUE);
}

static inline bool
decode_fragment_header(V_RECSTREAM *vstrm, u_int32_t header)
{
    /*
     * Sanity check. Try not to accept wildly incorrect
     * record sizes. Unfortunately, the only record size
     * we can positively identify as being 'wildly incorrect'
     * is zero. Ridiculously large record sizes may look wrong,
     * but we don't have any way to be certain that they aren't
     * what the client actually intended to send us.
     */
    if (header) {
        header = ntohl(header);
        if (header == 0) {
            vstrm->st_u.in.haveheader = FALSE;
            return(FALSE);
        }

        /* decode and clear LAST_FRAG bit */
        vstrm->st_u.in.last_frag = ((header & LAST_FRAG) == 0) ? FALSE : TRUE;
        vstrm->st_u.in.fbtbc = header & (~LAST_FRAG);
        vstrm->st_u.in.haveheader = TRUE;

        return (TRUE);
    }
    return (FALSE);
}

#if defined(__MINGW32__)
/* XXX Ick. */
#undef readv
#undef writev
#endif

/* Read an initial fragment.  Tries readahead to improve buffering. */
static bool
vrec_set_input_fragment(V_RECSTREAM *vstrm)
{
    struct v_rec_pos_t *pos;
    struct iovec iov[2];
    u_int32_t header;
    uint32_t nbytes, delta;

    pos = vrec_fpos(vstrm); /* XXX multiple fragments? */

    /* XXX assumes we have not already read into the next buffer--that
     * assumption could change.  if so, we would move the vrec at fill
     * position to the head of ioq, shift vrec->off, vrec->len, and
     * vstrm->st_u.in.buflen, and omit stream reset (we might add a flags
     * arg to vrec_truncate input_q).*/
    vstrm->st_u.in.buflen = 0;
    vrec_truncate_input_q(vstrm, 8);
    init_lpos(vrec_lpos(vstrm), vrec_fpos(vstrm));

    /* fragment header */
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(u_int32_t);

    /* data up to readahead bytes */
restart:
    iov[1].iov_base = pos->vrec->base + pos->vrec->off;
    iov[1].iov_len = MIN((pos->vrec->size - pos->vrec->len),
                         (vstrm->st_u.in.readahead_bytes - sizeof(u_int32_t)));

    /* check for filled pos->vrec */
    if (unlikely(! iov[1].iov_len)) {
        if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ|VREC_FLAG_BALLOC))
            return (FALSE);
        goto restart;
    }
    nbytes = vstrm->ops.readv(vstrm->xdrs, vstrm->vp_handle, iov, 2,
                              VREC_FLAG_NONE);
    if (likely(nbytes > 0)) {
        delta = (nbytes - sizeof(u_int32_t));
        if (likely(decode_fragment_header(vstrm, header))) {
            __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                    "%s fbtbc %ld de1ta %ld\n",
                    __func__, vstrm->st_u.in.fbtbc, delta);
            if (delta > vstrm->st_u.in.fbtbc) {
                /* XXX if this case happens, we need to process a header
                 * in the interior of a buffer (shift_pos is intended to
                 * indicate to skiprecord that we did that, by moving the
                 * current vrec to the head of the queue, and starting its
                 * offset at the read point, after the header).  I'm not
                 * clear it does happen */
                abort(); /* TODO: shift_pos */
            }
            pos->vrec->len += delta;
            vstrm->st_u.in.buflen += delta;
            vstrm->st_u.in.fbtbc -= delta; /* bytes read count against fbtbc */
            return (TRUE);
        }
    } else {
        /* XXX */
        vstrm->st_u.in.fbtbc = 0;
        vstrm->st_u.in.haveheader = FALSE;
        vstrm->st_u.in.last_frag = FALSE;
        if (nbytes < 0) {
            __warnx(TIRPC_DEBUG_FLAG_XDRREC, "%s ops.readv failed %d\n",
                    __func__, errno);
        }
    }
    return (FALSE);
}

/* Consume and discard cnt bytes from the input stream.  Try to read
 * the next fragment header if available.  Returns TRUE if a new fragment
 * header has been read, else false. */
static bool
vrec_skip_input_bytes(V_RECSTREAM *vstrm, long cnt)
{
    int ix;
    u_int32_t nbytes, resid, header = 0;
    struct iovec *iov;

    while (cnt > 0) {
        for (ix = 0, nbytes = 0, resid = cnt;
             ((resid > 0) && (ix < VREC_NSINK)); ++ix) {
            iov = &(vstrm->iovsink[ix]);
            iov->iov_len = MIN(resid, VREC_DISCARD_BUFSZ);
            resid -= iov->iov_len;
            nbytes += iov->iov_len;
        }
        /* on fragment boundary, try to read the next header */
        if (nbytes == cnt) {
            iov = &(vstrm->iovsink[VREC_STATIC_FRAG]);
            iov->iov_base = &header;
            /* iov->iov_len = sizeof(u_int32_t); */
            ++ix;
        }
        nbytes = vstrm->ops.readv(vstrm->xdrs, vstrm->vp_handle,
                                  (struct iovec *) &(vstrm->iovsink),
                                  ix+1 /* iovcnt */,
                                  VREC_FLAG_NONE);
        if (unlikely(nbytes < 0)) {
            __warnx(TIRPC_DEBUG_FLAG_XDRREC, "%s ops.readv failed %d\n",
                    __func__, errno);
            return false; /* XXX is there any way to recover? */
        } else {
            /* bytes read into current fragment count against fbtbc */
            __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                    "%s fbtbc %ld nbytes %ld\n",
                    __func__, vstrm->st_u.in.fbtbc, nbytes);
            vstrm->st_u.in.fbtbc -= nbytes;
            cnt -= nbytes;
        }
    } /* while */

    if (decode_fragment_header(vstrm, header)) {
        return (TRUE);
    }

    return (FALSE);
}

/* Read input bytes from the stream, segment-wise. */
static bool vrec_get_input_segments(V_RECSTREAM *vstrm, int cnt)
{
    struct v_rec_pos_t *pos = vrec_fpos(vstrm);
    struct v_rec *vrecs[VREC_NFILL];
    struct iovec iov[VREC_NFILL];
    u_int32_t delta, resid;
    int ix;

    resid = cnt;
    for (ix = 0; ((ix < VREC_NFILL) && (resid > 0)); ++ix) {
        /* XXX */
        delta = MIN(resid, (pos->vrec->size - pos->vrec->len));
        if (! delta) {
            if (! vrec_next(vstrm, VREC_FPOS,
                            VREC_FLAG_XTENDQ|VREC_FLAG_BALLOC))
                return (FALSE);
            delta = MIN(resid, pos->vrec->size);
        }
        /* XXX pos->vrec MAY have been allocated, but in advanced setups
         * it may be pre-posted buffer */
        vrecs[ix] = pos->vrec;
        iov[ix].iov_base = vrecs[ix]->base;
        iov[ix].iov_len = delta;
        resid -= delta;
    }
    /* XXX callers will re-try if we get a short read */
    resid = vstrm->ops.readv(vstrm->xdrs, vstrm->vp_handle, iov, ix,
                             VREC_FLAG_NONE);


    for (ix = 0; ((ix < VREC_NFILL) && (resid > 0)); ++ix) {
        vstrm->st_u.in.buflen += iov[ix].iov_len;
        /* bytes read count against fbtbc */
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "%s fbtbc %ld iov_len %ld\n",
                __func__, vstrm->st_u.in.fbtbc, iov[ix].iov_len);
        vstrm->st_u.in.fbtbc -= iov[ix].iov_len;
        vrecs[ix]->len = iov[ix].iov_len;
        resid -= iov[ix].iov_len;
    }
    pos->vrec = vrecs[ix];
    pos->loff = vstrm->st_u.in.buflen - vrecs[ix]->len;
    pos->bpos += ix;
    pos->boff = 0;
    return (TRUE);
}
