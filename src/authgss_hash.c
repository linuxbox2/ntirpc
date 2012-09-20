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

/* Portions Copyright (c) 2010-2011, ** others, update */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/types.h>
#include "rpc_com.h"
#include <intrinsic.h>
#include <misc/abstract_atomic.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>
#include <rpc/gss_internal.h>
#include "svc_internal.h"

/* GSS context cache */

struct authgss_x_part
{
    uint64_t gen;
    TAILQ_HEAD(ctx_tailq, svc_rpc_gss_data) lru_q;
};

struct authgss_hash_st
{
    mutex_t lock;
    struct rbtree_x xt;
    bool initialized;
};

static struct authgss_hash_st authgss_hash_st =
{
    PTHREAD_MUTEX_INITIALIZER, /* lock */
    {
        0, /* npart */
        RBT_X_FLAG_NONE, /* flags */
        255, /* cachesz */
        NULL /* tree */
    }, /* xt */
    FALSE /* initialized */
};

static inline uint64_t
gss_ctx_hash(gss_union_ctx_id_desc *gss_ctx)
{
    return ((uint64_t) gss_ctx->mech_type +
            (uint64_t) gss_ctx->internal_ctx_id);
}

static int
svc_rpc_gss_cmpf(const struct opr_rbtree_node *lhs,
                 const struct opr_rbtree_node *rhs)
{
    struct svc_rpc_gss_data *lk, *rk;

    lk = opr_containerof(lhs, struct svc_rpc_gss_data, node_k);
    rk = opr_containerof(rhs, struct svc_rpc_gss_data, node_k);

    if (lk->hk.k < rk->hk.k)
        return (-1);

    if (lk->hk.k == rk->hk.k)
        return (0);

    return (1);
}

void 
authgss_hash_init()
{
    int ix, code = 0;

    mutex_lock(&authgss_hash_st.lock);

    /* once */
    if (authgss_hash_st.initialized)
        goto unlock;

    code = rbtx_init(&authgss_hash_st.xt, svc_rpc_gss_cmpf,
                     __svc_params->gss.ctx_hash_partitions,
                     RBT_X_FLAG_ALLOC|RBT_X_FLAG_CACHE_RT);
    if (code)
        __warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
                "%s: rbtx_init failed", __func__);

    /* init read-through cache */
    for (ix = 0; ix < __svc_params->gss.ctx_hash_partitions; ++ix) {
        struct rbtree_x_part *xp = &(authgss_hash_st.xt.tree[ix]);
        struct authgss_x_part *axp;
        xp->cache = mem_zalloc(authgss_hash_st.xt.cachesz *
                               sizeof(struct opr_rbtree_node *));
        if (unlikely(! xp->cache)) {
            __warnx(TIRPC_DEBUG_FLAG_RPCSEC_GSS,
                    "%s: rbtx cache partition alloc failed", __func__);
            authgss_hash_st.xt.cachesz = 0;
            break;
        }
        /* partition ctx LRU */
        axp = (struct authgss_x_part *)
            mem_alloc(sizeof(struct authgss_x_part));
        TAILQ_INIT(&axp->lru_q);
        xp->u1 = axp;
    }
    authgss_hash_st.initialized = TRUE;

unlock:
    mutex_unlock(&authgss_hash_st.lock);
}

#define cond_init_authgss_hash() { \
        do { \
            if (! authgss_hash_st.initialized) \
                authgss_hash_init(); \
        } while (0); \
    }

struct svc_rpc_gss_data *
authgss_ctx_hash_get(struct rpc_gss_cred *gc)
{
    struct svc_rpc_gss_data gk, *gd = NULL;
    gss_union_ctx_id_desc *gss_ctx;
    struct opr_rbtree_node *ngd;
    struct authgss_x_part *axp;
    struct rbtree_x_part *t;

    cond_init_authgss_hash();

    gss_ctx = (gss_union_ctx_id_desc *) (gc->gc_ctx.value);
    gk.hk.k = gss_ctx_hash(gss_ctx);

    t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gk.hk.k);
    spin_lock(&t->sp);
    ngd = rbtree_x_cached_lookup(&authgss_hash_st.xt, t, &gk.node_k, gk.hk.k);
    if (ngd) {
        gd = opr_containerof(ngd, struct svc_rpc_gss_data, node_k);
        /* lru adjust */
        axp = (struct authgss_x_part *) t->u1;
        TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
        TAILQ_INSERT_TAIL(&axp->lru_q, gd, lru_q);
        ++(axp->gen);
        (void) atomic_inc_uint32_t(&gd->refcnt);
    }
    spin_unlock(&t->sp);
    (void) atomic_inc_uint32_t(&gd->gen);

    return (gd);
}

bool
authgss_ctx_hash_set(struct svc_rpc_gss_data *gd)
{
    struct rbtree_x_part *t;
    struct authgss_x_part *axp;
    bool rslt;

    cond_init_authgss_hash();

    ++(gd->refcnt); /* locked */
    t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gd->hk.k);
    spin_lock(&t->sp);
    rslt = rbtree_x_cached_insert(&authgss_hash_st.xt, t, &gd->node_k,
                                  gd->hk.k);
    if (rslt) {
        /* lru */
        axp = (struct authgss_x_part *) t->u1;
        TAILQ_INSERT_TAIL(&axp->lru_q, gd, lru_q);
    }
    spin_unlock(&t->sp);

    return (rslt);
}

bool
authgss_ctx_hash_del(struct svc_rpc_gss_data *gd)
{
    struct rbtree_x_part *t;
    struct authgss_x_part *axp;

    cond_init_authgss_hash();

    t = rbtx_partition_of_scalar(&authgss_hash_st.xt, gd->hk.k);
    spin_lock(&t->sp);
    rbtree_x_cached_remove(&authgss_hash_st.xt, t,  &gd->node_k, gd->hk.k);
    axp = (struct authgss_x_part *) t->u1;
    TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
    spin_unlock(&t->sp);

    /* release gd */
    unref_svc_rpc_gss_data(gd);

    return (TRUE);
}

void
authgss_ctx_gc_idle(void)
{
    struct rbtree_x_part *t;
    struct authgss_x_part *axp;
    struct svc_rpc_gss_data *gd;
    int ix, cnt;

    cond_init_authgss_hash();

    for (ix = 0; ix < authgss_hash_st.xt.npart; ++ix) {
        axp = (struct authgss_x_part *) t->u1;
        cnt = 0;
        spin_lock(&t->sp);
    again:
        gd = TAILQ_FIRST(&axp->lru_q);
        if (! gd)
            goto next_t;
        if (abs(axp->gen - gd->gen) > __svc_params->gss.max_idle_gen) {
            /* entry at LRU will eventually have no refs */
            if (gd->refcnt == 0) {
                rbtree_x_cached_remove(&authgss_hash_st.xt, t, &gd->node_k,
                                       gd->hk.k);
                TAILQ_REMOVE(&axp->lru_q, gd, lru_q);
                spin_unlock(&t->sp);
                unref_svc_rpc_gss_data(gd);
                spin_lock(&t->sp);
            }
            if (cnt++ <  __svc_params->gss.max_gc)
                goto again;
        }
    next_t:
        spin_unlock(&t->sp);
    }
}
