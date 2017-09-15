/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2013-2015 CohortFS, LLC.
 * Copyright (c) 2017 Red Hat, Inc.
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
#include "svc_xprt.h"

/**
 * @file svc_rqst.c
 * @contributeur William Allen Simpson <bill@cohortfs.com>
 * @brief Multi-channel event signal package
 *
 * @section DESCRIPTION
 *
 * Maintains a list of all extant transports by event (channel) id.
 *
 * Each SVCXPRT points to its own handler, however, so operations to
 * block/unblock events (for example) given an existing xprt handle
 * are O(1) without any ordered or hashed representation.
 */

#define SVC_RQST_TIMEOUT_MS (29 /* seconds (prime) was 120 */ * 1000)
#define SVC_RQST_WAKEUPS (1023)

static uint32_t round_robin;
/*static*/ uint32_t wakeups;

struct svc_rqst_rec {
	struct work_pool_entry ev_wpe;
	pthread_mutex_t mtx;

	int sv[2];
	uint32_t id_k;		/* chan id */
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

struct svc_rqst_set {
	mutex_t mtx;
	struct svc_rqst_rec *srr;
	uint32_t max_id;
	uint32_t next_id;
};

static struct svc_rqst_set svc_rqst_set = {
	MUTEX_INITIALIZER,
	NULL,
	0,
	0,
};

static inline void
SetNonBlock(int fd)
{
	int s_flags = fcntl(fd, F_GETFL, 0);
	(void)fcntl(fd, F_SETFL, (s_flags | O_NONBLOCK));
}

void
svc_rqst_init(uint32_t channels)
{
	mutex_lock(&svc_rqst_set.mtx);

	if (svc_rqst_set.srr)
		goto unlock;

	svc_rqst_set.max_id = channels;
	svc_rqst_set.next_id = channels;
	svc_rqst_set.srr = mem_zalloc(channels * sizeof(struct svc_rqst_rec));

 unlock:
	mutex_unlock(&svc_rqst_set.mtx);
}

/**
 * @brief Lookup a channel
 * @note Locking
 * - Returns with sr_rec locked.
 */
static inline struct svc_rqst_rec *
svc_rqst_lookup_chan(uint32_t chan_id)
{
	struct svc_rqst_rec *sr_rec;

	if (chan_id >= svc_rqst_set.max_id)
		return (NULL);

	sr_rec = &svc_rqst_set.srr[chan_id];
	if (!sr_rec->refcnt)
		return (NULL);

	mutex_lock(&sr_rec->mtx);
	atomic_inc_uint32_t(&sr_rec->refcnt);

	return (sr_rec);
}

/* forward declaration in lieu of moving code {WAS} */
static void svc_rqst_run_task(struct work_pool_entry *);

int
svc_rqst_new_evchan(uint32_t *chan_id /* OUT */, void *u_data, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;
	uint32_t n_id;
	int code = 0;

	mutex_lock(&svc_rqst_set.mtx);
	if (!svc_rqst_set.next_id) {
		/* too many new channels, re-use global default, may be zero */
		*chan_id =
		svc_rqst_set.next_id = __svc_params->ev_u.evchan.id;
		mutex_unlock(&svc_rqst_set.mtx);
		return (0);
	}
	n_id = --(svc_rqst_set.next_id);
	sr_rec = &svc_rqst_set.srr[n_id];

	if (sr_rec->refcnt) {
		/* already exists */
		*chan_id = n_id;
		mutex_unlock(&svc_rqst_set.mtx);
		return (0);
	}

	flags |= SVC_RQST_FLAG_EPOLL;	/* XXX */

	/* create a pair of anonymous sockets for async event channel wakeups */
	code = socketpair(AF_UNIX, SOCK_STREAM, 0, sr_rec->sv);
	if (code) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: failed creating event signal socketpair (%d)",
			__func__, code);
		++(svc_rqst_set.next_id);
		mutex_unlock(&svc_rqst_set.mtx);
		return (code);
	}

	/* set non-blocking */
	SetNonBlock(sr_rec->sv[0]);
	SetNonBlock(sr_rec->sv[1]);

#if defined(TIRPC_EPOLL)
	if (flags & SVC_RQST_FLAG_EPOLL) {
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
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: epoll_create failed (%d)", __func__,
				errno);
			mem_free(sr_rec->ev_u.epoll.events,
				 sr_rec->ev_u.epoll.max_events *
				 sizeof(struct epoll_event));
			++(svc_rqst_set.next_id);
			mutex_unlock(&svc_rqst_set.mtx);
			return (EINVAL);
		}

		/* permit wakeup of threads blocked in epoll_wait, with a
		 * couple of possible semantics */
		sr_rec->ev_u.epoll.ctrl_ev.events =
		    EPOLLIN | EPOLLRDHUP;
		sr_rec->ev_u.epoll.ctrl_ev.data.fd = sr_rec->sv[1];
		code =
		    epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_ADD,
			      sr_rec->sv[1], &sr_rec->ev_u.epoll.ctrl_ev);
		if (code == -1) {
			code = errno;
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: add control socket failed (%d)", __func__,
				code);
		}
	} else {
		/* legacy fdset (currently unhooked) */
		sr_rec->ev_type = SVC_EVENT_FDSET;
	}
#else
	sr_rec->ev_type = SVC_EVENT_FDSET;
#endif

	*chan_id =
	sr_rec->id_k = n_id;
	sr_rec->refcnt = 1;	/* svc_rqst_set ref */
	sr_rec->flags = flags & SVC_RQST_FLAG_MASK;
	mutex_init(&sr_rec->mtx, NULL);

	if (!code) {
		sr_rec->refcnt = 2;
		sr_rec->ev_wpe.fun = svc_rqst_run_task;
		sr_rec->ev_wpe.arg = u_data;
		work_pool_submit(&svc_work_pool, &sr_rec->ev_wpe);
	}
	mutex_unlock(&svc_rqst_set.mtx);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: create evchan %d control fd pair (%d:%d)",
		__func__, n_id,
		sr_rec->sv[0], sr_rec->sv[1]);
	return (code);
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

static inline void
svc_rqst_release(struct svc_rqst_rec *sr_rec /* LOCKED => UNLOCKED */)
{
	mutex_unlock(&sr_rec->mtx);

	if (atomic_dec_uint32_t(&sr_rec->refcnt))
		return;

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: remove evchan %d control fd pair (%d:%d)",
		__func__, sr_rec->id_k,
		sr_rec->sv[0], sr_rec->sv[1]);
	mutex_destroy(&sr_rec->mtx);
}

/*
 * SVC_RQST_FLAG_LOCKED
 */
static inline int
svc_rqst_unhook_events(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec)
{
	int code = EINVAL;

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev = &rec->ev_u.epoll.event;

		/* clear epoll vector */
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_DEL, rec->xprt.xp_fd, ev);
		if (code) {
			code = errno;
			__warnx(TIRPC_DEBUG_FLAG_WARN,
				"%s: %p fd %d epoll del failed "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d)",
				__func__, rec, rec->xprt.xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1], code);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p fd %d epoll del "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d)",
				__func__, rec, rec->xprt.xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1]);
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	return (code);
}

int
svc_rqst_rearm_events(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec = (struct svc_rqst_rec *)rec->ev_p;
	int code = EINVAL;

	if (xprt->xp_flags & (SVC_XPRT_FLAG_ADDED | SVC_XPRT_FLAG_DESTROYED))
		return (0);

	/* MUST follow the destroyed check above */
	if (sr_rec->signals & SVC_RQST_SIGNAL_SHUTDOWN)
		return (0);

	rpc_dplx_rli(rec);

	/* assuming success */
	atomic_set_uint16_t_bits(&xprt->xp_flags, SVC_XPRT_FLAG_ADDED);

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev = &rec->ev_u.epoll.event;

		/* set up epoll user data */
		ev->events = EPOLLIN | EPOLLONESHOT;

		/* rearm in epoll vector */
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_MOD, xprt->xp_fd, ev);
		if (code) {
			code = errno;
			atomic_clear_uint16_t_bits(&xprt->xp_flags,
						   SVC_XPRT_FLAG_ADDED);
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d epoll arm failed "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1], code);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p fd %d epoll arm "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d)",
				__func__, xprt, xprt->xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1]);
		}
		break;
	}
#endif
	default:
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		break;
	}			/* switch */

	rpc_dplx_rui(rec);

	return (code);
}

/*
 * SVC_RQST_FLAG_LOCKED
 */
static inline int
svc_rqst_hook_events(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec)
{
	int code = EINVAL;

	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
	{
		struct epoll_event *ev = &rec->ev_u.epoll.event;

		/* set up epoll user data */
		ev->data.ptr = rec;

		/* wait for read events, level triggered, oneshot */
		ev->events = EPOLLIN | EPOLLONESHOT;

		/* add to epoll vector */
		code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
				 EPOLL_CTL_ADD, rec->xprt.xp_fd, ev);
		if (code) {
			code = errno;
			atomic_clear_uint16_t_bits(&rec->xprt.xp_flags,
						   SVC_XPRT_FLAG_ADDED);
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d epoll add failed "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d) (%d)",
				__func__, rec, rec->xprt.xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1], code);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p fd %d epoll add "
				"sr_rec %p epoll_fd %d "
				"control fd pair (%d:%d)",
				__func__, rec, rec->xprt.xp_fd,
				sr_rec, sr_rec->ev_u.epoll.epoll_fd,
				sr_rec->sv[0], sr_rec->sv[1]);
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

	return (code);
}

/*
 * indirect on ev_p and ev_q protected by SVC_RQST_FLAG_LOCKED
 */
static void
svc_rqst_unreg(struct rpc_dplx_rec *rec, struct svc_rqst_rec *sr_rec)
{
	uint16_t xp_flags = atomic_postclear_uint16_t_bits(&rec->xprt.xp_flags,
							   SVC_XPRT_FLAG_ADDED);

	/* clear events */
	if (xp_flags & SVC_XPRT_FLAG_ADDED)
		(void)svc_rqst_unhook_events(rec, sr_rec);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT | TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: %p fd %d xp_refs %" PRIu32
		" chan_id %d refcnt %" PRIu32,
		__func__, &rec->xprt, rec->xprt.xp_fd, rec->xprt.xp_refs,
		sr_rec->id_k, sr_rec->refcnt);

	/* Unlinking after debug message ensures both the xprt and the sr_rec
	 * are still present, as the xprt unregisters before release.
	 */
	rec->ev_p = NULL;

	/* DROP one ref per xprt, but need no release here;
	 * by definition, there is always another partition ref.
	 */
	atomic_dec_uint32_t(&sr_rec->refcnt);
}

int
svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *sr_rec;
	struct svc_rqst_rec *ev_p;
	int code;
	uint16_t bits = SVC_XPRT_FLAG_ADDED | (flags & SVC_XPRT_FLAG_UREG);

	if (chan_id == 0) {
		/* Create a global/legacy event channel */
		code = svc_rqst_new_evchan(&(__svc_params->ev_u.evchan.id),
					   NULL /* u_data */ ,
					   SVC_RQST_FLAG_CHAN_AFFINITY);
		if (code) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p failed to create global/legacy channel (%d)",
				__func__, xprt, code);
			return (code);
		}
		chan_id = __svc_params->ev_u.evchan.id;
	}

	sr_rec = svc_rqst_lookup_chan(chan_id);
	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p unknown chan_id %d",
			__func__, xprt, chan_id);
		return (ENOENT);
	}

	while ((ev_p = (struct svc_rqst_rec *)rec->ev_p) != NULL) {
		if (ev_p == sr_rec) {
			mutex_unlock(&sr_rec->mtx);
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: %p already registered chan_id %d",
				__func__, xprt, chan_id);
			return (0);
		}
		mutex_lock(&ev_p->mtx);
		if (ev_p == (struct svc_rqst_rec *)rec->ev_p)
			svc_rqst_unreg(rec, ev_p);
		mutex_unlock(&ev_p->mtx);
	}

	/* assuming success */
	atomic_set_uint16_t_bits(&xprt->xp_flags, bits);

	/* link from xprt */
	rec->ev_p = sr_rec;

	/* register on event channel */
	code = svc_rqst_hook_events(rec, sr_rec);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT | TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: %p fd %d xp_refs %" PRIu32
		" chan_id %d refcnt %" PRIu32,
		__func__, xprt, xprt->xp_fd, xprt->xp_refs,
		sr_rec->id_k, sr_rec->refcnt);

	/* Unlocking after debug message ensures both the xprt and the sr_rec
	 * are still present, as the xprt unregisters before release.
	 */
	mutex_unlock(&sr_rec->mtx);

	return (code);
}

/* register newxprt on an event channel, based on various
 * parameters */
int
svc_rqst_xprt_register(SVCXPRT *newxprt, SVCXPRT *xprt)
{
	struct svc_rqst_rec *sr_rec;

	/* if no parent xprt, use global/legacy event channel */
	if (!xprt)
		return svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id,
					   newxprt,
					   SVC_RQST_FLAG_CHAN_AFFINITY);

	sr_rec = (struct svc_rqst_rec *) REC_XPRT(xprt)->ev_p;

	/* or if parent xprt has no dedicated event channel */
	if (!sr_rec)
		return svc_rqst_evchan_reg(__svc_params->ev_u.evchan.id,
					   newxprt,
					   SVC_RQST_FLAG_CHAN_AFFINITY);

	/* if round robin policy, begin with global/legacy event channel */
	if (!(sr_rec->flags & SVC_RQST_FLAG_CHAN_AFFINITY)) {
		int code = svc_rqst_evchan_reg(round_robin, newxprt,
					       SVC_RQST_FLAG_NONE);

		if (!code) {
			/* advance round robin channel */
			code = svc_rqst_new_evchan(&round_robin, NULL,
						   SVC_RQST_FLAG_NONE);
		}
		return (code);
	}

	return svc_rqst_evchan_reg(sr_rec->id_k, newxprt, SVC_RQST_FLAG_NONE);
}

void
svc_rqst_xprt_unregister(SVCXPRT *xprt)
{
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct svc_rqst_rec *ev_p;

	while ((ev_p = (struct svc_rqst_rec *)rec->ev_p) != NULL) {
		mutex_lock(&ev_p->mtx);
		if (ev_p == (struct svc_rqst_rec *)rec->ev_p)
			svc_rqst_unreg(rec, ev_p);
		mutex_unlock(&ev_p->mtx);
	}

	/* There is a small window between removing the registration
	 * (system call latency) and processing outstanding events.
	 * Therefore, remove from the transport tree here (and only here).
	 */
	svc_xprt_clear(xprt);
}

/*static*/ void
svc_rqst_xprt_task(struct work_pool_entry *wpe)
{
	struct rpc_dplx_rec *rec =
			opr_containerof(wpe, struct rpc_dplx_rec, ioq.ioq_wpe);

	if (rec->xprt.xp_refs > 1
	 && !(rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
		/* (idempotent) xp_flags and xp_refs are set atomic.
		 * xp_refs need more than 1 (this task).
		 */
		atomic_clear_uint16_t_bits(&rec->ioq.ioq_s.qflags,
					   IOQ_FLAG_WORKING);
		(void)clock_gettime(CLOCK_MONOTONIC_FAST, &(rec->recv.ts));
		(void)SVC_RECV(&rec->xprt);
	}

	/* If tests fail, log non-fatal "WARNING! already destroying!" */
	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
}

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */

struct svc_rqst_clean_arg {
	struct timespec ts;
	int timeout;
	int cleaned;
};

static bool
svc_rqst_clean_func(SVCXPRT *xprt, void *arg)
{
	struct svc_rqst_clean_arg *acc = (struct svc_rqst_clean_arg *)arg;

	if (xprt->xp_ops == NULL)
		return (false);

	if (xprt->xp_flags & (SVC_XPRT_FLAG_DESTROYED | SVC_XPRT_FLAG_UREG))
		return (false);

	if ((acc->ts.tv_sec - REC_XPRT(xprt)->recv.ts.tv_sec) < acc->timeout)
		return (false);

	SVC_DESTROY(xprt);
	acc->cleaned++;
	return (true);
}

void authgss_ctx_gc_idle(void);

static void
svc_rqst_clean_idle(int timeout)
{
	struct svc_rqst_clean_arg acc;
	static mutex_t active_mtx = MUTEX_INITIALIZER;
	static uint32_t active;

	if (mutex_trylock(&active_mtx) != 0)
		return;

	if (active > 0)
		goto unlock;

	++active;

#ifdef _HAVE_GSSAPI
	/* trim gss context cache */
	authgss_ctx_gc_idle();
#endif /* _HAVE_GSSAPI */

	if (timeout <= 0)
		goto unlock;

	/* trim xprts (not sorted, not aggressive [but self limiting]) */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &acc.ts);
	acc.timeout = timeout;
	acc.cleaned = 0;

	svc_xprt_foreach(svc_rqst_clean_func, (void *)&acc);

 unlock:
	--active;
	mutex_unlock(&active_mtx);
	return;
}

#ifdef TIRPC_EPOLL

static struct rpc_dplx_rec *
svc_rqst_epoll_event(struct svc_rqst_rec *sr_rec, struct epoll_event *ev)
{
	struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) ev->data.ptr;
	uint16_t xp_flags;

	if (unlikely(ev->data.fd == sr_rec->sv[1])) {
		/* signalled -- there was a wakeup on ctrl_ev (see
		 * top-of-loop) */
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: fd %d wakeup (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
		(void)consume_ev_sig_nb(sr_rec->sv[1]);
		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: fd %d after consume sig (sr_rec %p)",
			__func__, sr_rec->sv[1],
			sr_rec);
		return (NULL);
	}

	/* Another task may release transport in parallel.
	 * Take extra reference now to keep window as small as possible.
	 * Under normal circumstances, worker task (above) will release.
	 */
	SVC_REF(&rec->xprt, SVC_REF_FLAG_NONE);

	/* MUST handle flags after reference.
	 * Although another task may unhook, the error is non-fatal.
	 */
	xp_flags = atomic_postclear_uint16_t_bits(&rec->xprt.xp_flags,
						  SVC_XPRT_FLAG_ADDED);

	__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
		"%s: %p fd %d event %d",
		__func__, rec, rec->xprt.xp_fd, ev->events);

	if (rec->xprt.xp_refs > 1
	 && (xp_flags & SVC_XPRT_FLAG_ADDED)
	 && !(xp_flags & SVC_XPRT_FLAG_DESTROYED)
	 && !(atomic_postset_uint16_t_bits(&rec->ioq.ioq_s.qflags,
					   IOQ_FLAG_WORKING)
	      & IOQ_FLAG_WORKING)) {
		/* (idempotent) xp_flags and xp_refs are set atomic.
		 * xp_refs need more than 1 (this event).
		 */
		return (rec);
	}

	/* Do not return destroyed transports.
	 * Probably log non-fatal "WARNING! already destroying!"
	 */
	SVC_RELEASE(&rec->xprt, SVC_RELEASE_FLAG_NONE);
	return (NULL);
}

/*
 * - sr_rec LOCKED
 *  (sr_rec unlocked during event processing).
 * - Returns with sr_rec locked.
 */
static inline bool
svc_rqst_epoll_events(struct svc_rqst_rec *sr_rec, int n_events)
{
	struct rpc_dplx_rec *rec = NULL;
	int ix = 0;

	while (ix < n_events) {
		rec = svc_rqst_epoll_event(sr_rec,
					   &(sr_rec->ev_u.epoll.events[ix++]));
		if (rec)
			break;
	}

	if (!rec) {
		/* continue waiting for events with this task */
		return false;
	}

	while (ix < n_events) {
		struct rpc_dplx_rec *rec = svc_rqst_epoll_event(sr_rec,
					    &(sr_rec->ev_u.epoll.events[ix++]));
		if (!rec)
			continue;

		rec->ioq.ioq_wpe.fun = svc_rqst_xprt_task;
		work_pool_submit(&svc_work_pool, &(rec->ioq.ioq_wpe));
	}

	/* submit another task to handle events in order */
	atomic_inc_uint32_t(&sr_rec->refcnt);
	work_pool_submit(&svc_work_pool, &sr_rec->ev_wpe);

	/* in most cases have only one event, use this hot thread */
	mutex_unlock(&sr_rec->mtx);

	rec->ioq.ioq_wpe.fun = svc_rqst_xprt_task;
	svc_rqst_xprt_task(&(rec->ioq.ioq_wpe));

	/* failsafe idle processing after work task */
	if (atomic_postclear_uint32_t_bits(&wakeups, ~SVC_RQST_WAKEUPS)
	    > SVC_RQST_WAKEUPS) {
		svc_rqst_clean_idle(__svc_params->idle_timeout);
	}

	mutex_lock(&sr_rec->mtx);
	return true;
}

/*
 * - sr_rec LOCKED
 *  (sr_rec unlocked during loop).
 * - Returns with sr_rec locked.
 */
static inline bool
svc_rqst_epoll_loop(struct svc_rqst_rec *sr_rec)
{
	int n_events;

	for (;;) {
		mutex_unlock(&sr_rec->mtx);

		__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
			"%s: epoll_fd %d before epoll_wait",
			__func__,
			sr_rec->ev_u.epoll.epoll_fd);

		n_events = epoll_wait(sr_rec->ev_u.epoll.epoll_fd,
				      sr_rec->ev_u.epoll.events,
				      sr_rec->ev_u.epoll.max_events,
				      SVC_RQST_TIMEOUT_MS);

		mutex_lock(&sr_rec->mtx);

		if (unlikely(sr_rec->signals & SVC_RQST_SIGNAL_SHUTDOWN)) {
			__warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
				"%s: epoll_fd %d epoll_wait shutdown (%d)",
				__func__,
				sr_rec->ev_u.epoll.epoll_fd,
				n_events);
			return true;
		}
		if (n_events > 0) {
			atomic_add_uint32_t(&wakeups, n_events);

			if (svc_rqst_epoll_events(sr_rec, n_events))
				return false;
			continue;
		}
		if (!n_events) {
			/* timed out (idle) */
			atomic_inc_uint32_t(&wakeups);
			continue;
		}
		n_events = errno;
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s: epoll_fd %d epoll_wait failed (%d)",
			__func__,
			sr_rec->ev_u.epoll.epoll_fd,
			n_events);

		if (n_events != EINTR)
			return true;
	}
}
#endif

static void
svc_rqst_run_task(struct work_pool_entry *wpe)
{
	struct svc_rqst_rec *sr_rec =
		opr_containerof(wpe, struct svc_rqst_rec, ev_wpe);
	bool finished;

	mutex_lock(&sr_rec->mtx);

	/* enter event loop */
	switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
	case SVC_EVENT_EPOLL:
		finished = svc_rqst_epoll_loop(sr_rec);
		if (finished) {
			close(sr_rec->ev_u.epoll.epoll_fd);
			mem_free(sr_rec->ev_u.epoll.events,
				 sr_rec->ev_u.epoll.max_events *
				 sizeof(struct epoll_event));
		}
		break;
#endif
	default:
		finished = true;
		/* XXX formerly select/fd_set case, now placeholder for new
		 * event systems, reworked select, etc. */
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: unsupported event type",
			__func__);
		break;
	}			/* switch */

	if (finished) {
		/* reference count here should be 2:
		 *	1	svc_rqst_set
		 *	+1	this work_pool thread
		 * so, DROP one here so the final release will go to 0.
		 */
		atomic_dec_uint32_t(&sr_rec->refcnt);	/* svc_rqst_set */
	}
	svc_rqst_release(sr_rec);
}

int
svc_rqst_thrd_signal(uint32_t chan_id, uint32_t flags)
{
	struct svc_rqst_rec *sr_rec;

	sr_rec = svc_rqst_lookup_chan(chan_id);
	if (!sr_rec) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: unknown chan_id %d",
			__func__, chan_id);
		return (ENOENT);
	}

	ev_sig(sr_rec->sv[0], flags);	/* send wakeup */

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s: signalled chan_id %d",
		__func__, chan_id);
	svc_rqst_release(sr_rec);
	return (0);
}

static int
svc_rqst_delete_evchan(uint32_t chan_id)
{
	struct svc_rqst_rec *sr_rec;
	int code = 0;

	sr_rec = svc_rqst_lookup_chan(chan_id);
	if (!sr_rec) {
		return (ENOENT);
	}
	atomic_set_uint32_t_bits(&sr_rec->signals, SVC_RQST_SIGNAL_SHUTDOWN);
	ev_sig(sr_rec->sv[0], SVC_RQST_SIGNAL_SHUTDOWN);

	svc_rqst_release(sr_rec);
	return (code);
}

void
svc_rqst_shutdown(void)
{
	uint32_t channels = svc_rqst_set.max_id;

	while (channels > 0) {
		svc_rqst_delete_evchan(--channels);
	}
}
