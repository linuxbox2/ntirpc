/*
 * Copyright (c) 2013 Linux Box Corporation.
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

#include <rpc/xdr_ioq.h>

static bool xdr_ioq_getlong(XDR *xdrs, long *lp);
static bool xdr_ioq_putlong(XDR *xdrs, const long *lp);
static bool xdr_ioq_getbytes(XDR *xdrs, char *addr, u_int len);
static bool xdr_ioq_putbytes(XDR *xdrs, const char *addr, u_int len);
static bool xdr_ioq_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags);
static bool xdr_ioq_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags);
static u_int xdr_ioq_getpos(XDR *xdrs);
static bool xdr_ioq_setpos(XDR *xdrs, u_int pos);
static int32_t *xdr_ioq_inline(XDR *xdrs, u_int len);
static void xdr_ioq_destroy(XDR *xdrs);
static bool xdr_ioq_control(XDR *xdrs, /* const */ int rq, void *in);
static bool xdr_ioq_noop(void) __attribute__((unused));

static const struct  xdr_ops xdr_ioq_ops = {
    xdr_ioq_getlong,
    xdr_ioq_putlong,
    xdr_ioq_getbytes,
    xdr_ioq_putbytes,
    xdr_ioq_getpos,
    xdr_ioq_setpos,
    xdr_ioq_inline,
    xdr_ioq_destroy,
    xdr_ioq_control,
    xdr_ioq_getbufs,
    xdr_ioq_putbufs
};

#define LAST_FRAG ((u_int32_t)(1 << 31))

#define reset_pos(pos) \
    do { \
        (pos)->loff = 0; \
        (pos)->bpos = 0; \
        (pos)->boff = 0; \
    } while (0)

/* not intended to be general--we know fpos->vrec is head of queue */
#define init_lpos(lpos, fpos) \
    do { \
        (lpos)->vrec = (fpos)->vrec; \
        (lpos)->loff = 0; \
        (lpos)->bpos = 0; \
        (lpos)->boff = 0; \
    } while (0)

#define VREC_MAXBUFS 24

#if 0 /* jemalloc docs warn about reclaim */
#define alloc_buffer(size) mem_alloc_aligned(0x8, (size))
#else
#define alloc_buffer(size) mem_alloc((size))
#endif /* 0 */
#define free_buffer(addr) mem_free((addr), 0)

#define vrec_qlen(q) ((q)->size)
#define vrec_fpos(xioq) (&xioq->ioq.fpos)

enum vrec_cursor
{
    VREC_FPOS,
    VREC_RESET_POS
};

/*
 * Set initial read/insert or fill position.
 */
static inline void
ioq_stream_reset(struct xdr_ioq *xioq, enum vrec_cursor wh_pos)
{
    struct vpos_t *pos;

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(xioq);
        break;
    case VREC_RESET_POS:
        ioq_stream_reset(xioq, VREC_FPOS);
        /* XXX */
        pos = vrec_fpos(xioq);
        pos->vrec->off = 0;
        pos->vrec->len = 0;
        return;
        break;
    default:
        abort();
        break;
    }

    reset_pos(pos);
    pos->vrec = TAILQ_FIRST(&xioq->ioq.q);
}

static inline struct v_rec *
get_vrec(struct xdr_ioq *xioq)
{
    struct v_rec *vrec;
    vrec = mem_zalloc(sizeof(struct v_rec));
    TAILQ_INIT_ENTRY(vrec, ioq);
    return (vrec);
}

static inline void ioq_append_rec(struct xdr_ioq *xioq, struct v_rec *vrec)
{
    TAILQ_INSERT_TAIL(&xioq->ioq.q, vrec, ioq);
    (xioq->ioq.size)++;
}

static inline void
vrec_rele(struct xdr_ioq *xioq, struct v_rec *vrec)
{
    (vrec->refcnt)--;
    if (unlikely(vrec->refcnt == 0)) {
        if (vrec->flags & IOQ_FLAG_RECLAIM) {
            free_buffer(vrec->base);
        }
        mem_free(vrec, 0);
    }
}

static inline void
init_ioq(struct xdr_ioq *xioq)
{
    struct v_rec *vrec;

    TAILQ_INIT_ENTRY(xioq, ioq_s);
    TAILQ_INIT(&xioq->ioq.q);
    xioq->ioq.size = 0;
    xioq->ioq.frag_len = 0;
    vrec = get_vrec(xioq);
    vrec->size = xioq->def_bsize;
    vrec->refcnt = 1;
    vrec->base = alloc_buffer(vrec->size);
    vrec->flags = IOQ_FLAG_RECLAIM;
    ioq_append_rec(xioq, vrec);
    ioq_stream_reset(xioq, VREC_RESET_POS);
}

XDR *xdr_ioq_create(u_int def_bsize, u_int max_bsize, u_int flags)
{
    struct xdr_ioq *xioq = mem_alloc(sizeof(struct xdr_ioq));

    XDR *xdrs = xioq->xdrs;
    xdrs->x_ops = &xdr_ioq_ops;
    xdrs->x_op = XDR_ENCODE;
    xdrs->x_lib[0] = NULL;
    xdrs->x_lib[1] = NULL;
    xdrs->x_public = NULL;
    xdrs->x_private = xioq;

    xioq->def_bsize = def_bsize; /* XXX small multiple of pagesize */
    xioq->max_bsize = max_bsize;
    xioq->flags = flags;
    init_ioq(xioq);

    return (xdrs);
}

/*
 * Advance read/insert or fill position.
 */
static inline bool
vrec_next(struct xdr_ioq *xioq, u_int flags)
{
    struct vpos_t *pos;
    struct v_rec *vrec;

    pos = vrec_fpos(xioq);

    /* next buffer, if any */
    vrec = TAILQ_NEXT(pos->vrec, ioq);

    /* append new segments, iif requested */
    if ((! vrec) && likely(flags & IOQ_FLAG_XTENDQ)) {
      /* alloc a buffer, iif requested */
      if (flags & IOQ_FLAG_BALLOC) {
          /* XXX workaround for lack of segmented buffer interfaces
           * in some callers (e.g, GSS_WRAP) */
          if (xioq->flags & IOQ_FLAG_REALLOC) {
              void *base;
              vrec = pos->vrec;
	      /* bail if we have reached max bufsz */
	      if (vrec->size >= xioq->max_bsize)
		return false;
              base = mem_alloc(xioq->max_bsize);
              memcpy(base, vrec->base, vrec->len);
              mem_free(vrec->base, vrec->size);
              vrec->base = base;
              vrec->size = xioq->max_bsize;
              assert(vrec->flags & IOQ_FLAG_RECLAIM);
	      goto done;
          } else {
              vrec = get_vrec(xioq);
              vrec->size = xioq->def_bsize;
              vrec->base = alloc_buffer(vrec->size);
              vrec->flags = IOQ_FLAG_RECLAIM;
          }
      } else {
          /* XXX empty buffer slot (not supported for now) */
          abort();
	  vrec = get_vrec(xioq);
          vrec->size = 0;
          vrec->base = NULL;
          vrec->flags = IOQ_FLAG_NONE;
      }
    }

    /* new vrec */
    vrec->refcnt = 1;
    vrec->off = 0;
    vrec->len = 0;
    ioq_append_rec(xioq, vrec);

    /* advance iterator */
    pos->vrec = vrec;
    (pos->bpos)++;
    pos->boff = 0;
    /* pos->loff is unchanged */

 done:
    return (TRUE);
}

static bool
xdr_ioq_getlong(XDR *xdrs, long *lp)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);
    int32_t *buf = NULL;

    switch (xdrs->x_op) {
    case XDR_ENCODE:
        /* CASE 1:  we can only be re-consuming bytes in a stream
         * (after SETPOS/rewind) */
        if (pos->loff < xioq->ioq.frag_len) {
        restart:
            /* CASE 1.1: first try the inline, fast case */
            if ((pos->boff + sizeof(int32_t)) <= pos->vrec->len) {
                buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                *lp = (long)ntohl(*buf);
                pos->boff += sizeof(int32_t);
                pos->loff += sizeof(int32_t);
            } else {
                /* CASE 1.2: vrec_next */
                (void) vrec_next(xioq, IOQ_FLAG_NONE);
                goto restart;
            }
        } else
            return (FALSE);
        break;
    case XDR_DECODE:
    default:
        abort();
        break;
    } /* switch */

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_ioq_putlong(XDR *xdrs, const long *lp)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);

    if (unlikely((pos->vrec->len + sizeof(int32_t)) > pos->vrec->size)) {
        /* advance fill pointer */
        if (! vrec_next(xioq, IOQ_FLAG_XTENDQ|IOQ_FLAG_BALLOC))
            return (FALSE);
    }

    *((int32_t *)(pos->vrec->base + pos->vrec->off)) =
        (int32_t)htonl((u_int32_t)(*lp));

    pos->vrec->off += sizeof(int32_t);
    pos->vrec->len += sizeof(int32_t);
    pos->boff += sizeof(int32_t);
    pos->loff += sizeof(int32_t);
    if (pos->loff > xioq->ioq.frag_len)
      xioq->ioq.frag_len = pos->loff;

    return (TRUE);
}

static bool
xdr_ioq_getbytes(XDR *xdrs, char *addr, u_int len)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos;
    uint32_t off = 0;

    switch (xdrs->x_op) {
    case XDR_ENCODE:
        /* consuming bytes in a stream (after SETPOS/rewind) */
    restart:
        pos = vrec_fpos(xioq);
        while ((len > 0) &&
               (pos->loff < xioq->ioq.frag_len)) {
            int delta = MIN(len, (pos->vrec->len - pos->boff));
            if (unlikely(! delta)) {
                if (! vrec_next(xioq, IOQ_FLAG_NONE))
                    return (FALSE);
                goto restart;
            }
            /* in glibc 2.14+ x86_64, memcpy no longer tries to handle
             * overlapping areas, see Fedora Bug 691336 (NOTABUG);
             * we dont permit overlapping segments, so memcpy may be a
             * small win over memmove */
            memcpy(addr+off, (pos->vrec->base + pos->boff), delta);
            pos->loff += delta;
            pos->boff += delta;
            off += delta;
            len -= delta;
        }
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
xdr_ioq_putbytes(XDR *xdrs, const char *addr, u_int len)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos;
    uint32_t off = 0;
    int delta;

    while (len > 0) {
        pos = vrec_fpos(xioq);
        delta = MIN(len,  pos->vrec->size - pos->vrec->len);
        if (unlikely(! delta)) {
            /* advance fill pointer */
            if (unlikely(! vrec_next(xioq, IOQ_FLAG_XTENDQ|IOQ_FLAG_BALLOC))) {
                return (FALSE);
            }
            continue;
        }
        /* see note above */
        memcpy((pos->vrec->base + pos->vrec->off), addr+off, delta);
        pos->vrec->off += delta;
        pos->vrec->len += delta;
        pos->boff += delta;
        pos->loff += delta;
        off += delta;
	if (pos->loff > xioq->ioq.frag_len)
	  xioq->ioq.frag_len = pos->loff;
        len -= delta;
    }
    return (TRUE);
}

/* Get buffers from the queue. */
static bool
xdr_ioq_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);
    int ix;

    /* allocate sufficient slots to empty the queue, else MAX */
    uio->xbs_cnt = MIN(VREC_MAXBUFS, (vrec_qlen(&xioq->ioq) - pos->bpos));

    /* fail if no segments available */
    if (unlikely(! uio->xbs_cnt))
        return (FALSE);

    uio->xbs_buf = mem_alloc(uio->xbs_cnt);
    uio->xbs_resid = 0;
    ix = 0;

restart:
    /* re-consuming bytes in a stream (after SETPOS/rewind) */
    while ((len > 0) &&
           (pos->loff < xioq->ioq.frag_len)) {
        u_int delta = MIN(len, (pos->vrec->len - pos->boff));
        if (unlikely(! delta)) {
            if (! vrec_next(xioq, IOQ_FLAG_NONE))
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

    /* assert(len == 0); */
    return (TRUE);
}

/* Post buffers on the queue, or, if indicated in flags, return buffers
 * referenced with getbufs. */
static bool
xdr_ioq_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);
    xdr_buffer *xbuf;
    int ix;

    /* XXXX fixme */

    switch (flags & XDR_PUTBUFS_FLAG_BRELE) {
    case TRUE:
        /* the caller is returning buffers */
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            struct v_rec *vrec = (struct v_rec *)(uio->xbs_buf[ix]).xb_p1;
            vrec_rele(xioq, vrec);
            mem_free(uio->xbs_buf, 0);
        }
        break;
    case FALSE:
    default:
        for (ix = 0; ix < uio->xbs_cnt; ++ix) {
            /* advance fill pointer, do not allocate buffers */
            if (! vrec_next(xioq, IOQ_FLAG_XTENDQ))
                return (FALSE);
            xbuf = &(uio->xbs_buf[ix]);
            xioq->ioq.frag_len += xbuf->xb_len;
            pos->loff += xbuf->xb_len;
            pos->vrec->flags = IOQ_FLAG_NONE; /* !RECLAIM */
            pos->vrec->refcnt = (xbuf->xb_flags & XBS_FLAG_GIFT) ? 0 : 1;
            pos->vrec->base = xbuf->xb_base;
            pos->vrec->size = xbuf->xb_len;
            pos->vrec->len = xbuf->xb_len;
            pos->vrec->off = 0;
        }
        break;
    }
    return (TRUE);
}

static u_int
xdr_ioq_getpos(XDR *xdrs)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);
    /* == frag_len except after XDR_SETPOS */
    return (pos->loff);
}

static bool
xdr_ioq_setpos(XDR *xdrs, u_int pos)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *fpos = vrec_fpos(xioq);
    struct v_rec *vrec;
    u_int resid;
    int ix;

    ix = 0;
    resid = 0;
    TAILQ_FOREACH(vrec, &(xioq->ioq.q), ioq) {
        if ((vrec->size + resid) >= pos) {
            fpos->vrec = vrec;
            fpos->bpos = ix;
            fpos->loff = pos;
            fpos->boff = (pos-resid);
	    /* XXX oops, redundant */
	    fpos->vrec->off = fpos->loff;
	    fpos->vrec->len = fpos->loff;
            return (TRUE);
        }
        resid += vrec->len;
        ++ix;
    }

    return (FALSE);
}

static int32_t *
xdr_ioq_inline(XDR *xdrs, u_int len)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct vpos_t *pos = vrec_fpos(xioq);
    int32_t *buf = NULL;

    if ((pos->boff + len) <= pos->vrec->size) {
      buf = (int32_t *)(void *)(pos->vrec->base + pos->vrec->off);
      pos->vrec->off += len;
      pos->vrec->len += len;
      pos->boff += len;
      pos->loff += len;
      if (pos->loff > xioq->ioq.frag_len)
	xioq->ioq.frag_len = pos->loff;
    }

    return (buf);
}

static void
xdr_ioq_destroy(XDR *xdrs)
{
    struct xdr_ioq *xioq = (struct xdr_ioq *) xdrs->x_private;
    struct v_rec *vrec;

    /* release queued buffers */
    while (xioq->ioq.size > 0) {
        vrec = TAILQ_FIRST(&xioq->ioq.q);
        TAILQ_REMOVE(&xioq->ioq.q, vrec, ioq);
        (xioq->ioq.size)--;
        vrec_rele(xioq, vrec);
    }
    mem_free(xioq, sizeof(struct xdr_ioq));
}

static bool
xdr_ioq_control(XDR *xdrs, /* const */ int rq, void *in)
{
    return (TRUE);
}

static bool
xdr_ioq_noop(void)
{
    return (FALSE);
}
