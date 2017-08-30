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
 * @file pool_queue.h
 * @author William Allen Simpson <bill@cohortfs.com>
 * @brief Pthreads-based TAILQ package
 *
 * @section DESCRIPTION
 *
 * This provides simple queues using pthreads and TAILQ primitives.
 *
 * @note    Loosely based upon previous wait_queue by
 *          Matt Benjamin <matt@cohortfs.com>
 */

#ifndef POOL_QUEUE_H
#define POOL_QUEUE_H

#include <pthread.h>
#include <sys/types.h>
#include <misc/queue.h>

struct poolq_entry {
	TAILQ_ENTRY(poolq_entry) q;	/*** 1st ***/
	u_int qsize;			/* allocated size of q entry,
					 * 0: default size */
	uint16_t qflags;
};

struct poolq_head {
	TAILQ_HEAD(poolq_head_s, poolq_entry) qh;
	pthread_mutex_t qmutex;

	u_int qsize;			/* default size of q entries,
					 * 0: static size */
	int qcount;			/* number of entries,
					 * < 0: has waiting workers. */
};

static inline void
poolq_head_destroy(struct poolq_head *qh)
{
	pthread_mutex_destroy(&qh->qmutex);
}

static inline void
poolq_head_setup(struct poolq_head *qh)
{
	TAILQ_INIT(&qh->qh);
	pthread_mutex_init(&qh->qmutex, NULL);
	qh->qcount = 0;
}

#endif				/* POOL_QUEUE_H */
