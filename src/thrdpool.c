/*
 * Copyright (c) 2013 Linux Box Corporation.
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
#if !defined(_WIN32)
#include <netinet/in.h>
#include <err.h>
#endif

#include <rpc/types.h>
#include "rpc_com.h"
#include <sys/types.h>
#include <reentrant.h>
#include <misc/portable.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <intrinsic.h>
#include <misc/thrdpool.h>

int
thrdpool_init(struct thrdpool *pool, const char *name,
		  struct thrdpool_params *params)
{
	memset(pool, 0, sizeof(struct thrdpool));
	init_wait_entry(&pool->we);

	mutex_lock(&pool->we.mtx);
	pool->name = mem_strdup(name);
	pool->params = *params;
	TAILQ_INIT(&pool->idle_q);
	TAILQ_INIT(&pool->work_q);

	(void)pthread_attr_init(&pool->attr);
	(void)pthread_attr_setscope(&pool->attr, PTHREAD_SCOPE_SYSTEM);
	(void)pthread_attr_setdetachstate(&pool->attr, PTHREAD_CREATE_DETACHED);

	mutex_unlock(&pool->we.mtx);

	return 0;
}

static bool
thrd_wait(struct thrd *thrd)
{
	bool code = false;
	struct thrdpool *pool = thrd->pool;
	struct timespec ts;
	struct work *work;
	int rc;

	mutex_lock(&pool->we.mtx);
	if (pool->flags & THRD_FLAG_SHUTDOWN) {
		mutex_unlock(&pool->we.mtx);
		goto out;
	}
	TAILQ_FOREACH(work, &pool->work_q, tailq) {
		TAILQ_REMOVE(&pool->work_q, work, tailq);
		thrd->ctx.work = work;
		code = true;
		mutex_unlock(&pool->we.mtx);
		goto out;
	}

	TAILQ_INSERT_TAIL(&pool->idle_q, thrd, tailq);
	++(pool->n_idle);

	mutex_lock(&thrd->ctx.we.mtx);
	thrd->idle = true;
	mutex_unlock(&pool->we.mtx);

	while (1) {
		clock_gettime(CLOCK_REALTIME_FAST, &ts);
		timespec_addms(&ts, 1000 * 120);
		if (pool->flags & THRD_FLAG_SHUTDOWN) {
			rc = ETIMEDOUT;
		} else {
			rc = cond_timedwait(&thrd->ctx.we.cv, &thrd->ctx.we.mtx, &ts);
		}
		if (rc == ETIMEDOUT) {
			mutex_unlock(&thrd->ctx.we.mtx);
			mutex_lock(&pool->we.mtx);
			mutex_lock(&thrd->ctx.we.mtx);
			if (!thrd->idle) {
				/* raced */
				code = true;
				mutex_unlock(&thrd->ctx.we.mtx);
				mutex_unlock(&pool->we.mtx);
				goto out;
			}
			code = false;
			TAILQ_REMOVE(&pool->idle_q, thrd, tailq);
			--(pool->n_idle);
			thrd->idle = false;
			mutex_unlock(&thrd->ctx.we.mtx);
			mutex_unlock(&pool->we.mtx);
			goto out;
		}
		/* signalled */
		code = !thrd->idle;
		mutex_unlock(&thrd->ctx.we.mtx);
		break;
	}

 out:
	return (code);
}

static void *thrdpool_start_routine(void *arg)
{
	struct thrd *thrd = arg;
	struct thrdpool *pool = thrd->pool;
	bool reschedule;

	do {
		struct work *work = thrd->ctx.work;
		work->func(work->arg);
		thrd->ctx.work = 0;
		mem_free(work, sizeof(struct work));
		reschedule = thrd_wait(thrd);
	} while (reschedule);

	/* cleanup thread context */
	destroy_wait_entry(&thrd->ctx.we);
	--(thrd->pool->n_threads);
	mutex_lock(&pool->we.mtx);
	cond_signal(&pool->we.cv);
	mutex_unlock(&pool->we.mtx);
	mem_free(thrd, 0);

	return (NULL);
}

static inline bool thrdpool_dispatch(struct thrdpool *pool, struct work *work)
{
	struct thrd *thrd;
	TAILQ_FOREACH(thrd, &pool->idle_q, tailq) {
		mutex_lock(&thrd->ctx.we.mtx);
		TAILQ_REMOVE(&pool->idle_q, thrd, tailq);
		--(pool->n_idle);
		thrd->idle = false;
		thrd->ctx.work = work;
		cond_signal(&thrd->ctx.we.cv);
		mutex_unlock(&thrd->ctx.we.mtx);
		break;
	}
	return (true);
}

static inline bool thrdpool_spawn(struct thrdpool *pool, struct work *work)
{
	int code;
	struct thrd *thrd = mem_alloc(sizeof(struct thrd));
	memset(thrd, 0, sizeof(struct thrd));
	init_wait_entry(&thrd->ctx.we);
	thrd->pool = pool;
	thrd->ctx.work = work;
	++(pool->n_threads);

	code =
	    pthread_create(&thrd->ctx.id, &pool->attr, thrdpool_start_routine,
			   thrd);
	if (code != 0) {
		__warnx(TIRPC_DEBUG_FLAG_SVC_VC, "pthread_create failed %d\n",
			__func__, errno);
	}

	return (true);
}

int thrdpool_submit_work(struct thrdpool *pool, thrd_func_t func, void *arg)
{
	int code = 0;
	struct work *work;

	/* queue is draining */
	mutex_lock(&pool->we.mtx);
	if (unlikely(pool->flags & THRD_FLAG_SHUTDOWN))
		goto unlock;

	work = mem_zalloc(sizeof(struct work));
	if (unlikely(!work)) {
		code = -1;
		goto unlock;
	}
	work->func = func;
	work->arg = arg;
	/* idle thread(s) available */
	if (pool->n_idle > 0) {
		if (thrdpool_dispatch(pool, work))
			goto unlock;
	}

	/* need a thread */
	if ((pool->params.thrd_max == 0)
	    || (pool->n_threads < pool->params.thrd_max)) {
		code = thrdpool_spawn(pool, work);
		goto unlock;
	}
	TAILQ_INSERT_TAIL(&pool->work_q, work, tailq);

 unlock:
	mutex_unlock(&pool->we.mtx);

	return (code);
}

int thrdpool_shutdown(struct thrdpool *pool)
{
	struct timespec ts;
	int wait = 1;
	struct thrd *thrd;
	struct work *work;

	mutex_lock(&pool->we.mtx);
	pool->flags |= THRD_FLAG_SHUTDOWN;
	TAILQ_FOREACH(thrd, &pool->idle_q, tailq) {
		cond_signal(&thrd->ctx.we.cv);
	}
	TAILQ_FOREACH(work, &pool->work_q, tailq) {
		work->func(work->arg);
	}
	while (pool->n_threads > 0) {
		clock_gettime(CLOCK_REALTIME_FAST, &ts);
		timespec_addms(&ts, 1000 * wait);
		(void)cond_timedwait(&pool->we.cv, &pool->we.mtx, &ts);
		/* wait a bit longer */
		wait = 5;
	}

	mem_free(pool->name, 0);
	mutex_unlock(&pool->we.mtx);
	destroy_wait_entry(&pool->we);

	return (0);
}
