
#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <rpc/types.h>

#include <misc/rbtree_x.h>
#include "vc_lock.h"

#define VC_LOCK_PARTITIONS 17

static bool_t initialized = FALSE;

static struct vc_fd_rec_set vc_fd_rec_set = {
    PTHREAD_MUTEX_INITIALIZER /* clnt_fd_lock */,
    NULL /* xt */
};

void vc_lock_init()
{
    int code = 0;

    mutex_lock(&vc_fd_rec_set.clnt_fd_lock);

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(vc_fd_rec_set.xt, fd_cmpf /* NULL (inline) */,
                     VC_LOCK_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx("vc_lock_init: rbtx_init failed");

    mutex_unlock(&vc_fd_rec_set.clnt_fd_lock);
}

#define cond_init_vc_lock() { \
do { \
    if (! initialized) \
        vc_lock_init(); \
    } while (0); \
}

const int rpc_lock_value = 1; /* XXX perhaps better off as a flag bit */

/* vc_fd_lock has the same semantics as legacy clnt_fd_lock mechanism,
 * but greater concurrency */

/* this is the lock logic, but since the lifetime of these structures
 * is the life of the program, CLNT/SVCXPRT structures can keep a reference
 * to them in private data, and we can make the lock/unlock ops inline,
 * so amortized cost of this change for locks is 0. */

void vc_fd_lock(int fd)
{
    struct rbtree_x_part *t;
    struct vc_fd_rec ck, *crec;
    struct opr_rbtree_node *nv;

    cond_init_vc_lock();

    t = rbtx_partition_of_scalar(vc_fd_rec_set.xt, fd);

    ck.fd_k = fd;
    rwlock_rdlock(&t->lock);
    nv = opr_rbtree_lookup(&t->head, &ck.node_k);

    assert(nv);

    crec = opr_containerof(nv, struct vc_fd_rec, node_k);
    rwlock_unlock(&t->lock);

    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value) {
        cond_wait(&crec->cv, &crec->mtx);
    }
    crec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&crec->mtx);
}

void vc_fd_unlock(int fd)
{
}
