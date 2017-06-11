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
#include <misc/abstract_atomic.h>
#include "rpc_com.h"

#include <rpc/xdr_ioq.h>

static bool xdr_ioq_noop(void) __attribute__ ((unused));

#define VREC_MAXBUFS 24

static uint64_t next_id;

#if 0				/* jemalloc docs warn about reclaim */
#define alloc_buffer(size) mem_aligned(0x8, (size))
#else
#define alloc_buffer(size) mem_alloc((size))
#endif				/* 0 */
#define free_buffer(addr,size) mem_free((addr), size)

struct xdr_ioq_uv *
xdr_ioq_uv_create(size_t size, u_int uio_flags)
{
	struct xdr_ioq_uv *uv = mem_zalloc(sizeof(struct xdr_ioq_uv));

	if (size) {
		uv->v.vio_base = alloc_buffer(size);
		uv->v.vio_head = uv->v.vio_base;
		uv->v.vio_tail = uv->v.vio_base;
		uv->v.vio_wrap = uv->v.vio_base + size;
		/* ensure not wrapping to zero */
		assert(uv->v.vio_base < uv->v.vio_wrap);
	}
	uv->u.uio_flags = uio_flags;
	uv->u.uio_references = 1;	/* starting one */

	return (uv);
}

struct poolq_entry *
xdr_ioq_uv_fetch(struct xdr_ioq *xioq, struct poolq_head *ioqh,
		 char *comment, u_int count, u_int ioq_flags)
{
	struct poolq_entry *have = NULL;

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() %u %s",
		__func__, count, comment);
	pthread_mutex_lock(&ioqh->qmutex);

	while (count--) {
		if (likely(0 < ioqh->qcount--)) {
			/* positive for buffer(s) */
			have = TAILQ_FIRST(&ioqh->qh);
			TAILQ_REMOVE(&ioqh->qh, have, q);

			/* added directly to the queue.
			 * this lock is needed for context header queues,
			 * but is not a burden on uncontested data queues.
			 */
			pthread_mutex_lock(&xioq->ioq_uv.uvqh.qmutex);
			(xioq->ioq_uv.uvqh.qcount)++;
			TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, have, q);
			pthread_mutex_unlock(&xioq->ioq_uv.uvqh.qmutex);
		} else {
			u_int saved = xioq->xdrs[0].x_handy;

			/* negative for waiting worker(s):
			 * use the otherwise empty pool to hold them,
			 * simplifying mutex and pointer setup.
			 */
			TAILQ_INSERT_TAIL(&ioqh->qh, &xioq->ioq_s, q);

			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() waiting for %u %s",
				__func__, count, comment);

			/* Note: the mutex is the pool _head,
			 * but the condition is per worker,
			 * making the signal efficient!
			 *
			 * Nota Bene: count was already decremented,
			 * will be zero for last one needed,
			 * then will wrap as unsigned.
			 */
			xioq->xdrs[0].x_handy = count;
			pthread_cond_wait(&xioq->ioq_cond, &ioqh->qmutex);
			xioq->xdrs[0].x_handy = saved;

			/* entry was already added directly to the queue */
			have = TAILQ_LAST(&xioq->ioq_uv.uvqh.qh, q_head);
		}
	}

	pthread_mutex_unlock(&ioqh->qmutex);
	return have;
}

struct poolq_entry *
xdr_ioq_uv_fetch_nothing(struct xdr_ioq *xioq, struct poolq_head *ioqh,
			 char *comment, u_int count, u_int ioq_flags)
{
	return NULL;
}

static inline void
xdr_ioq_uv_recycle(struct poolq_head *ioqh, struct poolq_entry *have)
{
	pthread_mutex_lock(&ioqh->qmutex);

	if (likely(0 <= ioqh->qcount++)) {
		/* positive for buffer(s) */
		TAILQ_INSERT_TAIL(&ioqh->qh, have, q);
	} else {
		/* negative for waiting worker(s) */
		struct xdr_ioq *wait = _IOQ(TAILQ_FIRST(&ioqh->qh));

		/* added directly to the queue.
		 * no need to lock here, the mutex is the pool _head.
		 */
		(wait->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&wait->ioq_uv.uvqh.qh, have, q);

		/* Nota Bene: x_handy was decremented count,
		 * will be zero for last one needed,
		 * then will wrap as unsigned.
		 */
		if (0 < wait->xdrs[0].x_handy--) {
			/* not removed */
			ioqh->qcount--;
		} else {
			TAILQ_REMOVE(&ioqh->qh, &wait->ioq_s, q);
			pthread_cond_signal(&wait->ioq_cond);
		}
	}

	pthread_mutex_unlock(&ioqh->qmutex);
}

void
xdr_ioq_uv_release(struct xdr_ioq_uv *uv)
{
	if (uv->u.uio_refer) {
		/* not optional in this case! */
		uv->u.uio_refer->uio_release(uv->u.uio_refer, UIO_FLAG_NONE);
		uv->u.uio_refer = NULL;
	}

	if (!(--uv->u.uio_references)) {
		if (uv->u.uio_release) {
			/* handle both xdr_ioq_uv and vio */
			uv->u.uio_release(&uv->u, UIO_FLAG_NONE);
		} else if (uv->u.uio_flags & UIO_FLAG_FREE) {
			free_buffer(uv->v.vio_base, ioquv_size(uv));
			mem_free(uv, sizeof(*uv));
		} else if (uv->u.uio_flags & UIO_FLAG_BUFQ) {
			uv->u.uio_references = 1;	/* keeping one */
			xdr_ioq_uv_recycle(uv->u.uio_p1, &uv->uvq);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() memory leak, no release flags (%u)\n",
				__func__, uv->u.uio_flags);
			abort();
		}
	}
}

/*
 * Set current read/insert or fill position.
 */
static inline void
xdr_ioq_uv_reset(struct xdr_ioq *xioq, struct xdr_ioq_uv *uv)
{
	xioq->xdrs[0].x_data = uv->v.vio_head;
	xioq->xdrs[0].x_base = &uv->v;
	xioq->xdrs[0].x_v = uv->v;
}

/*
 * Update read/insert or fill position.
 */
static inline void
xdr_ioq_uv_update(struct xdr_ioq *xioq, struct xdr_ioq_uv *uv)
{
	xdr_ioq_uv_reset(xioq, uv);
	(xioq->ioq_uv.pcount)++;
	/* xioq->ioq_uv.plength is calculated in xdr_ioq_uv_advance() */
}

/*
 * Set initial read/insert or fill position.
 *
 * Note: must be done before any XDR_[GET|SET]POS()
 */
void
xdr_ioq_reset(struct xdr_ioq *xioq, u_int wh_pos)
{
	struct xdr_ioq_uv *uv = IOQ_(TAILQ_FIRST(&xioq->ioq_uv.uvqh.qh));

	xioq->ioq_uv.plength =
	xioq->ioq_uv.pcount = 0;

	if (wh_pos >= ioquv_size(uv)) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xioq %p wh_pos %d too big, ignored!\n",
			__func__, xioq, wh_pos);
	} else {
		uv->v.vio_head = uv->v.vio_base + wh_pos;
	}
	xdr_ioq_uv_reset(xioq, uv);

	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() xioq %p head %p wh_pos %d",
		__func__, xioq, uv->v.vio_head, wh_pos);
}

void
xdr_ioq_setup(struct xdr_ioq *xioq)
{
	XDR *xdrs = xioq->xdrs;

	/* the XDR is the top element of struct xdr_ioq */
	assert((void *)xdrs == (void *)xioq);

	TAILQ_INIT_ENTRY(&xioq->ioq_s, q);
	xioq->ioq_s.qflags = IOQ_FLAG_SEGMENT;

	poolq_head_setup(&xioq->ioq_uv.uvqh);
	pthread_cond_init(&xioq->ioq_cond, NULL);

	xdrs->x_ops = &xdr_ioq_ops;
	xdrs->x_op = XDR_ENCODE;
	xdrs->x_public = NULL;
	xdrs->x_private = NULL;
	xdrs->x_data = NULL;
	xdrs->x_base = NULL;
	xdrs->x_flags = XDR_FLAG_VIO;

	xioq->id = atomic_inc_uint64_t(&next_id);
}

struct xdr_ioq *
xdr_ioq_create(size_t min_bsize, size_t max_bsize, u_int uio_flags)
{
	struct xdr_ioq *xioq = mem_zalloc(sizeof(struct xdr_ioq));

	xdr_ioq_setup(xioq);
	xioq->xdrs[0].x_flags |= XDR_FLAG_FREE;
	xioq->ioq_uv.min_bsize = min_bsize;
	xioq->ioq_uv.max_bsize = max_bsize;

	if (!(uio_flags & UIO_FLAG_BUFQ)) {
		struct xdr_ioq_uv *uv = xdr_ioq_uv_create(min_bsize, uio_flags);
		xioq->ioq_uv.uvqh.qcount = 1;
		TAILQ_INSERT_HEAD(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
		xdr_ioq_reset(xioq, 0);
	}

	return (xioq);
}

/*
 * Advance read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 * All such updates are consolidated here and getpos/setpos,
 * reducing computations in the get/put/inline routines.
 */
static inline struct xdr_ioq_uv *
xdr_ioq_uv_advance(struct xdr_ioq *xioq)
{
	struct xdr_ioq_uv *uv = IOQV(xioq->xdrs[0].x_base);
	size_t len;

	/* update the most recent data length */
	xdr_tail_update(xioq->xdrs);

	len = ioquv_length(uv);
	xioq->ioq_uv.plength += len;

	/* next buffer, if any */
	return IOQ_(TAILQ_NEXT(&uv->uvq, q));
}

/*
 * Append at read/insert or fill position.
 */
static struct xdr_ioq_uv *
xdr_ioq_uv_append(struct xdr_ioq *xioq, u_int ioq_flags)
{
	struct xdr_ioq_uv *uv = IOQV(xioq->xdrs[0].x_base);

	if (xioq->ioq_uv.uvq_fetch) {
		/* more of the same kind */
		struct poolq_entry *have =
			xioq->ioq_uv.uvq_fetch(xioq, uv->u.uio_p1,
						"next buffer", 1,
						IOQ_FLAG_NONE);

		/* poolq_entry is the top element of xdr_ioq_uv */
		uv = IOQ_(have);
		assert((void *)uv == (void *)have);
	} else if (ioq_flags & IOQ_FLAG_BALLOC) {
		/* XXX workaround for lack of segmented buffer
		 * interfaces in some callers (e.g, GSS_WRAP) */
		if (uv->u.uio_flags & UIO_FLAG_REALLOC) {
			void *base;
			size_t size = ioquv_size(uv);
			size_t delta = xdr_tail_inline(xioq->xdrs);
			size_t len = ioquv_length(uv);

			/* bail if we have reached max bufsz */
			if (size >= xioq->ioq_uv.max_bsize)
				return (NULL);

			/* backtrack */
			xioq->ioq_uv.plength -= len;
			assert(uv->u.uio_flags & UIO_FLAG_FREE);

			base = mem_alloc(xioq->ioq_uv.max_bsize);
			memcpy(base, uv->v.vio_head, len);
			mem_free(uv->v.vio_base, size);
			uv->v.vio_base =
			uv->v.vio_head = base + 0;
			uv->v.vio_tail = base + len;
			uv->v.vio_wrap = base + xioq->ioq_uv.max_bsize;
			xioq->xdrs[0].x_v = uv->v;
			xioq->xdrs[0].x_data = uv->v.vio_tail - delta;
			return (uv);
		}
		uv = xdr_ioq_uv_create(xioq->ioq_uv.min_bsize, UIO_FLAG_FREE);
		(xioq->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
	} else {
		/* XXX empty buffer slot (not supported for now) */
		uv = xdr_ioq_uv_create(0, UIO_FLAG_NONE);
		(xioq->ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_TAIL(&xioq->ioq_uv.uvqh.qh, &uv->uvq, q);
	}

	xdr_ioq_uv_update(xioq, uv);
	return (uv);
}

static bool
xdr_ioq_getlong(XDR *xdrs, long *lp)
{
	struct xdr_ioq_uv *uv;
	void *future = xdrs->x_data + sizeof(uint32_t);

	while (future > xdrs->x_v.vio_tail) {
		if (unlikely(xdrs->x_data != xdrs->x_v.vio_tail)) {
			/* FIXME: insufficient data or unaligned? stop! */
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() x_data != x_v.vio_tail\n",
				__func__);
			return (false);
		}

		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv) {
			return (false);
		}
		xdr_ioq_uv_update(XIOQ(xdrs), uv);
		/* fill pointer has changed */
		future = xdrs->x_data + sizeof(uint32_t);
	}

	*lp = (long)ntohl(*((uint32_t *) (xdrs->x_data)));
	xdrs->x_data = future;
	return (true);
}

static bool
xdr_ioq_putlong(XDR *xdrs, const long *lp)
{
	struct xdr_ioq_uv *uv;
	void *future = xdrs->x_data + sizeof(uint32_t);

	while (future > xdrs->x_v.vio_wrap) {
		/* advance fill pointer, skipping unaligned */
		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv) {
			uv = xdr_ioq_uv_append(XIOQ(xdrs), IOQ_FLAG_BALLOC);
		} else {
			xdr_ioq_uv_update(XIOQ(xdrs), uv);
		}
		/* fill pointer has changed */
		future = xdrs->x_data + sizeof(uint32_t);
	}

	*((int32_t *) (xdrs->x_data)) = (int32_t) htonl((int32_t) (*lp));
	xdrs->x_data = future;
	return (true);
}

/* in glibc 2.14+ x86_64, memcpy no longer tries to handle overlapping areas,
 * see Fedora Bug 691336 (NOTABUG); we dont permit overlapping segments,
 * so memcpy may be a small win over memmove.
 */

static bool
xdr_ioq_getbytes(XDR *xdrs, char *addr, u_int len)
{
	struct xdr_ioq_uv *uv;
	ssize_t delta;

	while (len > 0
		&& XIOQ(xdrs)->ioq_uv.pcount < XIOQ(xdrs)->ioq_uv.uvqh.qcount) {
		delta = (uintptr_t)xdrs->x_v.vio_tail
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (unlikely(!delta)) {
			/* advance fill pointer */
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv) {
				return (false);
			}
			xdr_ioq_uv_update(XIOQ(xdrs), uv);
			continue;
		}
		memcpy(addr, xdrs->x_data, delta);
		xdrs->x_data += delta;
		addr += delta;
		len -= delta;
	}

	/* assert(len == 0); */
	return (true);
}

static bool
xdr_ioq_putbytes(XDR *xdrs, const char *addr, u_int len)
{
	struct xdr_ioq_uv *uv;
	ssize_t delta;

	while (len > 0) {
		delta = (uintptr_t)xdrs->x_v.vio_wrap
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (!delta) {
			/* advance fill pointer */
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv) {
				uv = xdr_ioq_uv_append(XIOQ(xdrs),
							IOQ_FLAG_BALLOC);
			} else {
				xdr_ioq_uv_update(XIOQ(xdrs), uv);
			}
			continue;
		}
		memcpy(xdrs->x_data, addr, delta);
		xdrs->x_data += delta;
		addr += delta;
		len -= delta;
	}
	return (true);
}

/* Get buffers from the queue. */
static bool
xdr_ioq_getbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
    /* XXX finalize */
#if 0

	struct xdr_ioq_uv *uv;
	ssize_t delta;
	int ix;

	/* allocate sufficient slots to empty the queue, else MAX */
	uio->xbs_cnt = XIOQ(xdrs)->ioq_uv.uvqh.qsize - XIOQ(xdrs)->ioq_uv.pcount;
	if (uio->xbs_cnt > VREC_MAXBUFS) {
		uio->xbs_cnt = VREC_MAXBUFS;
	}

	/* fail if no segments available */
	if (unlikely(! uio->xbs_cnt))
		return (FALSE);

	uio->xbs_buf = mem_alloc(uio->xbs_cnt);
	uio->xbs_resid = 0;
	ix = 0;

	/* re-consuming bytes in a stream (after SETPOS/rewind) */
	while (len > 0
		&& XIOQ(xdrs)->ioq_uv.pcount < XIOQ(xdrs)->ioq_uv.uvqh.qcount) {
		delta = (uintptr_t)XDR_VIO(xioq->xdrs)->vio_tail
			- (uintptr_t)xdrs->x_data;

		if (unlikely(delta > len)) {
			delta = len;
		} else if (unlikely(!delta)) {
			uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv)
				return (false);

			xdr_ioq_uv_update(XIOQ(xdrs), uv);
			continue;
		}
		(uio->xbs_buf[ix]).xb_p1 = uv;
		uv->u.uio_references)++;
		(uio->xbs_buf[ix]).xb_base = xdrs->x_data;
		XIOQ(xdrs)->ioq_uv.plength += delta;
		xdrs->x_data += delta;
		len -= delta;
	}
#endif /* 0 */

	/* assert(len == 0); */
	return (TRUE);
}

/* Post buffers on the queue, or, if indicated in flags, return buffers
 * referenced with getbufs. */
static bool
xdr_ioq_putbufs(XDR *xdrs, xdr_uio *uio, u_int flags)
{
	struct xdr_ioq_uv *uv;
	xdr_vio *v;
	int ix;

	for (ix = 0; ix < uio->uio_count; ++ix) {
		/* advance fill pointer, do not allocate buffers, refs =1 */
		uv = xdr_ioq_uv_advance(XIOQ(xdrs));
		if (!uv)
			uv = xdr_ioq_uv_append(XIOQ(xdrs), flags);
		else
			xdr_ioq_uv_update(XIOQ(xdrs), uv);

		v = &(uio->uio_vio[ix]);
		uv->u.uio_flags = UIO_FLAG_NONE; /* !RECLAIM */
		uv->v = *v;

#if 0
Saved for later golden buttery results -- Matt
	if (flags & XDR_PUTBUFS_FLAG_BRELE) {
		/* the caller is returning buffers */
		for (ix = 0; ix < uio->xbs_cnt; ++ix) {
			uv = (struct xdr_ioq_uv *)(uio->xbs_buf[ix]).xb_p1;
			xdr_ioq_uv_release(uv);
		}
		mem_free(uio->xbs_buf, 0);
		break;
	} else {
		for (ix = 0; ix < uio->xbs_cnt; ++ix) {
			/* advance fill pointer, do not allocate buffers */
			*uv = xdr_ioq_uv_advance(XIOQ(xdrs));
			if (!uv)
				uv = xdr_ioq_uv_append(XIOQ(xdrs),
							IOQ_FLAG_NONE);
			else
				xdr_ioq_uv_update(XIOQ(xdrs), uv);

			xbuf = &(uio->xbs_buf[ix]);
			XIOQ(xdrs)->ioq_uv.plength += xbuf->xb_len;
			uv->u.uio_flags = UIO_FLAG_NONE;	/* !RECLAIM */
			uv->u.uio_references =
			    (xbuf->xb_flags & UIO_FLAG_GIFT) ? 0 : 1;
			uv->v.vio_base = xbuf->xb_base;
			uv->v.vio_wrap = xbuf->xb_len;
			uv->v.vio_tail = xbuf->xb_len;
			uv->v.vio_head = 0;
		}
	}
#endif
		/* save original buffer sequence for rele */
		if (ix == 0) {
			uv->u.uio_refer = uio;
			(uio->uio_references)++;
		}
	}

	return (TRUE);
}

/*
 * Get read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 */
static u_int
xdr_ioq_getpos(XDR *xdrs)
{
	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	return (XIOQ(xdrs)->ioq_uv.plength
		+ ((uintptr_t)xdrs->x_data
		   - (uintptr_t)xdrs->x_v.vio_head));
}

/*
 * Set read/insert or fill position.
 *
 * Update the logical and physical offsets and lengths,
 * based upon the most recent position information.
 */
static bool
xdr_ioq_setpos(XDR *xdrs, u_int pos)
{
	struct poolq_entry *have;

	/* update the most recent data length, just in case */
	xdr_tail_update(xdrs);

	XIOQ(xdrs)->ioq_uv.plength =
	XIOQ(xdrs)->ioq_uv.pcount = 0;

	TAILQ_FOREACH(have, &(XIOQ(xdrs)->ioq_uv.uvqh.qh), q) {
		struct xdr_ioq_uv *uv = IOQ_(have);
		u_int len = ioquv_length(uv);
		u_int full = (uintptr_t)xdrs->x_v.vio_wrap
			   - (uintptr_t)xdrs->x_v.vio_head;

		if (pos <= full) {
			/* allow up to the end of the buffer,
			 * assuming next operation will extend.
			 */
			xdrs->x_data = uv->v.vio_head + pos;
			xdrs->x_base = &uv->v;
			xdrs->x_v = uv->v;
			return (true);
		}
		pos -= len;
		XIOQ(xdrs)->ioq_uv.plength += len;
		XIOQ(xdrs)->ioq_uv.pcount++;
	}

	return (false);
}

static int32_t *
xdr_ioq_inline(XDR *xdrs, u_int len)
{
	/* bugfix:  return fill pointer, not head! */
	int32_t *buf = (int32_t *)xdrs->x_data;
	void *future = xdrs->x_data + len;

	/* bugfix: do not move fill position beyond tail or wrap */
	switch (xdrs->x_op) {
	case XDR_ENCODE:
		if (future <= xdrs->x_v.vio_wrap) {
			/* bugfix:  do not move head! */
			xdrs->x_data = future;
			/* bugfix: do not move tail beyond pfoff or wrap! */
			xdr_tail_update(xdrs);
			return (buf);
		}
		break;
	case XDR_DECODE:
		/* re-consuming bytes in a stream
		 * (after SETPOS/rewind) */
		if (future <= xdrs->x_v.vio_tail) {
			/* bugfix:  do not move head! */
			xdrs->x_data = future;
			/* bugfix:  do not move tail! */
			return (buf);
		}
		break;
	default:
		abort();
		break;
	};

	return (NULL);
}

void
xdr_ioq_release(struct poolq_head *ioqh)
{
	struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

	/* release queued buffers */
	while (have) {
		struct poolq_entry *next = TAILQ_NEXT(have, q);

		TAILQ_REMOVE(&ioqh->qh, have, q);
		(ioqh->qcount)--;

		if (have->qflags & IOQ_FLAG_SEGMENT) {
			xdr_ioq_destroy(_IOQ(have), have->qsize);
		} else {
			xdr_ioq_uv_release(IOQ_(have));
		}
		have = next;
	}
	assert(ioqh->qcount == 0);
}

void
xdr_ioq_destroy(struct xdr_ioq *xioq, size_t qsize)
{
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"%s() xioq %p",
		__func__, xioq);

	xdr_ioq_release(&xioq->ioq_uv.uvqh);

	if (xioq->ioq_pool) {
		xdr_ioq_uv_recycle(xioq->ioq_pool, &xioq->ioq_s);
		return;
	}
	poolq_head_destroy(&xioq->ioq_uv.uvqh);

	if (xioq->xdrs[0].x_flags & XDR_FLAG_FREE) {
		mem_free(xioq, qsize);
	}
}

static void
xdr_ioq_destroy_internal(XDR *xdrs)
{
	xdr_ioq_destroy(XIOQ(xdrs), sizeof(struct xdr_ioq));
}

void
xdr_ioq_destroy_pool(struct poolq_head *ioqh)
{
	struct poolq_entry *have = TAILQ_FIRST(&ioqh->qh);

	while (have) {
		struct poolq_entry *next = TAILQ_NEXT(have, q);

		TAILQ_REMOVE(&ioqh->qh, have, q);
		(ioqh->qcount)--;

		_IOQ(have)->ioq_pool = NULL;
		xdr_ioq_destroy(_IOQ(have), have->qsize);
		have = next;
	}
	assert(ioqh->qcount == 0);
	poolq_head_destroy(ioqh);
}

static bool
xdr_ioq_control(XDR *xdrs, /* const */ int rq, void *in)
{
	return (true);
}

static bool
xdr_ioq_noop(void)
{
	return (false);
}

const struct xdr_ops xdr_ioq_ops = {
	xdr_ioq_getlong,
	xdr_ioq_putlong,
	xdr_ioq_getbytes,
	xdr_ioq_putbytes,
	xdr_ioq_getpos,
	xdr_ioq_setpos,
	xdr_ioq_inline,
	xdr_ioq_destroy_internal,
	xdr_ioq_control,
	xdr_ioq_getbufs,
	xdr_ioq_putbufs
};
