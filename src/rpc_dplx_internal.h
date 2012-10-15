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

#ifndef RPC_DPLX_INTERNAL_H
#define RPC_DPLX_INTERNAL_H

#include <rpc/rpc_dplx.h>

struct rpc_dplx_rec; /* in clnt_internal.h (avoids circular dependency) */

struct rpc_dplx_rec_set
{
    mutex_t clnt_fd_lock; /* XXX check dplx correctness */
    struct rbtree_x xt;
};

/* XXX perhaps better off as a flag bit (until we can remove it) */
#define rpc_flag_clear 0
#define rpc_lock_value 1

enum rpc_duplex_callpath
{
    RPC_DPLX_CLNT=1,
    RPC_DPLX_SVC
};

#define RPC_DPLX_FLAG_NONE          0x0000
#define RPC_DPLX_FLAG_LOCKED        0x0001
#define RPC_DPLX_FLAG_LOCK          0x0002 /* take chan lock before signal */
#define RPC_DPLX_FLAG_LOCKREC       0x0004
#define RPC_DPLX_FLAG_RECLOCKED     0x0008

#ifndef HAVE_STRLCAT
extern size_t strlcat(char *dst, const char *src, size_t siz);
#endif

#ifndef HAVE_STRLCPY
extern size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

#define RPC_DPLX_LKP_FLAG_NONE        0x0000
#define RPC_DPLX_LKP_IFLAG_LOCKREC    0x0001
#define RPC_DPLX_LKP_OFLAG_ALLOC      0x0002

struct rpc_dplx_rec *rpc_dplx_lookup_rec(int fd, uint32_t iflags,
                                         uint32_t *oflags);

static inline void
rpc_dplx_lock_init(struct rpc_dplx_lock *lock)
{
    lock->lock_flag_value = 0;
    mutex_init(&lock->we.mtx, NULL);
    cond_init(&lock->we.cv, 0, NULL);
}

static inline void
rpc_dplx_lock_destroy(struct rpc_dplx_lock *lock)
{
    mutex_destroy(&lock->we.mtx);
    cond_destroy(&lock->we.cv);
}

static inline int32_t
rpc_dplx_ref(struct rpc_dplx_rec *rec, u_int flags)
{
    int32_t refcnt;

    if (! (flags & RPC_DPLX_FLAG_LOCKED))
        mutex_lock(&rec->mtx);

    refcnt = ++(rec->refcnt);

    if (! (flags & RPC_DPLX_FLAG_LOCKED))
        mutex_unlock(&rec->mtx);

    return(refcnt);
}

int32_t rpc_dplx_unref(struct rpc_dplx_rec *rec, u_int flags);

/* swi:  send wait impl */
static inline void
rpc_dplx_swi(struct rpc_dplx_rec *rec, uint32_t wait_for)
{
    rpc_dplx_lock_t *lk = &rec->send.lock;

    mutex_lock(&lk->we.mtx);
    while (lk->lock_flag_value != rpc_flag_clear)
        cond_wait(&lk->we.cv, &lk->we.mtx);
    mutex_unlock(&lk->we.mtx);
}

/* swc: send wait clnt */ 
static inline void
rpc_dplx_swc(CLIENT *clnt, uint32_t wait_for)
{
    struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) clnt->cl_p2;
    rpc_dplx_swi(rec, wait_for);
}

/* rwi:  recv wait impl */
static inline void
rpc_dplx_rwi(struct rpc_dplx_rec *rec, uint32_t wait_for)
{
    rpc_dplx_lock_t *lk = &rec->recv.lock;

    mutex_lock(&lk->we.mtx);
    while (lk->lock_flag_value != rpc_flag_clear)
        cond_wait(&lk->we.cv, &lk->we.mtx);
    mutex_unlock(&lk->we.mtx);
}

/* rwc: recv wait clnt */ 
static inline void
rpc_dplx_rwc(CLIENT *clnt, uint32_t wait_for)
{
    struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) clnt->cl_p2;
    rpc_dplx_rwi(rec, wait_for);
}

/* ssi: send signal impl */
static inline void
rpc_dplx_ssi(struct rpc_dplx_rec *rec, uint32_t flags)
{
    rpc_dplx_lock_t *lk = &rec->send.lock;

    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_lock(&lk->we.mtx);
    cond_signal(&lk->we.cv);
    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_unlock(&lk->we.mtx);
}

/* ssc: send signal clnt */
static inline void
rpc_dplx_ssc(CLIENT *clnt, uint32_t flags)
{
    struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) clnt->cl_p2;
    rpc_dplx_ssi(rec, flags);
}

/* swf: send wait fd */
#define rpc_dplx_swf(fd, wait_for) \
    do { \
        struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd); \
        rpc_dplx_swi(rec, wait_for); \
    } while (0);


/* rsi: recv signal impl */
static inline void
rpc_dplx_rsi(struct rpc_dplx_rec *rec, uint32_t flags)
{
    rpc_dplx_lock_t *lk = &rec->recv.lock;

    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_lock(&lk->we.mtx);
    cond_signal(&lk->we.cv);
    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_unlock(&lk->we.mtx);
}

/* rsc: recv signal clnt */
static inline void
rpc_dplx_rsc(CLIENT *clnt, uint32_t flags)
{
    struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) clnt->cl_p2;
    rpc_dplx_rsi(rec, flags);
}

/* rwf: recv wait fd */
#define rpc_dplx_rwf(fd, wait_for) \
    do { \
        struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd); \
        rpc_dplx_rwi(rec, wait_for); \
    } while (0);

/* ssf: send signal fd */
#define rpc_dplx_ssf(fd, flags) \
    do { \
        struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd); \
        rpc_dplx_ssi(rec, flags); \
    } while (0);

/* rsf: send signal fd */
#define rpc_dplx_rsf(fd, flags) \
    do { \
        struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd); \
        rpc_dplx_rsi(rec, flags); \
    } while (0);

void rpc_dplx_shutdown(void);

#endif /* RPC_DPLX_INTERNAL_H */
