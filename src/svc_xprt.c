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

#define SVC_XPRT_PARTITIONS 17

static bool_t initialized = FALSE;

static struct svc_xprt_set svc_xprt_set_ = {
    PTHREAD_MUTEX_INITIALIZER /* svc_xprt_lock */,
    NULL /* xt */
};

void svc_xprt_init()
{
    int code = 0;

    mutex_lock(&svc_xprt_set_.lock);

    if (initialized)
        return;

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(svc_xprt_set_.xt, fd_cmpf /* NULL (inline) */,
                     SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx("svc_xprt_init: rbtx_init failed");

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
    t = rbtx_partition_of_scalar(svc_xprt_set_.xt, fd);

    rwlock_rdlock(&t->lock);
    nv = opr_rbtree_lookup(&t->head, &sk.node_k);
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
    t = rbtx_partition_of_scalar(svc_xprt_set_.xt, xprt->xp_fd);

    rwlock_wrlock(&t->lock);
    nv = opr_rbtree_lookup(&t->head, &sk.node_k);
    if (! nv) {
        srec = mem_alloc(sizeof(struct svc_xprt_rec));
        mutex_init(&srec->mtx, NULL);
        srec->fd_k = xprt->xp_fd;
        srec->xprt = xprt;
        srec->gen = 1;
    }
    rwlock_unlock(&t->lock);

    return (xprt2);
}

SVCXPRT* svc_xprt_set(SVCXPRT *xprt)
{
    SVCXPRT *xprt2 = NULL;
    struct svc_xprt_rec *srec = svc_xprt_lookup(xprt->xp_fd);

    if (srec){
        mutex_lock(&srec->mtx);
        /* XXX state flags and refcount here? */
        if (! srec->xprt) {
            if (xprt) /* may be NULL */
                if (xprt->xp_gen == 0) {
                    srec->gen++;
                    xprt->xp_gen = srec->gen;
                }
            srec->xprt = xprt;            
        }
        else
            xprt2 = srec->xprt;
        mutex_unlock(&srec->mtx);
    } else {
        /* no srec */
        xprt2 = svc_xprt_insert(xprt);
    }

    return (xprt2);
};

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
