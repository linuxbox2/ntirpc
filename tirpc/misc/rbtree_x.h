
#ifndef _RBTREE_X_H
#define _RBTREE_X_H

#include <config.h>
#include <misc/portable.h>
#include <inttypes.h>
#include <stdio.h>
#include <misc/rbtree.h>

struct rbtree_x_part
{
    CACHE_PAD(0);
    pthread_rwlock_t lock;
    pthread_mutex_t mtx;
    void *u1;
    void *u2;
    struct opr_rbtree t;
    struct opr_rbtree_node **cache;
    CACHE_PAD(1);
};

struct rbtree_x
{
    uint32_t npart;
    uint32_t flags;
    int32_t cachesz;
    struct rbtree_x_part *tree;
};

#define RBT_X_FLAG_NONE      0x0000
#define RBT_X_FLAG_ALLOC     0x0001

/* Cache strategies.
 *
 * In read-through strategy, entries are always inserted in the
 * tree, but lookups may be O(1) when an entry is shadowed in cache.
 *
 * In the write through strategy, t->cache and t->tree partition
 * t, and t->cache is always consulted first.
 */

#define RBT_X_FLAG_CACHE_RT   0x0002
#define RBT_X_FLAG_CACHE_WT   0x0004

#define rbtx_partition_of_scalar(xt, k) (((xt)->tree)+((k)%(xt)->npart))

extern int rbtx_init(struct rbtree_x *xt, opr_rbtree_cmpf_t cmpf,
                     uint32_t npart, uint32_t flags);

static inline struct opr_rbtree_node *
rbtree_x_cached_lookup(struct rbtree_x *xt, struct rbtree_x_part *t,
                       struct opr_rbtree_node *nk, uint64_t hk)
{
    struct opr_rbtree_node *nv_cached, *nv = NULL;
    uint32_t offset;

    if (! t)
        t = rbtx_partition_of_scalar(xt, hk);

    offset = hk % xt->cachesz;
    nv_cached = t->cache[offset];
    if (nv_cached) {
        if (t->t.cmpf(nv_cached, nk) == 0) {
            nv = nv_cached;
            goto out;
        }
    }

    nv = opr_rbtree_lookup(&t->t, nk);
    if (nv && (xt->flags & RBT_X_FLAG_CACHE_RT)) {
        t->cache[offset] = nv;
    }

    __warnx(TIRPC_DEBUG_FLAG_LOCK,
            "rbtree_x_cached_lookup: t %p nk %p nv %p"
            "(%s hk %"PRIx64" slot/offset %d)",
            t, nk, nv, (nv_cached) ? "CACHED" : "",
            hk, offset);

out:
    return (nv);
}

static inline struct opr_rbtree_node *
rbtree_x_cached_insert(struct rbtree_x *xt, struct rbtree_x_part *t,
                       struct opr_rbtree_node *nk, uint64_t hk)
{
    struct opr_rbtree_node *v_cached, *nv = NULL;
    uint32_t offset;

    if (! t)
        t = rbtx_partition_of_scalar(xt, hk);

    offset= hk % xt->cachesz;
    v_cached = t->cache[offset];
    if (xt->flags & RBT_X_FLAG_CACHE_WT) {
        if (! v_cached)
            nv = t->cache[offset] = nk;
        else {
            nv = opr_rbtree_insert(&t->t, nk);
	    if (! nv)
	      nv = nk;
	}
    } else {
        /* RBT_X_FLAG_CACHE_RT */
        nv = v_cached = t->cache[offset] = nk;
        (void) opr_rbtree_insert(&t->t, nk);
    }

    __warnx(TIRPC_DEBUG_FLAG_LOCK,
            "rbtree_x_cached_insert_wt: t %p inserting %p "
            "(%s hk %"PRIx64" slot/offset %d)",
            t, nk, (v_cached) ? "chaining" : "caching",
            hk, offset);

    return (nv);
}

static inline void
rbtree_x_cached_remove(struct rbtree_x *xt, struct rbtree_x_part *t,
                       struct opr_rbtree_node *nk, uint64_t hk)
{
    struct opr_rbtree_node *v_cached;
    uint32_t offset;

    if (! t)
        t = rbtx_partition_of_scalar(xt, hk);

    offset = hk % xt->cachesz;
    v_cached = t->cache[offset];

    __warnx(TIRPC_DEBUG_FLAG_LOCK,
            "rbtree_x_cached_remove_wt: t %p removing %p "
        "(%s hk %"PRIx64" slot/offset %d)",
        t, nk, (v_cached) ? "caching" : "chaining",
        hk, offset);

    if (xt->flags & RBT_X_FLAG_CACHE_WT) {
        if (v_cached && (t->t.cmpf(nk, v_cached) == 0))
            t->cache[offset] = NULL;
        else {
            return (opr_rbtree_remove(&t->t, nk));
        }
    } else {
        /* RBT_X_FLAG_CACHE_RT */
        if (v_cached && (t->t.cmpf(nk, v_cached) == 0))
            t->cache[offset] = NULL;
        return (opr_rbtree_remove(&t->t, nk));
    }
}

#endif /* _RBTREE_X_H */
