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

#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif				/* PORTMAP */
#include "rpc_com.h"
#include "rpc_dplx_internal.h"
#include "svc_xprt.h"

#define SVC_XPRT_PARTITIONS 7

static bool initialized;

struct svc_xprt_fd {
	mutex_t lock;
	struct rbtree_x xt;
};

static struct svc_xprt_fd svc_xprt_fd = {
	MUTEX_INITIALIZER /* svc_xprt_lock */ ,
	{
	 0,			/* npart */
	 RBT_X_FLAG_NONE,	/* flags */
	 0,			/* cachesz */
	 NULL			/* tree */
	}			/* xt */
};

static inline int
svc_xprt_fd_cmpf(const struct opr_rbtree_node *lhs,
		 const struct opr_rbtree_node *rhs)
{
	SVCXPRT *lk, *rk;

	lk = opr_containerof(lhs, struct rpc_svcxprt, xp_fd_node);
	rk = opr_containerof(rhs, struct rpc_svcxprt, xp_fd_node);

	if (lk->xp_fd < rk->xp_fd)
		return (-1);

	if (lk->xp_fd == rk->xp_fd)
		return (0);

	return (1);
}

int
svc_xprt_init(void)
{
	int code = 0;

	mutex_lock(&svc_xprt_fd.lock);

	if (initialized)
		goto unlock;

	/* one of advantages of this RBT is convenience of external
	 * iteration, we'll go to that shortly */
	code =
	    rbtx_init(&svc_xprt_fd.xt, svc_xprt_fd_cmpf /* NULL (inline) */ ,
		      SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
	if (code)
		__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
			"svc_xprt_init: rbtx_init failed");

	initialized = true;

 unlock:
	mutex_unlock(&svc_xprt_fd.lock);
	return (code);
}

static inline bool
svc_xprt_init_failure(void)
{
	if (initialized)
		return (false);
	return (svc_xprt_init() != 0);
}

/*
 * On success, returns with RPC_DPLX_FLAG_LOCKED
 */
SVCXPRT *
svc_xprt_lookup(int fd, svc_xprt_setup_t setup)
{
	struct rpc_svcxprt sk;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *nv;
	SVCXPRT *xprt = NULL;

	if (svc_xprt_init_failure())
		return (NULL);

	sk.xp_fd = fd;
	t = rbtx_partition_of_scalar(&svc_xprt_fd.xt, fd);

	rwlock_rdlock(&t->lock);
	nv = opr_rbtree_lookup(&t->t, &sk.xp_fd_node);
	if (!nv) {
		rwlock_unlock(&t->lock);
		rwlock_wrlock(&t->lock);
		nv = opr_rbtree_lookup(&t->t, &sk.xp_fd_node);
		if (!nv) {
			(*setup)(&xprt); /* zalloc, xp_refs = 1 */
			xprt->xp_fd = fd;
			xprt->xp_flags = SVC_XPRT_FLAG_INITIAL;

			rpc_dplx_rli(REC_XPRT(xprt));
			if (opr_rbtree_insert(&t->t, &xprt->xp_fd_node)) {
				/* cant happen */
				rpc_dplx_rui(REC_XPRT(xprt));
				__warnx(TIRPC_DEBUG_FLAG_LOCK,
					"%s: collision inserting in locked rbtree partition",
					__func__);
				(*setup)(&xprt);	/* free, sets NULL */
			}
			rwlock_unlock(&t->lock);
			return (xprt);
		}
		/* raced, fallthru */
	}
	xprt = opr_containerof(nv, struct rpc_svcxprt, xp_fd_node);

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	rpc_dplx_rli(REC_XPRT(xprt));
	rwlock_unlock(&t->lock);

	if (unlikely(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
		/* do not return destroyed xprts */
		rpc_dplx_rui(REC_XPRT(xprt));
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		return (NULL);
	}

	atomic_clear_uint16_t_bits(&xprt->xp_flags, SVC_XPRT_FLAG_INITIAL);
	return (xprt);
}

SVCXPRT *
svc_xprt_get(int fd)
{
	struct rpc_svcxprt sk;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *nv;
	SVCXPRT *srec = NULL;

	if (svc_xprt_init_failure())
		return (NULL);

	sk.xp_fd = fd;
	t = rbtx_partition_of_scalar(&svc_xprt_fd.xt, fd);

	rwlock_rdlock(&t->lock);
	nv = opr_rbtree_lookup(&t->t, &sk.xp_fd_node);
	rwlock_unlock(&t->lock);

	/* XXX safe, even if tree is reorganizing */
	if (nv)
		srec = opr_containerof(nv, struct rpc_svcxprt, xp_fd_node);

	return (srec);
}

/**
 * Clear an xprt
 *
 * @note Locking
 * - xprt is locked, unless RPC_DPLX_FLAG_LOCKED is passed
 * - xprt is unlocked if RPC_DPLX_FLAG_UNLOCK is passed, otherwise it is
 *   returned locked
 */
void
svc_xprt_clear(SVCXPRT *xprt, uint32_t flags)
{
	struct rbtree_x_part *t;

	if (svc_xprt_init_failure())
		return;

	if (!(flags & RPC_DPLX_FLAG_LOCKED))
		rpc_dplx_rli(REC_XPRT(xprt));

	if (opr_rbtree_node_valid(&xprt->xp_fd_node)) {
		t = rbtx_partition_of_scalar(&svc_xprt_fd.xt, xprt->xp_fd);

		rwlock_wrlock(&t->lock);
		opr_rbtree_remove(&t->t, &xprt->xp_fd_node);
		rwlock_unlock(&t->lock);
	}

	if (flags & RPC_DPLX_FLAG_UNLOCK)
		rpc_dplx_rui(REC_XPRT(xprt));
}

int
svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg)
{
	struct rpc_svcxprt sk;
	struct rbtree_x_part *t;
	struct opr_rbtree_node *n;
	SVCXPRT *xprt;
	uint64_t tgen;
	uint32_t rflag;
	int p_ix;
	int x_ix;
	int restarts;

	if (svc_xprt_init_failure())
		return (-1);

	/* concurrent, restartable iteration over t */
	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];
		restarts = 0;
		/* TI-RPC __svc_clean_idle held global svc_fd_lock
		 * exclusive locked for a full scan of the legacy svc_xprts
		 * array.  We avoid this via tree partitioning and by
		 * operating mostly unlocked. */
 restart:
		if (++restarts > 5)
			return (1);

		/* start with rlock */
		rwlock_rdlock(&t->lock);	/* t RLOCKED */
		tgen = t->t.gen;
		x_ix = 0;
		n = opr_rbtree_first(&t->t);
		while (n != NULL) {
			++x_ix;	/* diagnostic, index into logical srec
				 * sequence */
			xprt = opr_containerof(n, struct rpc_svcxprt, xp_fd_node);
			sk.xp_fd = xprt->xp_fd;

			/* call each_func with t !LOCKED */
			rwlock_unlock(&t->lock);

			/* restart if each_f disposed xprt */
			rflag = each_f(xprt, arg);
			if (rflag == SVC_XPRT_FOREACH_CLEAR)
				goto restart;

			/* validate */
			rwlock_rdlock(&t->lock);

			if (tgen != t->t.gen) {
				n = opr_rbtree_lookup(&t->t, &sk.xp_fd_node);
				if (!n) {
					/* invalidated, try harder */
					rwlock_unlock(&t->lock);
							/* t !LOCKED */
					goto restart;
				}
			}
			n = opr_rbtree_next(n);
		}		/* curr partition */
		rwlock_unlock(&t->lock); /* t !LOCKED */
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */

	return (0);
}

void
svc_xprt_dump_xprts(const char *tag)
{
	struct rbtree_x_part *t = NULL;
	struct opr_rbtree_node *n;
	SVCXPRT *xprt;
	int p_ix;

	if (!initialized)
		goto out;

	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];
		rwlock_rdlock(&t->lock);	/* t RLOCKED */
		__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
			"xprts at %s: tree %d size %d", tag, p_ix, t->t.size);
		n = opr_rbtree_first(&t->t);
		while (n != NULL) {
			xprt = opr_containerof(n, struct rpc_svcxprt, xp_fd_node);
			__warnx(TIRPC_DEBUG_FLAG_SVC_XPRT,
				"xprts at %s: %p xp_fd %d",
				tag, xprt, xprt->xp_fd);
			n = opr_rbtree_next(n);
		}		/* curr partition */
		rwlock_unlock(&t->lock);	/* t !LOCKED */
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */
 out:
	return;
}

void
svc_xprt_shutdown()
{
	struct rbtree_x_part *t;
	struct opr_rbtree_node *n;
	SVCXPRT *xprt;
	int p_ix;

	if (!initialized)
		return;

	p_ix = 0;
	while (p_ix < SVC_XPRT_PARTITIONS) {
		t = &svc_xprt_fd.xt.tree[p_ix];

		rwlock_wrlock(&t->lock);	/* t WLOCKED */
		n = opr_rbtree_first(&t->t);
		while (n != NULL) {
			xprt = opr_containerof(n, struct rpc_svcxprt, xp_fd_node);
			n = opr_rbtree_next(n);

			/* prevent repeats, see svc_xprt_clear() */
			rpc_dplx_rli(REC_XPRT(xprt));
			opr_rbtree_remove(&t->t, &xprt->xp_fd_node);
			rpc_dplx_rui(REC_XPRT(xprt));

			SVC_DESTROY(xprt);
		}		/* curr partition */
		rwlock_unlock(&t->lock);	/* t !LOCKED */
		rwlock_destroy(&t->lock);
		p_ix++;
	}			/* SVC_XPRT_PARTITIONS */

	/* free tree */
	mem_free(svc_xprt_fd.xt.tree,
		 SVC_XPRT_PARTITIONS * sizeof(struct rbtree_x_part));
}

void
svc_xprt_trace(SVCXPRT *xprt, const char *func, const char *tag, const int line)
{
	__warnx(TIRPC_DEBUG_FLAG_REFCNT,
		"%s() %p xp_refs %" PRId32
		" fd %d af %u port %u @ %s:%d",
		func, xprt, xprt->xp_refs,
		xprt->xp_fd,
		xprt->xp_remote.ss.ss_family,
		__rpc_address_port(&xprt->xp_remote),
		tag, line);
}
