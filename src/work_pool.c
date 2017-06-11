/*
 * Copyright (c) 2013-2015 CohortFS, LLC.
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

/**
 * @file work_pool.c
 * @author William Allen Simpson <bill@cohortfs.com>
 * @brief Pthreads-based work queue package
 *
 * @section DESCRIPTION
 *
 * This provides simple work queues using pthreads and TAILQ primitives.
 *
 * @note    Loosely based upon previous thrdpool by
 *          Matt Benjamin <matt@cohortfs.com>
 */

#include <config.h>

#include <sys/types.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#include <err.h>
#endif

#include <rpc/types.h>
#include "rpc_com.h"
#include <sys/types.h>
#include <misc/abstract_atomic.h>
#include <misc/portable.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <intrinsic.h>

#include <rpc/work_pool.h>

#define WORK_POOL_STACK_SIZE MAX(64 * 1024, PTHREAD_STACK_MIN)
#define WORK_POOL_TIMEOUT_MS (31 /* seconds (prime) */ * 1000)

/* forward declaration in lieu of moving code, was inline */

static int work_pool_spawn(struct work_pool *pool);

int
work_pool_init(struct work_pool *pool, const char *name,
		struct work_pool_params *params)
{
	int rc;

	memset(pool, 0, sizeof(*pool));
	poolq_head_setup(&pool->pqh);

	pool->name = mem_strdup(name);
	pool->params = *params;

	if (pool->params.thrd_max < 1) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() thrd_max (%d) < 1",
			__func__, pool->params.thrd_max);
		pool->params.thrd_max = 1;
	};

	if (pool->params.thrd_min < 1) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() thrd_min (%d) < 1",
			__func__, pool->params.thrd_min);
		pool->params.thrd_min = 1;
	};

	rc = pthread_attr_init(&pool->attr);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't init pthread's attributes: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setscope(&pool->attr, PTHREAD_SCOPE_SYSTEM);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's scope: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setdetachstate(&pool->attr, PTHREAD_CREATE_DETACHED);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's join state: %s (%d)",
			__func__, strerror(rc), rc);
		return rc;
	}

	rc = pthread_attr_setstacksize(&pool->attr, WORK_POOL_STACK_SIZE);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() can't set pthread's stack size: %s (%d)",
			__func__, strerror(rc), rc);
	}

	/* initial spawn will spawn more threads as needed */
	return work_pool_spawn(pool);
}

static inline int
work_pool_wait(struct work_pool *pool, struct work_pool_thread *wpt)
{
	struct timespec ts;
	int rc;

	clock_gettime(CLOCK_REALTIME_FAST, &ts);
	timespec_addms(&ts, WORK_POOL_TIMEOUT_MS);

	/* Note: the mutex is the pool _head,
	 * but the condition is per worker,
	 * making the signal efficient!
	 */
	rc = pthread_cond_timedwait(&wpt->pqcond, &pool->pqh.qmutex, &ts);
	if (rc) {
		if (!wpt->work) {
			/* Allow for possible timing race: work entry can be
			 * set by another thread with the timeout result?
			 * Then, has already been removed there.
			 * Only remove with no work here.
			 */
			TAILQ_REMOVE(&pool->pqh.qh, &wpt->pqe, q);
			++(pool->pqh.qcount);
		}
		if (rc != ETIMEDOUT) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() cond_timedwait failed (%d)\n",
				__func__, rc);
			return (rc);
		}
	}

	return (0);
}

/**
 * @brief The worker thread
 *
 * This is the body of the worker thread. The argument is a pointer to
 * its working context, kept in a list for each pool.
 *
 * @param[in] arg 	thread context
 */

static void *
work_pool_thread(void *arg)
{
	struct work_pool_thread *wpt = arg;
	struct work_pool *pool = wpt->pool;
	struct poolq_entry *have;

	atomic_inc_uint32_t(&pool->n_threads);
	pthread_cond_init(&wpt->pqcond, NULL);

	do {
		/* testing at top of loop allows pre-specification of work,
		 * and thread termination after timeout with no work (below).
		 */
		if (wpt->work) {
			if (pool->pqh.qcount > -pool->params.thrd_min
			 && pool->n_threads < pool->params.thrd_max) {
				/* busy, so dynamically add another thread */
				(void)work_pool_spawn(pool);
			}
	
			__warnx(TIRPC_DEBUG_FLAG_EVENT,
				"%s() %s task %p",
				__func__, pool->name, wpt->work);
			wpt->work->wpt = wpt;
			wpt->work->fun(wpt->work);
			wpt->work->wpt = NULL;
			wpt->work = NULL;
		}

		pthread_mutex_lock(&pool->pqh.qmutex);

		if (0 < pool->pqh.qcount--) {
			/* positive for task(s) */
			have = TAILQ_FIRST(&pool->pqh.qh);
			TAILQ_REMOVE(&pool->pqh.qh, have, q);

			wpt->work = (struct work_pool_entry *)have;
		} else {
			/* negative for waiting worker(s):
			 * use the otherwise empty pool to hold them,
			 * simplifying mutex and pointer setup.
			 */
			TAILQ_INSERT_TAIL(&pool->pqh.qh, &wpt->pqe, q);

			__warnx(TIRPC_DEBUG_FLAG_EVENT,
				"%s() %s waiting for task",
				__func__, pool->name);

			if (unlikely(work_pool_wait(pool, wpt))) {
				/* failed, not timeout */
				pthread_mutex_unlock(&pool->pqh.qmutex);
				break;
			}
		}

		pthread_mutex_unlock(&pool->pqh.qmutex);
	} while (wpt->work || pool->n_threads <= pool->params.thrd_min);

	/* cleanup thread context */
	atomic_dec_uint32_t(&pool->n_threads);
	cond_destroy(&wpt->pqcond);
	mem_free(wpt, sizeof(*wpt));

	return (NULL);
}

static inline void
work_pool_dispatch(struct work_pool *pool, struct work_pool_entry *work)
{
	struct work_pool_thread *wpt = (struct work_pool_thread *)
		TAILQ_FIRST(&pool->pqh.qh);

	TAILQ_REMOVE(&pool->pqh.qh, &wpt->pqe, q);
	wpt->work = work;

	/* Note: the mutex is the pool _head,
	 * but the condition is per worker,
	 * making the signal efficient!
	 */
	pthread_cond_signal(&wpt->pqcond);
}

static int
work_pool_spawn(struct work_pool *pool)
{
	int rc;
	struct work_pool_thread *wpt = mem_zalloc(sizeof(*wpt));

	wpt->pool = pool;

	rc = pthread_create(&wpt->pt, &pool->attr, work_pool_thread, wpt);
	if (rc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() pthread_create failed (%d)\n",
			__func__, rc);
		return rc;
	}

	return (0);
}

int
work_pool_submit(struct work_pool *pool, struct work_pool_entry *work)
{
	int rc = 0;

	if (unlikely(!pool->params.thrd_max)) {
		/* queue is draining */
		return (0);
	}
	pthread_mutex_lock(&pool->pqh.qmutex);

	if (likely(0 > pool->pqh.qcount++)) {
		/* negative for waiting worker(s) */
		work_pool_dispatch(pool, work);
	} else {
		/* positive for task(s) */
		TAILQ_INSERT_TAIL(&pool->pqh.qh, &work->pqe, q);
	}

	pthread_mutex_unlock(&pool->pqh.qmutex);
	return rc;
}

int
work_pool_shutdown(struct work_pool *pool)
{
	struct timespec ts = {
		.tv_sec = 5,
		.tv_nsec = 0,
	};

	pool->params.thrd_max =
	pool->params.thrd_min = 0;

	pthread_mutex_lock(&pool->pqh.qmutex);

	while (0 > pool->pqh.qcount) {
		/* unlike _submit, only increment negatives */
		pool->pqh.qcount++;
		work_pool_dispatch(pool, NULL);
	}

	pthread_mutex_unlock(&pool->pqh.qmutex);

	while (pool->n_threads > 0) {
		nanosleep(&ts, NULL);
	}

	mem_free(pool->name, 0);
	poolq_head_destroy(&pool->pqh);

	return (0);
}
