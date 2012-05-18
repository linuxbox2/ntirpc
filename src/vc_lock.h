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

#ifndef TIRPC_VC_LOCK_H
#define TIRPC_VC_LOCK_H

struct vc_fd_rec; /* in clnt_internal.h (avoids circular dependency) */

struct vc_fd_rec_set
{
    mutex_t clnt_fd_lock; /* global mtx we'll try to spam less than formerly */
    struct rbtree_x xt;
};

/* XXX perhaps better off as a flag bit */
#define rpc_flag_clear 0
#define rpc_lock_value 1

#define VC_LOCK_FLAG_NONE          0x0000
#define VC_LOCK_FLAG_MTX_LOCKED    0x0001
#define VC_LOCK_FLAG_LOCK          0x0002 /* take crec->mtx before signal */

static inline int32_t vc_lock_ref(struct vc_fd_rec *crec, u_int flags)
{
    int32_t refcount;

    if (! (flags & VC_LOCK_FLAG_MTX_LOCKED))
        mutex_lock(&crec->mtx);

    refcount = ++(crec->refcount);

    if (! (flags & VC_LOCK_FLAG_MTX_LOCKED))
        mutex_unlock(&crec->mtx);

    return(refcount);
}

int32_t vc_lock_unref(struct vc_fd_rec *crec, u_int flags);
void vc_fd_lock(int fd, sigset_t *mask);
void vc_fd_unlock(int fd, sigset_t *mask);
struct vc_fd_rec *vc_lookup_fd_rec(int fd);

static inline void vc_lock_init_cl(CLIENT *cl)
{
    /* XXX switch */
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    if (! cx->cx_crec) {
        /* many clients (and xprts) shall point to crec */
        cx->cx_crec = vc_lookup_fd_rec(cx->cx_fd); /* ref+1 */
    }
}

static inline void vc_lock_init_xprt(SVCXPRT *xprt)
{
    if (! xprt->xp_p5) {
        /* many xprts shall point to crec */
        xprt->xp_p5 = vc_lookup_fd_rec(xprt->xp_fd); /* ref+1 */
    }
}

#define AMTX 1

static inline void vc_fd_lock_impl(struct vc_fd_rec *crec, sigset_t *mask)
{
    sigset_t newmask;

#if AMTX
    mutex_lock(&crec->mtx);
#else
    sigfillset(&newmask);
    sigdelset(&newmask, SIGINT); /* XXXX debugger */
    thr_sigsetmask(SIG_SETMASK, &newmask, mask);

    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value)
        cond_wait(&crec->cv, &crec->mtx);
    crec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&crec->mtx);
#endif
}

static inline void vc_fd_unlock_impl(struct vc_fd_rec *crec, sigset_t *mask)
{
#if AMTX
    mutex_unlock(&crec->mtx);
#else
    mutex_lock(&crec->mtx);
    crec->lock_flag_value = rpc_flag_clear;
    cond_signal(&crec->cv);
    mutex_unlock(&crec->mtx);
    thr_sigsetmask(SIG_SETMASK, mask, (sigset_t *) NULL);
#endif
}

static inline void vc_fd_wait_impl(struct vc_fd_rec *crec, uint32_t wait_for)
{
    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value != rpc_flag_clear)
        cond_wait(&crec->cv, &crec->mtx);
    mutex_unlock(&crec->mtx);
}

static inline void vc_fd_signal_impl(struct vc_fd_rec *crec, uint32_t flags)
{
    if (flags & VC_LOCK_FLAG_LOCK)
        mutex_lock(&crec->mtx);
    cond_signal(&crec->cv);
    if (flags & VC_LOCK_FLAG_LOCK)
        mutex_unlock(&crec->mtx);
}

static inline void vc_fd_lock_c(CLIENT *cl, sigset_t *mask)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    vc_lock_init_cl(cl);
    vc_fd_lock_impl(cx->cx_crec, mask);
}

static inline void vc_fd_unlock_c(CLIENT *cl, sigset_t *mask)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    /* unless lock order violation, cl is lock-initialized */
    vc_fd_unlock_impl(cx->cx_crec, mask);
}

void vc_fd_wait(int fd, uint32_t wait_for);

static inline void vc_fd_wait_c(CLIENT *cl, uint32_t wait_for)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    vc_lock_init_cl(cl);
    vc_fd_wait_impl(cx->cx_crec, wait_for);
}

void vc_fd_signal(int fd, uint32_t flags);

static inline void vc_fd_signal_c(CLIENT *cl, uint32_t flags)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    vc_lock_init_cl(cl);
    vc_fd_signal_impl(cx->cx_crec, flags);
}

static inline void vc_fd_lock_x(SVCXPRT *xprt, sigset_t *mask)
{ 
    vc_lock_init_xprt(xprt);
    vc_fd_lock_impl((struct vc_fd_rec *) xprt->xp_p5, mask);
}

static inline void vc_fd_unlock_x(SVCXPRT *xprt, sigset_t *mask)
{
    vc_lock_init_xprt(xprt);
    vc_fd_unlock_impl((struct vc_fd_rec *) xprt->xp_p5, mask);
}

static inline void vc_lock_unref_clnt(CLIENT *cl)
{
    int32_t refcount __attribute__((unused)) = 0;
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    if (cx->cx_crec) {
        refcount = vc_lock_unref(cx->cx_crec, VC_LOCK_FLAG_NONE);
        cx->cx_crec = NULL;
    }
}

static inline void vc_lock_unref_xprt(SVCXPRT *xprt)
{
    int32_t refcount __attribute__((unused)) = 0;

    if (xprt->xp_p5) {
        refcount = vc_lock_unref((struct vc_fd_rec *) xprt->xp_p5,
                                 VC_LOCK_FLAG_NONE);
        xprt->xp_p5 = NULL;
    }
}

void vc_lock_shutdown();

#endif /* TIRPC_VC_LOCK_H */
