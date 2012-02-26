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

#include <misc/rbtree_x.h>

#include "clnt_internal.h"
#include "vc_lock.h"
#include "svc_xprt.h"

#define SVC_XPRT_PARTITIONS 7

static bool_t initialized = FALSE;

static struct svc_xprt_set svc_xprt_set_ = {
    PTHREAD_MUTEX_INITIALIZER /* svc_xprt_lock */,
    { 0, NULL } /* xt */
};

void svc_xprt_init()
{
    int code = 0;

    mutex_lock(&svc_xprt_set_.lock);

    if (initialized)
        goto unlock;

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(&svc_xprt_set_.xt, fd_cmpf /* NULL (inline) */,
                     SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx("svc_xprt_init: rbtx_init failed");

    initialized = TRUE;

unlock:
    mutex_unlock(&svc_xprt_set_.lock);
}

#define cond_init_svc_xprt() { \
do { \
    if (! initialized) \
        svc_xprt_init(); \
    } while (0); \
}

static inline struct svc_xprt_rec *svc_xprt_lookup(int fd)
{
    struct rbtree_x_part *t;
    struct svc_xprt_rec sk, *srec = NULL;
    struct opr_rbtree_node *nv;

    cond_init_svc_xprt();

    sk.fd_k = fd;
    t = rbtx_partition_of_scalar(&svc_xprt_set_.xt, fd);

    rwlock_rdlock(&t->lock);
    nv = opr_rbtree_lookup(&t->t, &sk.node_k);
    rwlock_unlock(&t->lock);

    /* XXX safe, even if tree is reorganizing */
    if (nv)
        srec = opr_containerof(nv, struct svc_xprt_rec, node_k);

    return (srec);    
}

static inline SVCXPRT *svc_xprt_insert(SVCXPRT *xprt)
{
    struct rbtree_x_part *t;
    struct svc_xprt_rec sk, *srec;
    struct opr_rbtree_node *nv;
    SVCXPRT *xprt2 = NULL;

    cond_init_svc_xprt();

    sk.fd_k = xprt->xp_fd;
    t = rbtx_partition_of_scalar(&svc_xprt_set_.xt, xprt->xp_fd);

    rwlock_wrlock(&t->lock);
    nv = opr_rbtree_lookup(&t->t, &sk.node_k);
    if (! nv) {
        srec = mem_alloc(sizeof(struct svc_xprt_rec));
        mutex_init(&srec->mtx, NULL);
        srec->fd_k = xprt->xp_fd;
        srec->xprt = xprt;
        srec->gen = 1;
        if (opr_rbtree_insert(&t->t, &srec->node_k)) {
            /* cant happen */
            __warnx("%s: collision inserting in locked rbtree partition",
                    __func__);
            mem_free(srec, sizeof(struct svc_xprt_rec));
        }
    }
    rwlock_unlock(&t->lock);

    return (xprt2);
}

#define SVC_XPRT_FLAG_NONE       0x0000
#define SVC_XPRT_FLAG_CLEAR      0x0001

static inline SVCXPRT* svc_xprt_set_impl(SVCXPRT *xprt, uint32_t flags)
{
    SVCXPRT *xprt2 = NULL;
    struct svc_xprt_rec *srec = svc_xprt_lookup(xprt->xp_fd);

    if (srec) {
        mutex_lock(&srec->mtx);
        /* XXX state flags and refcount here? */
        if (! srec->xprt) {
            if (xprt->xp_gen == 0) {
                srec->gen++;
                xprt->xp_gen = srec->gen;
            }
        }
        else {
            xprt2 = srec->xprt;
            if (flags & SVC_XPRT_FLAG_CLEAR)
                srec->xprt = NULL;
            else
                srec->xprt = xprt;
        }
        mutex_unlock(&srec->mtx);
    } else {
        /* no srec */
        xprt2 = svc_xprt_insert(xprt);
    }

    return (xprt2);
};

SVCXPRT* svc_xprt_set(SVCXPRT *xprt)
{
    return (svc_xprt_set_impl(xprt, SVC_XPRT_FLAG_NONE));
}

SVCXPRT *svc_xprt_clear(SVCXPRT *xprt)
{
    return (svc_xprt_set_impl(xprt, SVC_XPRT_FLAG_CLEAR));
}

SVCXPRT* svc_xprt_get(int fd)
{
    SVCXPRT *xprt = NULL;
    struct svc_xprt_rec *srec = svc_xprt_lookup(fd);

    if (srec) {
        mutex_lock(&srec->mtx);
        xprt = srec->xprt;
        mutex_unlock(&srec->mtx);
    }

    return (xprt);    
}

int svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg)
{
    struct rbtree_x_part *t = NULL;
    struct opr_rbtree_node *n;
    struct svc_xprt_rec sk, *srec;
    uint64_t gen;
    int p_ix, x_ix, restarts, code = 0;

    cond_init_svc_xprt();

    /* concurrent, restartable iteration over t */
    p_ix = 0;
    while (p_ix < SVC_XPRT_PARTITIONS) {
        t = &svc_xprt_set_.xt.tree[p_ix];
        restarts = 0;
        /* TI-RPC __svc_clean_idle held global svc_fd_lock
         * exclusive locked for a full scan of the legacy svc_xprts
         * array.  We avoid this via tree partitioning and by
         * operating mostly unlocked. */
        rwlock_rdlock(&t->lock); /* t RLOCKED */
    restart:
        if (++restarts > 5)
            break;
        gen = t->t.gen;
        x_ix = 0;
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            ++x_ix; /* diagnostic, index into logical srec sequence */
            srec = opr_containerof(n, struct svc_xprt_rec, node_k);
            if (srec->xprt) {
                sk.fd_k = srec->fd_k;
            
                /* call each_func with t !LOCKED, srec LOCKED */
                mutex_lock(&srec->mtx);
                rwlock_unlock(&t->lock);
                each_f(srec->xprt, arg);
                mutex_unlock(&srec->mtx);

                /* can invalidate */
                rwlock_rdlock(&t->lock);

                if (gen != t->t.gen) {
                    /* invalidated, try harder */
                    n = opr_rbtree_lookup(&t->t, &sk.node_k);
                    if (!n)
                        goto restart;
                }
            }
            n = opr_rbtree_next(n);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        p_ix++;
    } /* SVC_XPRT_PARTITIONS */

    return (code);
}

void svc_xprt_shutdown()
{
    struct rbtree_x_part *t = NULL;
    struct opr_rbtree_node *n;
    struct svc_xprt_rec *srec;
    int p_ix;

    if (! initialized)
        goto out;

    p_ix = 0;
    while (p_ix < SVC_XPRT_PARTITIONS) {
        t = &svc_xprt_set_.xt.tree[p_ix];
        rwlock_wrlock(&t->lock); /* t RLOCKED */
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            srec = opr_containerof(n, struct svc_xprt_rec, node_k);
            if (srec->xprt) {            
                /* call each_func with t !LOCKED, srec LOCKED */
                mutex_lock(&srec->mtx);
                SVC_DESTROY(srec->xprt);
                srec->xprt = NULL;
                mutex_unlock(&srec->mtx);
            }
            /* now remove srec */
            opr_rbtree_remove(&t->t, &srec->node_k);
            /* and free it */
            mem_free(srec, sizeof(struct svc_xprt_rec));    
            n = opr_rbtree_first(&t->t);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        p_ix++;
    } /* SVC_XPRT_PARTITIONS */

    /* free tree */
    mem_free(svc_xprt_set_.xt.tree,
             SVC_XPRT_PARTITIONS*sizeof(struct rbtree_x_part));
out:
    return;
}
