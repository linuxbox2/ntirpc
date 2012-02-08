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
      rqst_xprt_cmpf,
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
    opr_rbtree_init(&svc_rqst_set_.t, rqst_xprt_cmpf /* may be NULL */);

unlock:
    rwlock_unlock(&svc_rqst_set_.lock);
}

int svc_rqst_register_thrd(uint32_t *id /* OUT */, void *u_data,
                            uint32_t flags)
{
    uint32_t n_id;
    struct svc_rqst_rec *sr_rec, *trec;
    int code = 0;

    rwlock_wrlock(&svc_rqst_set_.lock);
    sr_rec = mem_alloc(sizeof(struct svc_rqst_rec));
    if (!sr_rec) {
        __warnx("%s: failed allocating svc_rqst_rec", __func__);
        goto unlock;
    }

    n_id = ++(svc_rqst_set_.next_id);
    sr_rec->id_k = n_id;
    sr_rec->gen = 0;
    sr_rec->u_data = u_data;
    mutex_init(&sr_rec->mtx, NULL);
    opr_queue_Init(&sr_rec->xprt_q);

    if (opr_rbtree_insert(&svc_rqst_set_.t, &sr_rec->node_k)) {
        /* cant happen */
        __warnx("%s: inserted a counted value twice (counter fail)", __func__);
        mem_free(sr_rec, sizeof(struct svc_rqst_rec));
        n_id = 0; /* invalid value */
    }

    *id = n_id;
    
unlock:
    rwlock_unlock(&svc_rqst_set_.lock);

    return (code);
}
