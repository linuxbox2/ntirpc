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

#include <sys/time.h>
#include <misc/opr.h>
#include <misc/rbtree_x.h>
#include <misc/wait_queue.h>
#include <rpc/svc.h>

typedef struct rpc_dplx_lock {
	struct wait_entry we;
	int32_t lock_flag_value;	/* XXX killme */
	struct {
		const char *func;
		int line;
	} locktrace;
} rpc_dplx_lock_t;

/* new unified state */
struct rpc_dplx_rec {
	struct rpc_svcxprt xprt;	/**< Transport Independent handle */

	int fd_k;
#if 0
	mutex_t mtx;
#else
	struct {
		mutex_t mtx;
		const char *func;
		int line;
	} locktrace;
#endif
	struct opr_rbtree_node node_k;
	uint32_t refcnt;
	struct {
		rpc_dplx_lock_t lock;
	} send;
	struct {
		rpc_dplx_lock_t lock;
	} recv;
};
#define REC_XPRT(p) (opr_containerof((p), struct rpc_dplx_rec, xprt))

#define REC_LOCK(rec) \
	do { \
		mutex_lock(&((rec)->locktrace.mtx)); \
		(rec)->locktrace.func = __func__; \
		(rec)->locktrace.line = __LINE__; \
	} while (0)

#define REC_UNLOCK(rec) mutex_unlock(&((rec)->locktrace.mtx))

struct rpc_dplx_rec_set {
	mutex_t clnt_fd_lock;	/* XXX check dplx correctness */
	struct rbtree_x xt;
};

/* XXX perhaps better off as a flag bit (until we can remove it) */
#define rpc_flag_clear 0
#define rpc_lock_value 1

#define RPC_DPLX_FLAG_NONE          0x0000
#define RPC_DPLX_FLAG_LOCKED        0x0001
#define RPC_DPLX_FLAG_LOCK          0x0002
#define RPC_DPLX_FLAG_LOCKREC       0x0004
#define RPC_DPLX_FLAG_RECLOCKED     0x0008
#define RPC_DPLX_FLAG_UNLOCK        0x0010

#ifndef HAVE_STRLCAT
extern size_t strlcat(char *, const char *, size_t);
#endif

#ifndef HAVE_STRLCPY
extern size_t strlcpy(char *, const char *src, size_t);
#endif

#define RPC_DPLX_LKP_FLAG_NONE        0x0000
#define RPC_DPLX_LKP_IFLAG_LOCKREC    0x0001
#define RPC_DPLX_LKP_OFLAG_ALLOC      0x0002

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

static inline void
rpc_dplx_rec_init(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_init(&rec->send.lock);
	rpc_dplx_lock_init(&rec->recv.lock);
}

static inline void
rpc_dplx_rec_destroy(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_destroy(&rec->send.lock);
	rpc_dplx_lock_destroy(&rec->recv.lock);
}

/* rlt: recv lock trace */
static inline void
rpc_dplx_rlt(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	if (__debug_flag(TIRPC_DEBUG_FLAG_LOCK)) {
		if (lk->locktrace.line) {
			__warnx(TIRPC_DEBUG_FLAG_LOCK,
				"%s:%d locking @%s:%d",
				func, line,
				lk->locktrace.func,
				lk->locktrace.line);
		} else {
			__warnx(TIRPC_DEBUG_FLAG_LOCK,
				"%s:%d locking",
				func, line);
		}
	}
	mutex_lock(&lk->we.mtx);
	lk->locktrace.func = (char *)func;
	lk->locktrace.line = line;
}

/* rli: recv lock impl */
#define rpc_dplx_rli(rec) \
	rpc_dplx_rlt(rec, __func__, __LINE__)

/* rui: recv unlock trace */
static inline void
rpc_dplx_rut(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	__warnx(TIRPC_DEBUG_FLAG_LOCK,
		"%s:%d unlocking @%s:%d",
		func, line,
		lk->locktrace.func,
		lk->locktrace.line);
	lk->locktrace.line = 0;
	mutex_unlock(&lk->we.mtx);
}

/* rli: recv lock impl */
#define rpc_dplx_rui(rec) \
	rpc_dplx_rut(rec, __func__, __LINE__)

static inline int32_t
rpc_dplx_ref(struct rpc_dplx_rec *rec, u_int flags)
{
	int32_t refcnt;

	if (!(flags & RPC_DPLX_FLAG_LOCKED))
		REC_LOCK(rec);

	refcnt = ++(rec->refcnt);

	/* release rec lock only if a) we took it and b) caller doesn't
	 * want it returned locked */
	if ((!(flags & RPC_DPLX_FLAG_LOCKED))
	    && (!(flags & RPC_DPLX_FLAG_LOCK)))
		REC_UNLOCK(rec);

	__warnx(TIRPC_DEBUG_FLAG_REFCNT, "%s: rec %p rec->refcnt %u", __func__,
		rec, refcnt);

	return (refcnt);
}

int32_t
rpc_dplx_unref(struct rpc_dplx_rec *rec, u_int flags);

/* rwi:  recv wait trace */
static inline void
rpc_dplx_rwt(struct rpc_dplx_rec *rec, const char *func, int line)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	__warnx(TIRPC_DEBUG_FLAG_LOCK,
		"%s:%d waiting @%s:%d",
		func, line,
		lk->locktrace.func,
		lk->locktrace.line);
	lk->locktrace.line = 0;
	cond_wait(&lk->we.cv, &lk->we.mtx);
	lk->locktrace.func = (char *)func;
	lk->locktrace.line = line;
}

/* rwi:  recv wait impl */
#define rpc_dplx_rwi(rec) \
	rpc_dplx_rwt(rec, __func__, __LINE__)

/* rsi: recv signal impl */
static inline void
rpc_dplx_rsi(struct rpc_dplx_rec *rec)
{
	rpc_dplx_lock_t *lk = &rec->recv.lock;

	cond_signal(&lk->we.cv);
}

#endif				/* RPC_DPLX_INTERNAL_H */
