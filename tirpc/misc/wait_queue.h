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

#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <misc/queue.h>
#include <reentrant.h>

typedef struct wait_entry {
	mutex_t mtx;
	cond_t cv;
} wait_entry_t;

#define Wqe_LFlag_None        0x0000
#define Wqe_LFlag_WaitSync    0x0001
#define Wqe_LFlag_SyncDone    0x0002

/* thread wait queue */
typedef struct wait_q_entry {
	uint32_t flags;
	uint32_t waiters;
	wait_entry_t lwe;	/* left */
	wait_entry_t rwe;	/* right */
	 TAILQ_HEAD(we_tailq, waiter) waitq;
} wait_q_entry_t;

static inline void init_wait_entry(wait_entry_t *we)
{
	mutex_init(&we->mtx, NULL);
	pthread_cond_init(&we->cv, NULL);
}

static inline void destroy_wait_entry(wait_entry_t *we)
{
	mutex_destroy(&we->mtx);
	cond_destroy(&we->cv);
}

static inline void init_wait_q_entry(wait_q_entry_t *wqe)
{
	TAILQ_INIT(&wqe->waitq);
	init_wait_entry(&wqe->lwe);
	init_wait_entry(&wqe->rwe);
}

static inline void thread_delay_ms(unsigned long ms)
{
	struct timespec then = {
		.tv_sec = ms / 1000,
		.tv_nsec = ms % 1000000UL
	};
	nanosleep(&then, NULL);
}

#endif				/* WAIT_QUEUE_H */
