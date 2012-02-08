
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
#include <unistd.h>
#include <signal.h>

#include <rpc/rpc.h>
#include <rpc/svc.h>

#include "rpc_com.h"
#include "clnt_internal.h"

#include <misc/rbtree_x.h>
#include "vc_lock.h"

#define VC_LOCK_PARTITIONS 17

static bool_t initialized = FALSE;

static struct vc_fd_rec_set vc_fd_rec_set = {
    PTHREAD_MUTEX_INITIALIZER /* clnt_fd_lock */,
    { 0, NULL } /* xt */
};

void vc_lock_init()
{
    int code = 0;

    mutex_lock(&vc_fd_rec_set.clnt_fd_lock);

    if (initialized)
        goto unlock;

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(&vc_fd_rec_set.xt, fd_cmpf /* NULL (inline) */,
                     VC_LOCK_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx("vc_lock_init: rbtx_init failed");

    initialized = TRUE;

unlock:
    mutex_unlock(&vc_fd_rec_set.clnt_fd_lock);
}

#define cond_init_vc_lock() { \
do { \
    if (! initialized) \
        vc_lock_init(); \
    } while (0); \
}

/* vc_fd_lock has the same semantics as legacy clnt_fd_lock mechanism,
 * but greater concurrency */

/* since the lifetime of all vc_fd_rec structures
 * is the life of the program, CLNT/SVCXPRT structures can keep a reference
 * to them in private data, and we can make the lock/unlock ops inline,
 * so amortized cost of this change for locks is 0. */

struct vc_fd_rec *vc_lookup_fd_rec(int fd)
{
    struct rbtree_x_part *t;
    struct vc_fd_rec ck, *crec = NULL;
    struct opr_rbtree_node *nv;

    cond_init_vc_lock();

    ck.fd_k = fd;
    t = rbtx_partition_of_scalar(&vc_fd_rec_set.xt, fd);

    /* find or install a vc_fd_rec at fd */
    rwlock_rdlock(&t->lock);
    nv = opr_rbtree_lookup(&t->t, &ck.node_k);
    rwlock_unlock(&t->lock);

    if (! nv) {
        rwlock_wrlock(&t->lock);
        nv = opr_rbtree_lookup(&t->t, &ck.node_k);
        if (! nv) {
            crec = mem_alloc(sizeof(struct vc_fd_rec));
            memset(crec, 0, sizeof(struct vc_fd_rec));
            mutex_init(&crec->mtx, NULL);
            cond_init(&crec->cv, 0, NULL);
            crec->fd_k = fd;
            if (opr_rbtree_insert(&t->t, &crec->node_k)) {
                /* cant happen */
                __warnx("%s: collision inserting in locked rbtree partition",
                        __func__);
                mem_free(crec, sizeof(struct vc_fd_rec));
            }
        }
        rwlock_unlock(&t->lock);
    }

    /* XXX safe, even if tree is reorganizing */
    crec = opr_containerof(nv, struct vc_fd_rec, node_k);

out:
    return (crec);    
}

void vc_fd_lock(int fd, sigset_t *mask)
{
    struct vc_fd_rec *crec = vc_lookup_fd_rec(fd);
    sigset_t newmask;

    assert(crec);

    sigfillset(&newmask);
    sigdelset(&newmask, SIGINT); /* XXXX debugger */
    thr_sigsetmask(SIG_SETMASK, &newmask, mask);

    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value) {
        cond_wait(&crec->cv, &crec->mtx);
    }
    crec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&crec->mtx);
}

void vc_fd_unlock(int fd, sigset_t *mask)
{
    struct vc_fd_rec *crec = vc_lookup_fd_rec(fd);

    assert(crec);

    mutex_lock(&crec->mtx);
    crec->lock_flag_value = rpc_flag_clear;
    mutex_unlock(&crec->mtx);
    thr_sigsetmask(SIG_SETMASK, mask, (sigset_t *) NULL);
    cond_signal(&crec->cv);
}
