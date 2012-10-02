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
#include <reentrant.h>
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"
#include "svc_xprt.h"

#define SVC_XPRT_PARTITIONS 7

static bool initialized = FALSE;

static struct svc_xprt_set svc_xprt_set_ = {
    MUTEX_INITIALIZER /* svc_xprt_lock */,
    { 
        0, RBT_X_FLAG_NONE, 0, NULL
    } /* xt */
};

#define SVC_XPRT_FLAG_NONE       0x0000
#define SVC_XPRT_FLAG_CLEAR      0x0001
#define SVC_XPRT_FLAG_WLOCKED    0x0002
#define SVC_XPRT_FLAG_UNLOCK     0x0004

static inline int
svc_xprt_fd_cmpf(const struct opr_rbtree_node *lhs,
                 const struct opr_rbtree_node *rhs)
{
    struct svc_xprt_rec *lk, *rk;

    lk = opr_containerof(lhs, struct svc_xprt_rec, node_k);
    rk = opr_containerof(rhs, struct svc_xprt_rec, node_k);

    if (lk->fd_k < rk->fd_k)
        return (-1);

    if (lk->fd_k == rk->fd_k)
        return (0);

    return (1);
}

void
svc_xprt_init()
{
    int code = 0;

    mutex_lock(&svc_xprt_set_.lock);

    if (initialized)
        goto unlock;

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(&svc_xprt_set_.xt, svc_xprt_fd_cmpf /* NULL (inline) */,
                     SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
                "svc_xprt_init: rbtx_init failed");

    initialized = TRUE;

unlock:
    mutex_unlock(&svc_xprt_set_.lock);
}

#define cond_init_svc_xprt() {                  \
        do {                                    \
            if (! initialized)                  \
                svc_xprt_init();                \
        } while (0);                            \
    }

static inline struct svc_xprt_rec *
svc_xprt_lookup(int fd)
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

static inline SVCXPRT *
svc_xprt_insert(SVCXPRT *xprt, uint32_t flags)
{
    struct rbtree_x_part *t;
    struct svc_xprt_rec sk, *srec;
    struct opr_rbtree_node *nv;
    SVCXPRT *xprt2 = NULL;

    cond_init_svc_xprt();

    sk.fd_k = xprt->xp_fd;
    t = rbtx_partition_of_scalar(&svc_xprt_set_.xt, xprt->xp_fd);

    if (! (flags & SVC_XPRT_FLAG_WLOCKED))
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
            __warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
                    "%s: collision inserting in locked rbtree partition",
                    __func__);
            mutex_destroy(&srec->mtx);
            mem_free(srec, sizeof(struct svc_xprt_rec));
        }
    }

    if (flags & SVC_XPRT_FLAG_UNLOCK)
        rwlock_unlock(&t->lock);

    return (xprt2);
}

static inline SVCXPRT *
svc_xprt_set_impl(SVCXPRT *xprt, uint32_t flags)
{
    SVCXPRT *xprt2 = NULL;
    struct rbtree_x_part *t;
    struct svc_xprt_rec sk, *srec;
    struct opr_rbtree_node *ov;

    cond_init_svc_xprt();

    sk.fd_k = xprt->xp_fd;
    t = rbtx_partition_of_scalar(&svc_xprt_set_.xt, sk.fd_k);

    rwlock_wrlock(&t->lock);
    ov = opr_rbtree_lookup(&t->t, &sk.node_k);

    if (ov) {
        srec = opr_containerof(ov, struct svc_xprt_rec, node_k);
        mutex_lock(&srec->mtx);
        /* XXX state flags and refcount here? */
        if (! srec->xprt) {
            if (xprt->xp_gen == 0) {
                srec->gen++;
                xprt->xp_gen = srec->gen;
            }
            srec->xprt = xprt;
        } else {
            xprt2 = srec->xprt;
            if (flags & SVC_XPRT_FLAG_CLEAR) {
                /* XXX avoid bloat, remove cleared entries */
                opr_rbtree_remove(&t->t, &srec->node_k);
                srec->xprt = NULL;
                mutex_unlock(&srec->mtx);
                mutex_destroy(&srec->mtx);
                mem_free(srec, sizeof(struct svc_xprt_rec));
                goto unlock;
            }
            else
                srec->xprt = xprt;
        }
        mutex_unlock(&srec->mtx);
    } else {
        /* no srec */
        xprt2 = svc_xprt_insert(xprt, SVC_XPRT_FLAG_WLOCKED);
    }

unlock:
    rwlock_unlock(&t->lock);

    return (xprt2);
};

SVCXPRT *
svc_xprt_set(SVCXPRT *xprt)
{
    return (svc_xprt_set_impl(xprt, SVC_XPRT_FLAG_NONE));
}

SVCXPRT *
svc_xprt_clear(SVCXPRT *xprt)
{
    return (svc_xprt_set_impl(xprt, SVC_XPRT_FLAG_CLEAR));
}

SVCXPRT *
svc_xprt_get(int fd)
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

int
svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg)
{
    struct rbtree_x_part *t = NULL;
    struct opr_rbtree_node *n;
    struct svc_xprt_rec sk, *srec;
    SVCXPRT *xprt;
    int p_ix, x_ix, restarts, code = 0;
    uint64_t tgen;
    uint32_t rflag;

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
    restart:
        if (++restarts > 5)
            break;
        /* start with rlock */
        rwlock_rdlock(&t->lock); /* t RLOCKED */
        tgen = t->t.gen;
        x_ix = 0;
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            ++x_ix; /* diagnostic, index into logical srec sequence */
            srec = opr_containerof(n, struct svc_xprt_rec, node_k);
            mutex_lock(&srec->mtx);
            if (srec->xprt) {
                sk.fd_k = srec->fd_k;
                xprt = srec->xprt;

                /* call each_func with t !LOCKED, srec !LOCKED */
                mutex_unlock(&srec->mtx);
                rwlock_unlock(&t->lock);

                /* restart if each_f disposed xprt */
                rflag = each_f(xprt, arg);
                if (rflag == SVC_XPRT_FOREACH_CLEAR)
                    goto restart;

                /* invalidate */
                rwlock_rdlock(&t->lock);

                if (tgen != t->t.gen) {
                    /* invalidated, try harder */
                    n = opr_rbtree_lookup(&t->t, &sk.node_k);
                    if (! n) {
                        rwlock_unlock(&t->lock); /* t !LOCKED */
                        goto restart;
                    }
                }
            }
            n = opr_rbtree_next(n);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        p_ix++;
    } /* SVC_XPRT_PARTITIONS */

    return (code);
}

void svc_xprt_dump_xprts(const char *tag)
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
        rwlock_rdlock(&t->lock); /* t RLOCKED */
        __warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
                "xprts at %s: tree %d size %d", tag, p_ix, t->t.size);
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            srec = opr_containerof(n, struct svc_xprt_rec, node_k);
            __warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
                    "xprts at %s:  srec %p fd %d xprt %p xp_fd %d", tag, srec,
                    srec->fd_k, srec->xprt,
                    (srec->xprt) ? srec->xprt->xp_fd : 0);
            n = opr_rbtree_next(n);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        p_ix++;
    } /* SVC_XPRT_PARTITIONS */
out:
    return;
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
        rwlock_wrlock(&t->lock); /* t WLOCKED */
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            srec = opr_containerof(n, struct svc_xprt_rec, node_k);
            if (srec->xprt) {
                /* call each_func with t !LOCKED, srec LOCKED */
                mutex_lock(&srec->mtx);
                SVC_DESTROY(srec->xprt);
                srec->xprt = NULL;
                mutex_unlock(&srec->mtx);
                mutex_destroy(&srec->mtx);
            }
            /* now remove srec */
            opr_rbtree_remove(&t->t, &srec->node_k);
            /* and free it */
            mem_free(srec, sizeof(struct svc_xprt_rec));
            n = opr_rbtree_first(&t->t);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        rwlock_destroy(&t->lock);
        p_ix++;
    } /* SVC_XPRT_PARTITIONS */

    /* free tree */
    mem_free(svc_xprt_set_.xt.tree,
             SVC_XPRT_PARTITIONS*sizeof(struct rbtree_x_part));
out:
    return;
}
