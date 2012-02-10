#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h>
#endif
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/types.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"

#include <rpc/svc.h>

#include <misc/rbtree.h>
#include <misc/opr_queue.h>
#include <svc_rqst.h>

/*
 * The TI-RPC instance should be able to reach every registered
 * handler, and potentially each SVCXPRT registered on it.
 *
 * Each SVCXPRT points to its own handler, however, so operations to
 * block/unblock events (for example) given an existing xprt handle
 * are O(1) without any ordered or hashed representation.
 */

static bool_t initialized = FALSE;

static struct svc_rqst_set svc_rqst_set_ = {
    PTHREAD_RWLOCK_INITIALIZER /* lock */,
    { NULL,
      rqst_thrd_cmpf,
      0, /* size */
      0  /* gen */ 
    } /* t */,
    0 /* gen */
};

void svc_rqst_init()
{
    pthread_rwlockattr_t rwlock_attr;

    rwlock_wrlock(&svc_rqst_set_.lock);

    if (initialized)
        goto unlock;

    /* prior versions of Linux tirpc are subject to default prefer-reader
     * behavior (so have potential for writer starvation) */
    rwlockattr_init(&rwlock_attr);
#ifdef GLIBC
    pthread_rwlockattr_setkind_np(
        &rwlock_attr, 
        PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    rwlock_init(&svc_rqst_set_.lock, &rwlock_attr);
    opr_rbtree_init(&svc_rqst_set_.t, rqst_thrd_cmpf /* may be NULL */);

unlock:
    rwlock_unlock(&svc_rqst_set_.lock);
}

void svc_rqst_init_xprt(SVCXPRT *xprt)
{
    struct svc_xprt_ev *xp_ev = mem_alloc(sizeof(struct svc_xprt_ev));
#if defined(TIRPC_EPOLL)
    xp_ev->ev_type = SVC_EVENT_EPOLL;
    
#else
    xp_ev->ev_type = SVC_EVENT_FDSET;
#endif

}

void svc_rqst_finalize_xprt(SVCXPRT *xprt)
{
    if (! xprt->xp_ev)
        goto out;
#if defined(TIRPC_EPOLL)
#endif
    if (xprt->xp_ev)
        mem_free(xprt->xp_ev, sizeof(struct svc_xprt_ev));
out:
    return;
}

static inline struct svc_rqst_rec *svc_rqst_lookup_chan(uint32_t chan_id,
                                                        uint32_t flags)
{
    struct svc_rqst_rec trec, *sr_rec = NULL;
    struct opr_rbtree_node *ns;

    trec.id_k = chan_id;
 
   switch (flags) {
    case SVC_RQST_FLAG_RLOCK:
        rwlock_rdlock(&svc_rqst_set_.lock);
        break;
    case SVC_RQST_FLAG_WLOCK:
        rwlock_wrlock(&svc_rqst_set_.lock);
        break;
    default:
        break;
    }

    ns = opr_rbtree_lookup(&svc_rqst_set_.t, &trec.node_k);
    if (ns) 
        sr_rec = opr_containerof(ns, struct svc_rqst_rec, node_k);

    if (flags & SVC_RQST_FLAG_UNLOCK)
        rwlock_unlock(&svc_rqst_set_.lock);

    return (sr_rec);
}

int svc_rqst_new_evchan(uint32_t *chan_id /* OUT */, void *u_data,
                        uint32_t flags)
{
    uint32_t n_id;
    struct svc_rqst_rec *sr_rec;
    int code = 0;

    sr_rec = mem_alloc(sizeof(struct svc_rqst_rec));
    if (!sr_rec) {
        __warnx("%s: failed allocating svc_rqst_rec", __func__);
        goto out;
    }

    n_id = ++(svc_rqst_set_.next_id);
    sr_rec->id_k = n_id;
    sr_rec->states = SVC_RQST_STATE_NONE;
    sr_rec->u_data = u_data;
    sr_rec->gen = 0;
    mutex_init(&sr_rec->mtx, NULL);
    opr_rbtree_init(&sr_rec->xprt_q, rqst_xprt_cmpf /* may be NULL */);

    rwlock_wrlock(&svc_rqst_set_.lock);
    if (opr_rbtree_insert(&svc_rqst_set_.t, &sr_rec->node_k)) {
        /* cant happen */
        __warnx("%s: inserted a counted value twice (counter fail)", __func__);
        mem_free(sr_rec, sizeof(struct svc_rqst_rec));
        n_id = 0; /* invalid value */
    }    
    rwlock_unlock(&svc_rqst_set_.lock);

    *chan_id = n_id;

out:
    return (code);
}

int svc_rqst_free_evchan(uint32_t chan_id, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct opr_rbtree_node *n;
    SVCXPRT *xprt = NULL;
    int code = 0;

    sr_rec = svc_rqst_lookup_chan(chan_id, SVC_RQST_FLAG_WLOCK);
    if (! sr_rec) {
        code = ENOENT;
        goto unlock;
    }

    /* traverse sr_req->xprt_q inorder */
    mutex_lock(&sr_rec->mtx);
    n = opr_rbtree_first(&sr_rec->xprt_q);
    while (n != NULL) {
        /* indirect on xp_ev */
        xprt = opr_containerof(n, struct svc_xprt_ev, node_k)->xprt;
        /* XXXX TODO: unregister */
        n = opr_rbtree_next(n);
    }
    mutex_unlock(&sr_rec->mtx);

unlock:
    rwlock_unlock(&svc_rqst_set_.lock);

    return (code);
}

int svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct svc_xprt_ev *xp_ev;
    int code = EINVAL;

    sr_rec = svc_rqst_lookup_chan(chan_id, SVC_RQST_FLAG_RLOCK);
    if (! sr_rec) {
        code = ENOENT;
        goto unlock;
    }

    mutex_lock(&sr_rec->mtx);
    assert(xprt->xp_ev); /* cf. svc_rqst_init_xprt */
    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    if (opr_rbtree_insert(&sr_rec->xprt_q, &xp_ev->node_k)) {
        /* shouldn't happen */
        __warnx("%s: SVCXPRT %p already registered",
                __func__, xprt);
    }
    mutex_unlock(&sr_rec->mtx);

unlock:
    rwlock_unlock(&svc_rqst_set_.lock);

    return (code);
}

int svc_rqst_evchan_unreg(uint32_t chan_id, SVCXPRT *xprt, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec;
    struct opr_rbtree_node *nx;
    struct svc_xprt_ev *xp_ev;
    int code = EINVAL;

    sr_rec = svc_rqst_lookup_chan(chan_id, SVC_RQST_FLAG_RLOCK);
    if (! sr_rec) {
        code = ENOENT;
        goto unlock;
    }

    mutex_lock(&sr_rec->mtx);
    assert(xprt->xp_ev); /* cf. svc_rqst_init_xprt */
    xp_ev = (struct svc_xprt_ev *) xprt->xp_ev;
    nx = opr_rbtree_lookup(&sr_rec->xprt_q, &xp_ev->node_k);
    if (nx)
        opr_rbtree_remove(&sr_rec->xprt_q, &xp_ev->node_k);
    else
        __warnx("%s: SVCXPRT %p found but cant be removed",
                __func__, xprt);
    mutex_unlock(&sr_rec->mtx);

unlock:
    rwlock_unlock(&svc_rqst_set_.lock);

    return (code);
}

int svc_rqst_thrd_run(uint32_t chan_id, uint32_t flags)
{
    struct svc_rqst_rec *sr_rec = NULL;
    int code = 0;

    sr_rec = svc_rqst_lookup_chan(chan_id, SVC_RQST_FLAG_RLOCK);
    if (! sr_rec) {
        rwlock_unlock(&svc_rqst_set_.lock);
        code = ENOENT;
        goto out;
    }

    /* serialization model for srec is mutual exclusion on mutation only,
     * with a secondary state machine to detect inconsistencies (e.g., trying
     * to unregister a channel when it is active) */
    mutex_lock(&sr_rec->mtx);
    rwlock_unlock(&svc_rqst_set_.lock); /* !RLOCK */
    sr_rec->states |= SVC_RQST_STATE_ACTIVE;
    mutex_unlock(&sr_rec->mtx);
    
    /* XXXX enter type-specific event loop */

out:
    return (code);
}
