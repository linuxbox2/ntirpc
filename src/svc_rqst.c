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
#include <misc/portable.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#if defined(TIRPC_EPOLL)
#include <misc/epoll.h>
#endif
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <rpc/types.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */
#include <rpc/svc_rqst.h>

#include "rpc_com.h"

#include <rpc/svc.h>
#include <misc/rbtree.h>
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

static bool_t initialized = FALSE;

static struct svc_rqst_set svc_rqst_set = {
    MUTEX_INITIALIZER, /* mtx */
    {
        0, /* npart */
        RBT_X_FLAG_NONE, /* flags */
        8192, /* cachesz */
        NULL /* tree */
    }, /* xt */
    0 /* next_id */
};

extern struct svc_params __svc_params[1];

#define cond_init_svc_rqst() { \
do { \
    if (! initialized) \
        svc_rqst_init(); \
    } while (0); \
}

static inline void SetNonBlock(int fd)
{
    int s_flags = fcntl(fd, F_GETFL, 0);
    (void) fcntl(fd, F_SETFL, (s_flags|O_NONBLOCK));
}

void svc_rqst_init()
{
    int ix, code = 0;

    mutex_lock(&svc_rqst_set.mtx);

    if (initialized)
        goto unlock;

    mutex_init(&svc_rqst_set.mtx, NULL);
    code = rbtx_init(&svc_rqst_set.xt, rqst_thrd_cmpf,
                     7 /* partitions */,
                     RBT_X_FLAG_ALLOC|RBT_X_FLAG_CACHE_RT);
    if (code)
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: rbtx_init failed", __func__);

    /* init read-through cache */
    for (ix = 0; ix < 7 /* partitions */; ++ix) {
        struct rbtree_x_part *xp = &(svc_rqst_set.xt.tree[ix]);
        xp->cache = mem_zalloc(svc_rqst_set.xt.cachesz *
                               sizeof(struct opr_rbtree_node *));
        if (! xp->cache) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: rbtx cache partition alloc failed", __func__);
            svc_rqst_set.xt.cachesz = 0;
            break;
        }
    }
    initialized = TRUE;

unlock:
    mutex_unlock(&svc_rqst_set.mtx);
}

void svc_rqst_init_xprt(SVCXPRT *xprt)
{
    struct svc_xprt_ev *xp_ev = mem_alloc(sizeof(struct svc_xprt_ev));

    xp_ev->xprt = xprt;
#if defined(TIRPC_EPOLL)
    xp_ev->ev_type = SVC_EVENT_EPOLL;
#else
    xp_ev->ev_type = SVC_EVENT_FDSET;
#endif
    xp_ev->flags = XP_EV_FLAG_NONE;
    xprt->xp_ev = xp_ev;

    /* reachable */
    svc_xprt_set(xprt);
}

void svc_rqst_finalize_xprt(SVCXPRT *xprt)
{
    if (! xprt->xp_ev)
        goto out;

    /* remove xprt from xprt table */
    svc_xprt_clear(xprt);

    /* free state */
    if (xprt->xp_ev)
        mem_free(xprt->xp_ev, sizeof(struct svc_xprt_ev));

out:
    return;
}

static inline struct svc_rqst_rec *
svc_rqst_lookup_chan(uint32_t chan_id, struct rbtree_x_part **ref_t,
                     uint32_t flags)
{
    struct svc_rqst_rec trec, *sr_rec = NULL;
    struct rbtree_x_part *t;
    struct opr_rbtree_node *ns;

    cond_init_svc_rqst();

    trec.id_k = chan_id;
    t = rbtx_partition_of_scalar(&svc_rqst_set.xt, trec.id_k);
    *ref_t = t;

    if (flags & SVC_RQST_FLAG_LOCK)
        mutex_lock(&t->mtx);

    ns = rbtree_x_cached_lookup(&svc_rqst_set.xt, t, &trec.node_k, trec.id_k);
    if (ns) 
        sr_rec = opr_containerof(ns, struct svc_rqst_rec, node_k);

    if (flags & SVC_RQST_FLAG_UNLOCK)
        mutex_unlock(&t->mtx);

    return (sr_rec);
}

#define SVC_RQST_FLAG_MASK (SVC_RQST_FLAG_CHAN_AFFINITY)

int
svc_rqst_new_evchan(uint32_t *chan_id /* OUT */, void *u_data,
                    uint32_t flags)
{
    uint32_t n_id;
    struct svc_rqst_rec *sr_rec;
    struct rbtree_x_part *t;
    bool rslt __attribute__((unused));
    int code = 0;

    cond_init_svc_rqst();

    flags |= SVC_RQST_FLAG_EPOLL; /* XXX */

    sr_rec = mem_alloc(sizeof(struct svc_rqst_rec));
    if (!sr_rec) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: failed allocating svc_rqst_rec", __func__);
        goto out;
    }
    memset(sr_rec, 0, sizeof(struct svc_rqst_rec));

    /* create a pair of anonymous sockets for async event channel wakeups */
    code = socketpair(AF_UNIX, SOCK_STREAM, 0, sr_rec->sv);
    if (code) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: failed creating event signal socketpair",
                __func__);
        goto out;
    }

    /* set non-blocking */
    SetNonBlock(sr_rec->sv[0]);
    SetNonBlock(sr_rec->sv[1]);

#if defined(TIRPC_EPOLL)
    if (flags & SVC_RQST_FLAG_EPOLL) {

        /* XXX improve mask */
        sr_rec->flags = flags & SVC_RQST_FLAG_MASK;

        sr_rec->ev_type = SVC_EVENT_EPOLL;

        /* XXX improve this too */
        sr_rec->ev_u.epoll.max_events = __svc_params->ev_u.evchan.max_events;
        sr_rec->ev_u.epoll.events = (struct epoll_event *)
            mem_alloc(
                sr_rec->ev_u.epoll.max_events*sizeof(struct epoll_event));

        /* create epoll fd */
        sr_rec->ev_u.epoll.epoll_fd =
            epoll_create_wr(sr_rec->ev_u.epoll.max_events, EPOLL_CLOEXEC);

        if (sr_rec->ev_u.epoll.epoll_fd == -1) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: epoll_create failed (%d)",
                    __func__, errno);
            code = EINVAL;
            goto out;
        }

        /* permit wakeup of threads blocked in epoll_wait, with a
         * couple of possible semantics */
        sr_rec->ev_u.epoll.ctrl_ev.events = EPOLLIN|EPOLLONESHOT;
        sr_rec->ev_u.epoll.ctrl_ev.data.fd = sr_rec->sv[1];
        code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
                         EPOLL_CTL_ADD,
                         sr_rec->sv[1],
                         &sr_rec->ev_u.epoll.ctrl_ev);
        if (code == -1)
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: add control socket failed (%d)",
                    __func__, errno);
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
    sr_rec->gen = 0;
    mutex_init(&sr_rec->mtx, NULL);
    opr_rbtree_init(&sr_rec->xprt_q, rqst_xprt_cmpf /* may be NULL */);

    t = rbtx_partition_of_scalar(&svc_rqst_set.xt, sr_rec->id_k);
    mutex_lock(&t->mtx);
    rslt = rbtree_x_cached_insert(&svc_rqst_set.xt, t, &sr_rec->node_k,
                                  sr_rec->id_k);
    mutex_unlock(&t->mtx);

    __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
            "%s: create evchan %d socketpair %d:%d", __func__, n_id,
            sr_rec->sv[0], sr_rec->sv[1]);

    *chan_id = n_id;

out:
    return (code);
}

static inline void
evchan_unreg_impl(struct svc_rqst_rec *sr_rec,
                                     SVCXPRT *xprt, uint32_t flags)
{
    struct svc_xprt_ev *xp_ev;
    struct opr_rbtree_node *nx;

    if (! (flags & SVC_RQST_FLAG_SREC_LOCK))
        mutex_lock(&sr_rec->mtx);

    assert(xprt->xp_ev); /* cf. svc_rqst_init_xprt */
    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;

    nx = opr_rbtree_lookup(&sr_rec->xprt_q, &xp_ev->node_k);
    if (nx)
        opr_rbtree_remove(&sr_rec->xprt_q, &xp_ev->node_k);
    else
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: SVCXPRT %p found but cant be removed",
                __func__, xprt);

    /* clear from event mux */
    (void) svc_rqst_block_events(xprt, SVC_RQST_FLAG_SREC_LOCKED);

    /* XXX lock xprt? */
    xprt->xp_flags &= ~SVC_XPRT_FLAG_EVCHAN;

    /* unlink from xprt */
    xp_ev->sr_rec = NULL;

    if (! (flags & SVC_RQST_FLAG_SREC_LOCK))
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
    int code = 
        write(fd, &sig, sizeof(uint32_t));
    __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
            "%s: fd %d sig %d", __func__, fd, sig);
    if (code < 1)
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: error writing to event socket (%d:%d)",
                __func__, code, errno);
}

/*
 * Read a single 4-byte value from the shared event-notification channel,
 * the socket is in non-blocking mode.  The value read is returned.
 */
static inline uint32_t
consume_ev_sig_nb(int fd)
{
    uint32_t sig = 0;
    (void) read(fd, &sig, sizeof(uint32_t));
    return (sig);
}

int
svc_rqst_delete_evchan(uint32_t chan_id, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct opr_rbtree_node *n;
    struct rbtree_x_part *t;
    SVCXPRT *xprt = NULL;
    int code = 0;

    sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_LOCK);
    if (! sr_rec) {
        mutex_unlock(&t->mtx);
        code = ENOENT;
        goto out;
    }

    /* traverse sr_req->xprt_q inorder */
    mutex_lock(&sr_rec->mtx);

    n = opr_rbtree_first(&sr_rec->xprt_q);
    while (n != NULL) {
        /* indirect on xp_ev */
        xprt = opr_containerof(n, struct svc_xprt_ev, node_k)->xprt;
        /* stop processing events */
        evchan_unreg_impl(sr_rec, xprt,
                          (SVC_RQST_FLAG_LOCK | SVC_RQST_FLAG_SREC_LOCK));

        /* wake up*/
        ev_sig(sr_rec->sv[0], 0);
        switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
        case SVC_EVENT_EPOLL:
            code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd,
                             EPOLL_CTL_DEL,
                             sr_rec->sv[1],
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

        n = opr_rbtree_next(n);
    }

    /* now remove sr_rec */
    rbtree_x_cached_remove(&svc_rqst_set.xt, t, &sr_rec->node_k, sr_rec->id_k);
    mutex_unlock(&t->mtx);

    switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        close(sr_rec->ev_u.epoll.epoll_fd);
        mem_free(sr_rec->ev_u.epoll.events,
                 sr_rec->ev_u.epoll.max_events*sizeof(struct epoll_event));
        break;
#endif
    default:
        /* XXX */
        break;
    }
    sr_rec->states = SVC_RQST_STATE_DESTROYED;
    sr_rec->id_k = 0; /* no chan */
    mutex_unlock(&sr_rec->mtx);
    mutex_destroy(&sr_rec->mtx);
    mem_free(sr_rec, sizeof(struct svc_rqst_rec));

out:
    return (code);
}

int
svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct svc_xprt_ev *xp_ev;
    struct rbtree_x_part *t;
    int code = 0;

    if (chan_id == 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: called with chan_id 0, fatal (bug)", __func__);
        goto out;
    }

    sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_LOCK);
    if (! sr_rec) {
        mutex_unlock(&t->mtx);
        code = ENOENT;
        goto out;
    }

    mutex_lock(&sr_rec->mtx);

    if (flags & SVC_RQST_FLAG_XPRT_UREG)
        xprt_unregister(xprt);

    assert(xprt->xp_ev); /* cf. svc_rqst_init_xprt */
    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    if (opr_rbtree_insert(&sr_rec->xprt_q, &xp_ev->node_k)) {
        /* shouldn't happen */
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: SVCXPRT %p already registered",
                __func__, xprt);
    }

    /* link from xprt */
    xp_ev->sr_rec = sr_rec;

    /* XXXX check that we don't accidentally require EVCHAN to be
     * discretionary EVCHAN */
    /* mark xprt */
    if (xprt->xp_flags & SVC_RQST_FLAG_XPRT_GCHAN)
        xprt->xp_flags |= SVC_XPRT_FLAG_GCHAN;
    else
        xprt->xp_flags |= SVC_XPRT_FLAG_EVCHAN;

    mutex_unlock(&sr_rec->mtx);

    /* register on event mux */
    (void) svc_rqst_unblock_events(xprt, SVC_RQST_FLAG_NONE);

    mutex_unlock(&t->mtx);

out:
    return (code);
}

int
svc_rqst_evchan_unreg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct rbtree_x_part *t;
    int code = EINVAL;

    sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_LOCK);
    if (! sr_rec) {
        code = ENOENT;
        goto unlock;
    }

    evchan_unreg_impl(sr_rec, xprt, SVC_RQST_FLAG_LOCK);
    
unlock:
    mutex_unlock(&t->mtx);
    return (code);
}

int
svc_rqst_block_events(SVCXPRT *xprt, uint32_t flags)
{
    struct svc_xprt_ev *xp_ev;
    struct svc_rqst_rec *sr_rec;
    int code = 0;

    cond_init_svc_rqst();

    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    sr_rec = xp_ev->sr_rec;

    if (! (flags & SVC_RQST_FLAG_SREC_LOCKED))
        mutex_lock(&sr_rec->mtx);

    xp_ev->flags |= XP_EV_FLAG_BLOCKED;

    switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        {
            struct epoll_event *ev = &xp_ev->ev_u.epoll.event;

            /* clear epoll vector */
            code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_DEL,
                             xprt->xp_fd, ev);
            if (code == -1) {
                __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                        "%s: epoll del failed fd %d (%d)",
                        __func__, xprt->xp_fd, errno);
                code = errno;
            } else {
                xp_ev->flags &= ~XP_EV_FLAG_ADDED;
                __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                        "%s: epoll del fd %d epoll_fd %d",
                        __func__, xprt->xp_fd, sr_rec->ev_u.epoll.epoll_fd);
            }
        }
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        break;
    } /* switch */

    if (! (flags & SVC_RQST_FLAG_SREC_LOCKED))
        mutex_unlock(&sr_rec->mtx);

    return (0);
}

int
svc_rqst_rearm_events(SVCXPRT *xprt, uint32_t  __attribute__((unused)) flags)
{
    struct svc_rqst_rec *sr_rec;
    struct svc_xprt_ev *xp_ev;
    int code = 0;

    cond_init_svc_rqst();

    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    sr_rec = xp_ev->sr_rec;

    assert(sr_rec);

    mutex_lock(&sr_rec->mtx);
    if (xp_ev->flags & XP_EV_FLAG_ADDED) {

        switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
        case SVC_EVENT_EPOLL:
        {
            struct epoll_event *ev = &xp_ev->ev_u.epoll.event;

            /* set up epoll user data */
            /* ev->data.ptr = xprt; */ /* XXX already set */
            ev->events = EPOLLIN|EPOLLONESHOT;

            /* rearm in epoll vector */
            code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_MOD,
                             xprt->xp_fd, ev);

            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: rearm xprt %p fd %d sr_rec %p epoll_fd %d control fd "
                    "pair (%d:%d) (%d, %d)",
                    __func__, xprt, xprt->xp_fd, sr_rec,
                    sr_rec->ev_u.epoll.epoll_fd,
                    sr_rec->sv[0], sr_rec->sv[1], code, errno);
        }
        break;
#endif
        default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
            break;
        } /* switch */
    }

    mutex_unlock(&sr_rec->mtx);

    return (0);
}

int svc_rqst_unblock_events(SVCXPRT *xprt,
                            uint32_t __attribute__((unused))flags)
{
    struct svc_rqst_rec *sr_rec;
    struct svc_xprt_ev *xp_ev;
    int code = 0;

    cond_init_svc_rqst();

    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    sr_rec = xp_ev->sr_rec;

    assert(sr_rec);

    mutex_lock(&sr_rec->mtx);

    xp_ev->flags &= ~XP_EV_FLAG_BLOCKED;

    switch (sr_rec->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        {
            struct epoll_event *ev = &xp_ev->ev_u.epoll.event;

            /* set up epoll user data */
            ev->data.ptr = xprt;

            /* wait for read events, level triggered, oneshot */
            ev->events = EPOLLIN|EPOLLONESHOT;

            /* add to epoll vector */
            code = epoll_ctl(sr_rec->ev_u.epoll.epoll_fd, EPOLL_CTL_ADD,
                             xprt->xp_fd, ev);
            if (! code) {
                xp_ev->flags |= XP_EV_FLAG_ADDED;
            }

            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: add xprt %p fd %d sr_rec %p epoll_fd %d control fd "
                    "pair (%d:%d) (%d, %d)",
                    __func__, xprt, xprt->xp_fd, sr_rec,
                    sr_rec->ev_u.epoll.epoll_fd,
                    sr_rec->sv[0], sr_rec->sv[1], code, errno);
        }
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        break;
    } /* switch */

    ev_sig(sr_rec->sv[0], 0); /* send wakeup */

    mutex_unlock(&sr_rec->mtx);

    return (0);
}

/* register newxprt on an event channel, based on various
 * parameters */
int svc_rqst_xprt_register(SVCXPRT *xprt, SVCXPRT *newxprt)
{
    struct svc_rqst_rec *sr_rec;
    struct svc_xprt_ev *xp_ev;
    int code = 0;

    cond_init_svc_rqst();

    /* do nothing if event registration is globally disabled */
    if (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)
        goto out;

    /* use global registration if no parent xprt */
    if (! xprt) {
        xprt_register(newxprt);
        goto out;
    }

    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    sr_rec = xp_ev->sr_rec;

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

int
svc_rqst_xprt_unregister(SVCXPRT *xprt, uint32_t __attribute__((unused)) flags)
{
    struct svc_xprt_ev *xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    struct svc_rqst_rec *sr_rec = xp_ev->sr_rec;
    int code = 0;

    if (! sr_rec) {
        xprt_unregister(xprt);
        goto out;
    }

    /* if xprt is is on a dedicated channel? */
    if (sr_rec->id_k)
        evchan_unreg_impl(sr_rec, xprt, SVC_RQST_FLAG_NONE);
    else
        xprt_unregister(xprt);

out:
    return (code);
}

bool_t __svc_clean_idle2(int timeout, bool_t cleanblock);

static inline int 
svc_rqst_thrd_run_epoll(struct svc_rqst_rec *sr_rec,
                        uint32_t __attribute__((unused)) flags)
{
    struct svc_xprt_ev *xp_ev;
    struct epoll_event *ev;
    SVCXPRT *xprt;

    int ix, code = 0;
    int timeout_ms = 30*1000;
    int n_events;

    for (;;) {

        mutex_lock(&sr_rec->mtx);

        /* check for signals */
        if (sr_rec->signals & SVC_RQST_SIGNAL_SHUTDOWN) {
            mutex_unlock(&sr_rec->mtx);
            break;
        }

        mutex_unlock(&sr_rec->mtx);

        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "%s: before epoll_wait fd %d", __func__,
                sr_rec->ev_u.epoll.epoll_fd);

        switch (n_events = epoll_wait(sr_rec->ev_u.epoll.epoll_fd,
                                      sr_rec->ev_u.epoll.events, 
                                      sr_rec->ev_u.epoll.max_events, 
                                      timeout_ms)) {
        case -1:
            if (errno == EINTR)
                continue;
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: epoll_wait failed %d", __func__, errno);
            break;
        case 0:
            /* timed out (idle) */
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: before __svc_clean_idle2", __func__);
            __svc_clean_idle2(30, FALSE);
            __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                    "%s: after __svc_clean_idle2", __func__);
            continue;
        default:
            /* new events */
            for (ix = 0; ix < n_events; ++ix) {
                ev = &(sr_rec->ev_u.epoll.events[ix]);

                __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                        "%s: event ix %d fd or ptr (%d:%p)", __func__,
                        ix, ev->data.fd, ev->data.ptr);

                if (ev->data.fd != sr_rec->sv[1]) {
                    /* TODO: need to lookup xprt */
                    xprt = (SVCXPRT *) ev->data.ptr;
                    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
                    if (! (xp_ev->flags & XP_EV_FLAG_BLOCKED)) {
                        code = xprt->xp_ops2->xp_getreq(xprt);
                    }
                } else {
                    /* signalled -- there was a wakeup on ctrl_ev (see
                     * top-of-loop) */
                    __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                            "%s: wakeup fd %d (sr_rec %p)",
                            __func__, sr_rec->sv[1], sr_rec);
                    (void) consume_ev_sig_nb(sr_rec->sv[1]);
                    __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                            "%s: after consume sig fd %d (sr_rec %p)",
                            __func__, sr_rec->sv[1], sr_rec);

                }
            } /* each events[ix] */
        } /* switch */
    } /* ;; */

    return (code);
}

int
svc_rqst_thrd_run(uint32_t chan_id, __attribute__((unused)) uint32_t flags)
{
    struct svc_rqst_rec *sr_rec = NULL;
    struct rbtree_x_part *t;
    int code = 0;

    sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_LOCK);
    if (! sr_rec) {
        mutex_unlock(&t->mtx);
        __warnx(TIRPC_DEBUG_FLAG_SVC_RQST,
                "svc_rqst_thrd_run: unknown chan_id %d", chan_id);
        code = ENOENT;
        goto out;
    }

    /* serialization model for srec is mutual exclusion on mutation only,
     * with a secondary state machine to detect inconsistencies (e.g., trying
     * to unregister a channel when it is active) */
    mutex_lock(&sr_rec->mtx);
    mutex_unlock(&t->mtx); /* !LOCK */
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
    } /* switch */

out:
    return (code);
}

int
svc_rqst_thrd_signal(uint32_t chan_id, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct rbtree_x_part *t;
    int code = 0;

    sr_rec = svc_rqst_lookup_chan(chan_id, &t, SVC_RQST_FLAG_LOCK);
    if (! sr_rec) {
        mutex_unlock(&t->mtx);
        code = ENOENT;
        goto out;
    }

    mutex_lock(&sr_rec->mtx);
    sr_rec->signals |= (flags & ~SVC_RQST_SIGNAL_MASK);
    ev_sig(sr_rec->sv[0], flags); /* send wakeup */
    mutex_unlock(&sr_rec->mtx);

out:
    return (code);
}
