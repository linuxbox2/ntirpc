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
#include <sys/poll.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif				/* PORTMAP */
#include <rpc/svc_rqst.h>

#include "rpc_com.h"

#include <rpc/svc.h>
#include <misc/rbtree_x.h>
#include <misc/opr_queue.h>
#include "clnt_internal.h"
#include "svc_internal.h"
#include <rpc/svc_rqst.h>
#include "svc_xprt.h"

/*
 * The TI-RPC instance should be able to reach every registered
 * handler, and potentially each SVCXPRT registered on it.
 *
 * Each SVCXPRT points to its own handler, however, so operations to
 * block/unblock events (for example) given an existing xprt handle
 * are O(1) without any ordered or hashed representation.
 */

#define SVC_RQST_PARTITIONS 7

static bool initialized;

struct svc_rqst_set {
	mutex_t mtx;
	struct rbtree_x xt;
	uint32_t next_id;
};

static struct svc_rqst_set svc_rqst_set = {
	MUTEX_INITIALIZER,	/* mtx */
	{
	 0,			/* npart */
	 RBT_X_FLAG_NONE,	/* flags */
	 23,			/* cachesz */
	 NULL			/* tree */
	},			/* xt */
	0			/* next_id */
};

extern struct svc_params __svc_params[1];

#define cond_init_svc_rqst() { \
		do { \
			if (!initialized) \
				svc_rqst_init(); \
		} while (0); \
	}

struct svc_rqst_rec {
	struct opr_rbtree_node node_k;
	TAILQ_HEAD(evq_head, rpc_svcxprt) xprt_q;	/* xprt handles */
	pthread_mutex_t mtx;
	void *u_data;		/* user-installable opaque data */
	uint64_t gen;		/* generation number */

	int sv[2];
	uint32_t id_k;		/* chan id */
	uint32_t states;
	uint32_t signals;
	uint32_t refcnt;
	uint16_t flags;

	/*
	 * union of event processor types
	 */
	enum svc_event_type ev_type;
	union {
#if defined(TIRPC_EPOLL)
		struct {
			int epoll_fd;
			struct epoll_event ctrl_ev;
			struct epoll_event *events;
			u_int max_events;	/* max epoll events */
		} epoll;
#endif
		struct {
			fd_set set;	/* select/fd_set (currently unhooked) */
		} fd;
	} ev_u;
};

static inline int rqst_thrd_cmpf(const struct opr_rbtree_node *lhs,
				 const struct opr_rbtree_node *rhs)
{
	struct svc_rqst_rec *lk, *rk;

	lk = opr_containerof(lhs, struct svc_rqst_rec, node_k);
	rk = opr_containerof(rhs, struct svc_rqst_rec, node_k);

	if (lk->id_k < rk->id_k)
		return (-1);

	if (lk->id_k == rk->id_k)
		return (0);

	return (1);
}

/* forward declaration in lieu of moving code {WAS} */

static int
svc_rqst_evchan_unreg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags);

static int svc_rqst_unhook_events(SVCXPRT *, struct svc_rqst_rec *);
static int svc_rqst_hook_events(SVCXPRT *, struct svc_rqst_rec *);

static inline void
SetNonBlock(int fd)
{
	int s_flags = fcntl(fd, F_GETFL, 0);
	(void)fcntl(fd, F_SETFL, (s_flags | O_NONBLOCK));
}

void
svc_rqst_init()
{
	int ix, code = 0;

	mutex_lock(&svc_rqst_set.mtx);

	if (initialized)
		goto unlock;

	code = rbtx_init(&svc_rqst_set.xt, rqst_thrd_cmpf, SVC_RQST_PARTITIONS,
			 RBT_X_FLAG_ALLOC | RBT_X_FLAG_CACHE_RT);
	if (code)
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST, "%s: rbtx_init failed",
			__func__);

	/* init read-through cache */
	for (ix = 0; ix < SVC_RQST_PARTITIONS; ++ix) {
		struct rbtree_x_part *xp = &(svc_rqst_set.xt.tree[ix]);

		xp->cache = mem_calloc(svc_rqst_set.xt.cachesz,
					sizeof(struct opr_rbtree_node *));
	}
	initialized = true;

 unlock:
	mutex_unlock(&svc_rqst_set.mtx);
}

void
svc_rqst_init_xprt(SVCXPRT *xprt)
{
	/* reachable */
	svc_xprt_set(xprt, SVC_XPRT_FLAG_UNLOCK);
	/* !!! not checking for duplicate xp_fd ??? */
}

/**
 * @brief Lookup a channel
 * @note Locking
 * - SVC_RQST_FLAG_PART_UNLOCK - Unlock the tree partition before returning.
 *   Otherwise, it is returned locked.
 * - SVC_RQST_FLAG_SREC_LOCKED - The sr_rec is already locked; don't lock it
 *   again.
 * - SVC_RQST_FLAG_SREC_UNLOCK - Unlock the sr_rec before returning.  Otherwise,
 *   it is returned locked.
 */
static inline struct svc_rqst_rec *
svc_rqst_lookup_chan(uint32_t chan_id, struct rbtree_x_part **ref_t,
		     uint32_t flags)
{
	struct svc_rqst_rec trec;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *ns;
	struct svc_rqst_rec *sr_rec = NULL;

	cond_init_svc_rqst();

	trec.id_k = chan_id;
	t = rbtx_partition_of_scalar(&svc_rqst_set.xt, trec.id_k);
	*ref_t = t;

	mutex_lock(&t->mtx);

	ns = rbtree_x_cached_lookup(&svc_rqst_set.xt, t, &trec.node_k,
				    trec.id_k);
	if (ns) {
		sr_rec = opr_containerof(ns, struct svc_rqst_rec, node_k);
		if (!(flags & SVC_RQST_FLAG_SREC_LOCKED))
			mutex_lock(&sr_rec->mtx);
		++(sr_rec->refcnt);
		if (flags & SVC_RQST_FLAG_SREC_UNLOCK)
			mutex_unlock(&sr_rec->mtx);
	}

	if (flags & SVC_RQST_FLAG_PART_UNLOCK)
		mutex_unlock(&t->mtx);

	return (sr_rec);
}

int
svc_rqst_new_evchan(uint32_t *chan_id /* OUT */, void *u_data, uint32_t flags)
{
	uint32_t n_id;
	struct svc_rqst_rec *sr_rec;
	struct rbtree_x_part *t;
	bool rslt __attribute__ ((unused));
	int code = 0;

	cond_init_svc_rqst();

	flags |= SVC_RQST_FLAG_EPOLL;	/* XXX */

	sr_rec = mem_zalloc(sizeof(struct svc_rqst_rec));

	/* create a pair of anonymous sockets for async event channel wakeups */
	code = socketpair(AF_UNIX, SOCK_STREAM, 0, sr_rec->sv);
	if (code) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: failed creating event signal socketpair",
			__func__);
		mem_free(sr_rec, sizeof(struct svc_rqst_rec));
		goto out;
	}

	/* set non-blocking */
	SetNonBlock(sr_rec->sv[0]);
	SetNonBlock(sr_rec->sv[1]);

#if defined(TIRPC_EPOLL)
	if (flags & SVC_RQST_FLAG_EPOLL) {

		sr_rec->flags = flags & SVC_RQST_FLAG_MASK;

		sr_rec->ev_type = SVC_EVENT_EPOLL;

		/* XXX improve this too */
		sr_rec->ev_u.epoll.max_events =
		    __svc_params->ev_u.evchan.max_events;
		sr_rec->ev_u.epoll.events = (struct epoll_event *)
		    mem_alloc(sr_rec->ev_u.epoll.max_events *
			      sizeof(struct epoll_event));

		/* create epoll fd */
		sr_rec->ev_u.epoll.epoll_fd =
		    epoll_create_wr(sr_rec->ev_u.epoll.max_events,
				    EPOLL_CLOEXEC);

		if (sr_rec->ev_u.epoll.epoll_fd == -1) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: epoll_create failed (%d)", __func__,
				errno);
			mem_free(sr_rec->ev_u.epoll.events,
				 sr_rec->ev_u.epoll.max_events *
				 sizeof(struct epoll_event));
			mem_free(sr_rec, sizeof(struct svc_rqst_rec));
			code = EINVAL;
			goto out;
		}

		/* permit wakeup of threads blocked in epoll_wait, with a
		 * couple of possible semantics */
		sr_rec->ev_u.epoll.ctrl_ev.events =
		    EPOLLIN | EPOLLRDHUP;
		sr_rec->ev_u.epoll.ctrl_ev.data.fd = sr_rec->sv[1];
		code =
		    epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_ADD,
			      sr_rec->sv[1], &sr_rec->ev_u.epoll.ctrl_ev);
		if (code == -1)
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: add control socket failed (%d)", __func__,
				errno);
	} else {
		/* legacy fdset (currently unhooked) */
		sr_rec->ev_type = SVC_EVENT_FDSET;
	}
#else
	sr_rec->ev_type = SVC_EVENT_FDSET;
#endif

	mutex_lock(&svc_rqst_set.mtx);
	n_id = ++(svc_rqst_set.next_id);
	mutex_unlock(&svc_rqst_set.mtx);

	sr_rec->id_k = n_id;
	sr_rec->states = SVC_RQST_STATE_NONE;
	sr_rec->u_data = u_data;
	sr_rec->refcnt = 1;	/* svc_rqst_set ref */
	sr_rec->gen = 0;
	mutex_init(&sr_rec->mtx, NULL);
	TAILQ_INIT(&sr_rec->xprt_q);

	t = rbtx_partition_of_scalar(&svc_rqst_set.xt, sr_rec->id_k);
	mutex_lock(&t->mtx);
	rslt =
	    rbtree_x_cached_insert(&svc_rqst_set.xt, t, &sr_rec->node_k,
				   sr_rec->id_k);
	mutex_unlock(&t->mtx);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: create evchan %d socketpair %d:%d", __func__, n_id,
		sr_rec->sv[0], sr_rec->sv[1]);

	*chan_id = n_id;

 out:
	return (code);
}

/*
 * @note Lock flags
 * - Locks xprt unless SVC_RQST_FLAG_LOCKED is passed
 * - Locks sr_rec unless RVC_RQST_FLAG_SREC_LOCKED is passed
 * - Returns with xprt locked unless SVC_RQST_FLAG_UNLOCK is passed
 * - Returns with sr_rec locked unless SVC_RQST_FLAG_SREC_UNLOCKED is passed
 */
static inline void
evchan_unreg_impl(struct svc_rqst_rec *sr_rec, SVCXPRT *xprt, uint32_t flags)
{
	if (!(flags & SVC_RQST_FLAG_SREC_LOCKED))
		mutex_lock(&sr_rec->mtx);

	if (!(flags & SVC_RQST_FLAG_LOCKED))
		mutex_lock(&xprt->xp_lock);

	TAILQ_REMOVE(&sr_rec->xprt_q, xprt, xp_evq);

	/* clear events */
	(void)svc_rqst_unhook_events(xprt, sr_rec);	/* both LOCKED */

	/* unlink from xprt */
	xprt->xp_ev = NULL;

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s: %p xp_refs %" PRIu32
		" after remove, before channel release",
		__func__, xprt, xprt->xp_refs);

	if (flags & SVC_RQST_FLAG_UNLOCK)
		mutex_unlock(&xprt->xp_lock);

	if (flags & SVC_RQST_FLAG_SREC_UNLOCK)
		mutex_unlock(&sr_rec->mtx);
}

/*
 * Write 4-byte value to shared event-notification channel.  The
 * value as presently implemented can be interpreted only by one consumer,
 * so is not relied on.
 */
static inline void
ev_sig(int fd, uint32_t sig)
{
	int code = write(fd, &sig, sizeof(uint32_t));
	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST, "%s: fd %d sig %d", __func__, fd,
		sig);
	if (code < 1)
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: error writing to event socket (%d:%d)", __func__,
			code, errno);
}

/*
 * Read a single 4-byte value from the shared event-notification channel,
 * the socket is in non-blocking mode.  The value read is returned.
 */
static inline uint32_t
consume_ev_sig_nb(int fd)
{
	uint32_t sig = 0;
	int code __attribute__ ((unused));

	code = read(fd, &sig, sizeof(uint32_t));
	return (sig);
}

/**
 * Release a request
 * @note Locking
 * - sr_req is locked unless SVC_RQST_FLAG_SREC_LOCKED is passed
 * - sr_req is unlocked unless SR_REQ_RELEASE_KEEP_LOCKED is passed
 */
static inline void
sr_rec_release(struct svc_rqst_rec *sr_rec, uint32_t flags)
{
	uint32_t refcnt;

	if (!(flags & SVC_RQST_FLAG_SREC_LOCKED))
		mutex_lock(&sr_rec->mtx);

	refcnt = --(sr_rec->refcnt);

	if (!(flags & SR_REQ_RELEASE_KEEP_LOCKED))
		mutex_unlock(&sr_rec->mtx);

	if (refcnt == 0) {
		if (flags & SR_REQ_RELEASE_KEEP_LOCKED)
			mutex_unlock(&sr_rec->mtx);
		/* assert sr_rec DESTROYED */
		mutex_destroy(&sr_rec->mtx);
		mem_free(sr_rec, sizeof(struct svc_rqst_rec));
	}
}

static int
svc_rqst_delete_evchan(uint32_t chan_id)
{
	struct svc_rqst_rec *sr_rec;
	struct rbtree_x_part *t;
	SVCXPRT *next;
	SVCXPRT *xprt = NULL;
	int code = 0;

	sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_XPRT_FLAG_NONE);
	if (!sr_rec) {
		mutex_unlock(&t->mtx);
		code = ENOENT;
		goto out;
	}

	/* traverse sr_req->xprt_q inorder */
	/* sr_rec LOCKED */
	xprt = TAILQ_FIRST(&sr_rec->xprt_q);

	while (xprt) {
		next = TAILQ_NEXT(xprt, xp_evq);

		/* indirect on xp_ev */
		/* stop processing events */
		evchan_unreg_impl(sr_rec, xprt, (SVC_RQST_FLAG_UNLOCK |
						 SVC_RQST_FLAG_SREC_LOCKED));

		/* wake up */
		ev_sig(sr_rec->sv[0], 0);
		switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
		case SVC_EVENT_EPOLL:
			code =
			    epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				      EPOLL_CTL_DEL, sr_rec->sv[1],
				      &sr_rec->ev_u.epoll.ctrl_ev);
			if (code == -1)
				__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
					"%s: epoll del control socket failed (%d)",
					__func__, errno);
			break;
#endif
		default:
			break;
		}

		xprt = next;
	}

	/* now remove sr_rec */
	rbtree_x_cached_remove(&svc_rqst_set.xt, t, &sr_rec->node_k,
			       sr_rec->id_k);
	mutex_unlock(&t->mtx);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
		close(sr_rec->ev_u.epoll.epoll_fd);
		mem_free(sr_rec->ev_u.epoll.events,
			 sr_rec->ev_u.epoll.max_events *
			 sizeof(struct epoll_event));
		break;
#endif
	default:
		/* XXX */
		break;
	}
	sr_rec->states = SVC_RQST_STATE_DESTROYED;
	sr_rec->id_k = 0;	/* no chan */
	/*	ref count here should be 2:
	 *	1	initial create/rbt ref we just deleted
	 *	+1	lookup (top of this routine through here)
	 * so, DROP one ref here so the final release will go to 0.
	 */
	--(sr_rec->refcnt);	/* DROP one extra ref - initial create */
	sr_rec_release(sr_rec, SVC_RQST_FLAG_SREC_LOCKED);

 out:
	return (code);
}

int
svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;
	struct rbtree_x_part *t;
	int code = 0;

	if (chan_id == 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: called with chan_id 0, fatal (bug)", __func__);
		goto out;
	}

	sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_XPRT_FLAG_NONE);
	if (!sr_rec) {
		mutex_unlock(&t->mtx);
		code = ENOENT;
		goto out;
	}

	mutex_lock(&xprt->xp_lock);

	if (flags & SVC_RQST_FLAG_XPRT_UREG) {
		if (chan_id != __svc_params->ev_u.evchan.id) {
			svc_rqst_evchan_unreg(__svc_params->ev_u.evchan.id,
					      xprt,
					      (SVC_RQST_FLAG_LOCKED |
					       SVC_RQST_FLAG_SREC_LOCKED));
			svc_xprt_clear(xprt, SVC_XPRT_FLAG_LOCKED);
		}
	}

	TAILQ_INSERT_TAIL(&sr_rec->xprt_q, xprt, xp_evq);

	/* link from xprt */
	xprt->xp_ev = sr_rec;

	/* register on event channel */
	(void)svc_rqst_hook_events(xprt, sr_rec);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s: pre channel %p xp_refs %" PRIu32,
		__func__, xprt, xprt->xp_refs);

	mutex_unlock(&xprt->xp_lock);

	sr_rec_release(sr_rec, SVC_RQST_FLAG_SREC_LOCKED);

	mutex_unlock(&t->mtx);
 out:
	return (code);
}

/**
 * Unregister an evchan
 * @note Locking
 * - Takes sr_req lock, unless SVC_RQST_FLAG_SREC_LOCKED is passed
 * - Takes xprt lock, unless SVC_RQST_FLAG_LOCKED is passed
 * - Returns with sr_req locked and xprt locked at all times
 */
static int
svc_rqst_evchan_unreg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;
	struct rbtree_x_part *t;
	int code = EINVAL;

	/* Don't let them force unlocking of the part; we need that */
	flags &= ~(SVC_RQST_FLAG_PART_UNLOCK | SVC_RQST_FLAG_SREC_UNLOCK);

	sr_rec = svc_rqst_lookup_chan(chan_id, &t, flags);
	if (!sr_rec) {
		code = ENOENT;
		goto unlock;
	}

	evchan_unreg_impl(sr_rec, xprt, (flags | SVC_RQST_FLAG_SREC_LOCKED));

 unlock:
	mutex_unlock(&t->mtx);

	if (sr_rec)
		sr_rec_release(sr_rec, SVC_RQST_FLAG_SREC_LOCKED |
			       SR_REQ_RELEASE_KEEP_LOCKED);

	return (code);
}

static int
svc_rqst_unhook_events(SVCXPRT *xprt /* LOCKED */ ,
		       struct svc_rqst_rec *sr_rec /* LOCKED */)
{
	int code;

	cond_init_svc_rqst();

	atomic_set_uint16_t_bits(&xprt->xp_flags, SVC_XPRT_FLAG_BLOCKED);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev = &xprt->ev_u.epoll.event;

		/* clear epoll vector */
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_DEL, xprt->xp_fd, ev);
		if (code) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p epoll del failed fd %d "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d, %d)",
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1],
				code, errno);
			code = errno;
		} else {
			atomic_clear_uint16_t_bits(&xprt->xp_flags,
						   SVC_XPRT_FLAG_ADDED);

			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p epoll del fd %d "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d, %d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1],
				code, errno);
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	return (0);
}

int
svc_rqst_rearm_events(SVCXPRT *xprt, uint32_t __attribute__ ((unused)) flags)
{
	struct svc_rqst_rec *sr_rec;
	int code;

	cond_init_svc_rqst();

	sr_rec = (struct svc_rqst_rec *)xprt->xp_ev;

	/* Don't rearm a destroyed (but not yet collected) xprx */
	if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)
		goto out;

	/* MUST follow the destroyed check above */
	assert(sr_rec);

	mutex_lock(&sr_rec->mtx);
	if (atomic_fetch_uint16_t(&xprt->xp_flags) & SVC_XPRT_FLAG_ADDED) {

		switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
		case SVC_EVENT_EPOLL:
		{
			struct epoll_event *ev = &xprt->ev_u.epoll.event;

			/* set up epoll user data */
			/* ev->data.ptr = xprt; *//* XXX already set */
			ev->events = EPOLLIN | EPOLLONESHOT;

			/* rearm in epoll vector */
			code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
					 EPOLL_CTL_MOD, xprt->xp_fd, ev);

			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p epoll arm fd %d "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d, %d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1],
				code, errno);
			break;
		}
#endif
		default:
			/* XXX formerly select/fd_set case, now placeholder
			 * for new event systems, reworked select, etc. */
			break;
		}		/* switch */
	}

	mutex_unlock(&sr_rec->mtx);

 out:
	return (0);
}

static int
svc_rqst_hook_events(SVCXPRT *xprt /* LOCKED */ ,
		     struct svc_rqst_rec *sr_rec /* LOCKED */)
{
	int code;

	cond_init_svc_rqst();

	atomic_clear_uint16_t_bits(&xprt->xp_flags, SVC_XPRT_FLAG_BLOCKED);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev = &xprt->ev_u.epoll.event;

		/* set up epoll user data */
		ev->data.ptr = xprt;

		/* wait for read events, level triggered, oneshot */
		ev->events = EPOLLIN | EPOLLONESHOT;

		/* add to epoll vector */
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_ADD, xprt->xp_fd, ev);
		if (code) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p epoll add failed fd %d "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d, %d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1],
				code, errno);
			code = errno;
		} else {
			atomic_set_uint16_t_bits(&xprt->xp_flags,
						 SVC_XPRT_FLAG_ADDED);

			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p epoll add fd %d "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d, %d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1],
				code, errno);
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	ev_sig(sr_rec->sv[0], 0);	/* send wakeup */

	return (0);
}

/* register newxprt on an event channel, based on various
 * parameters */
int
svc_rqst_xprt_register(SVCXPRT *xprt, SVCXPRT *newxprt)
{
	struct svc_rqst_rec *sr_rec;
	int code = 0;

	cond_init_svc_rqst();

	/* do nothing if event registration is globally disabled */
	if (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
		goto out;

	/* use global registration if no parent xprt */
	if (!xprt) {
		xprt_register(newxprt);
		goto out;
	}

	sr_rec = (struct svc_rqst_rec *)xprt->xp_ev;

	/* or if parent xprt has no dedicated event channel */
	if (!sr_rec) {
		xprt_register(newxprt);
		goto out;
	}

	/* follow policy if applied.  the client code will still normally
	 * be called back to, e.g., adjust channel assignment */
	if (sr_rec->flags & SVC_RQST_FLAG_CHAN_AFFINITY)
		svc_rqst_evchan_reg(sr_rec->id_k, newxprt, SVC_RQST_FLAG_NONE);
	else
		xprt_register(newxprt);

 out:
	return (code);
}

void
xprt_unregister(SVCXPRT *xprt)
{
	struct svc_rqst_rec *sr_rec = (struct svc_rqst_rec *)xprt->xp_ev;

	/* if xprt is is on a dedicated channel? */
	if (sr_rec && sr_rec->id_k) {
		__warnx(TIRPC_DEBUG_FLAG_REFCNT,
			"%s:%u %p xp_refs %" PRIu32,
			__func__, __LINE__, xprt, xprt->xp_refs);
		evchan_unreg_impl(sr_rec, xprt, SVC_RQST_FLAG_NONE);
	} else {
		__warnx(TIRPC_DEBUG_FLAG_REFCNT,
			"%s:%u %p xp_refs %" PRIu32,
			__func__, __LINE__, xprt, xprt->xp_refs);
		(void)svc_rqst_evchan_unreg(__svc_params->ev_u.evchan.id, xprt,
					    SVC_RQST_FLAG_PART_UNLOCK);
	}

	/* remove xprt from xprt table */
	svc_xprt_clear(xprt, SVC_XPRT_FLAG_LOCKED);

	/* free state */
	xprt->xp_ev = NULL;

	/* xprt must be unlocked before sr_rec */
	mutex_unlock(&xprt->xp_lock);

	if (sr_rec)
		mutex_unlock(&sr_rec->mtx);
}

bool_t __svc_clean_idle2(int timeout, bool_t cleanblock);

#ifdef TIRPC_EPOLL

static inline void
svc_rqst_handle_event(struct svc_rqst_rec *sr_rec, struct epoll_event *ev,
		      uint32_t wakeups)
{
	SVCXPRT *xprt;
	int code __attribute__ ((unused));

	if (ev->data.fd != sr_rec->sv[1]) {
		xprt = (SVCXPRT *) ev->data.ptr;

		if (!(atomic_fetch_uint16_t(&xprt->xp_flags)
			& (SVC_XPRT_FLAG_BLOCKED | SVC_XPRT_FLAG_DESTROYED))
		 && (xprt->xp_refs > 0)) {
			/* check for valid xprt. No need for lock;
			 * (idempotent) xp_flags and xp_refs are set atomic.
			 */
			__warnx(TIRPC_DEBUG_FLAG_REFCNT |
				TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p xp_refs %" PRIu32
				" fd %d or ptr %p EPOLL event %d",
				__func__, xprt, xprt->xp_refs,
				ev->data.fd, ev->data.ptr, ev->events);

			/* take extra ref, callout will release */
			SVC_REF(xprt, SVC_REF_FLAG_NONE);

			/* ! LOCKED */
			code = xprt->xp_ops->xp_getreq(xprt);
			__warnx(TIRPC_DEBUG_FLAG_REFCNT,
				"%s: %p xp_refs %" PRIu32
				" post xp_getreq",
				__func__, xprt,
				xprt->xp_refs);
		}
		/* XXX failsafe idle processing */
		if ((wakeups % 1000) == 0)
			__svc_clean_idle2(__svc_params->idle_timeout, true);
	} else {
		/* signalled -- there was a wakeup on ctrl_ev (see
		 * top-of-loop) */
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: wakeup fd %d (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
		(void)consume_ev_sig_nb(sr_rec->sv[1]);
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: after consume sig fd %d (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
	}
}

static inline int
svc_rqst_thrd_run_epoll(struct svc_rqst_rec *sr_rec, uint32_t
			__attribute__ ((unused)) flags)
{
	struct epoll_event *ev;
	int ix, code = 0;
	int timeout_ms = 120 * 1000;	/* XXX */
	int n_events;
	static uint32_t wakeups;

	for (;;) {

		mutex_lock(&sr_rec->mtx);

		++(wakeups);

		/* check for signals */
		if (sr_rec->signals & SVC_RQST_SIGNAL_SHUTDOWN) {
			mutex_unlock(&sr_rec->mtx);
			break;
		}

		mutex_unlock(&sr_rec->mtx);

		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: before epoll_wait fd %d", __func__,
			sr_rec->ev_u.epoll.epoll_fd);

		switch (n_events =
			epoll_wait(sr_rec->ev_u.epoll.epoll_fd,
				   sr_rec->ev_u.epoll.events,
				   sr_rec->ev_u.epoll.max_events, timeout_ms)) {
		case -1:
			if (errno == EINTR)
				continue;
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: epoll_wait failed %d", __func__, errno);
			break;
		case 0:
			/* timed out (idle) */
			__svc_clean_idle2(__svc_params->idle_timeout, true);
			continue;
		default:
			/* new events */
			for (ix = 0; ix < n_events; ++ix) {
				ev = &(sr_rec->ev_u.epoll.events[ix]);
				svc_rqst_handle_event(sr_rec, ev, wakeups);
			}
		}
	}

	return (code);
}
#endif

int
svc_rqst_thrd_run(uint32_t chan_id, __attribute__ ((unused)) uint32_t flags)
{
	struct svc_rqst_rec *sr_rec = NULL;
	struct rbtree_x_part *t;
	int code = 0;

	sr_rec = svc_rqst_lookup_chan(chan_id, &t, (SVC_RQST_FLAG_PART_UNLOCK));
	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"svc_rqst_thrd_run: unknown chan_id %d", chan_id);
		code = ENOENT;
		goto out;
	}

	/* serialization model for srec is mutual exclusion on mutation only,
	 * with a secondary state machine to detect inconsistencies (e.g.,
	 * trying to unregister a channel when it is active) */
	sr_rec->states |= SVC_RQST_STATE_ACTIVE;
	mutex_unlock(&sr_rec->mtx);

	/* enter event loop */
	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
		code = svc_rqst_thrd_run_epoll(sr_rec, flags);
		break;
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"svc_rqst_thrd_run: unsupported event type");
		break;
	}			/* switch */
	if (sr_rec)
		sr_rec_release(sr_rec, SVC_RQST_FLAG_NONE);

 out:
	return (code);
}

int
svc_rqst_thrd_signal(uint32_t chan_id, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;
	struct rbtree_x_part *t;
	int code = 0;

	sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_PART_UNLOCK);
	if (!sr_rec) {
		code = ENOENT;
		goto out;
	}

	sr_rec->signals |= (flags & SVC_RQST_SIGNAL_MASK);
	ev_sig(sr_rec->sv[0], flags);	/* send wakeup */
	mutex_unlock(&sr_rec->mtx);

 out:
	if (sr_rec)
		sr_rec_release(sr_rec, SVC_RQST_FLAG_NONE);
	return (code);
}

/*
 * Activate a transport handle.
 */
void
xprt_register(SVCXPRT *xprt)
{
	int code __attribute__ ((unused)) = 0;

	/* Create a legacy/global event channel */
	if (!(__svc_params->ev_u.evchan.id)) {
		code =
		    svc_rqst_new_evchan(&(__svc_params->ev_u.evchan.id),
					NULL /* u_data */ ,
					SVC_RQST_FLAG_CHAN_AFFINITY);
	}

	/* and bind xprt to it */
	code =
	    svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id, xprt,
				SVC_RQST_FLAG_CHAN_AFFINITY);
}				/* xprt_register */

void
svc_rqst_shutdown(void)
{
	if (__svc_params->ev_u.evchan.id) {
		svc_rqst_delete_evchan(__svc_params->ev_u.evchan.id);
		__svc_params->ev_u.evchan.id = 0;
	}
}
