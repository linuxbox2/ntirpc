
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

#define RBTX_REC_MAXPART 23

int rbtx_init(struct rbtree_x *xt, opr_rbtree_cmpf_t cmpf, uint32_t npart,
              uint32_t flags)
{
    int ix, code = 0;
    pthread_rwlockattr_t rwlock_attr;

    if ((npart > RBTX_REC_MAXPART) ||
        (npart % 2 == 0)) {
            __warnx("rbtx_init: value %d is an unlikely value for npart "
                    "(suggest a small prime)",
                    npart);
        }

    if (flags & RBT_X_FLAG_ALLOC)
        xt->tree = mem_alloc(npart * sizeof(struct rbtree_x_part));

    /* prior versions of Linux tirpc are subject to default prefer-reader
     * behavior (so have potential for writer starvation) */
    rwlockattr_init(&rwlock_attr);
#ifdef GLIBC
    pthread_rwlockattr_setkind_np(
        &rwlock_attr, 
        PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

    xt->npart = npart;

    for (ix = 0; ix < npart; ++ix) {
        rwlock_init(&xt->tree[ix].lock, &rwlock_attr);
        opr_rbtree_init(&xt->tree[ix].t, cmpf /* may be NULL */);
    }

    return (code);
}
