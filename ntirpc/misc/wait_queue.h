/*
 * Copyright (c) 2013 Linux Box Corporation.
 * Copyright (c) 2013-2017 Red Hat, Inc. and/or its affiliates.
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

struct waitq_entry {
	mutex_t mtx;
	cond_t cv;
};

#define Wqe_LFlag_None        0x0000
#define Wqe_LFlag_WaitSync    0x0001
#define Wqe_LFlag_SyncDone    0x0002

/* thread wait queue */
struct waitq_head {
	TAILQ_HEAD(waitq_head_s, waiter) waitq;
	struct waitq_entry lwe;	/* left */
	struct waitq_entry rwe;	/* right */
	uint32_t flags;
	uint32_t waiters;
};

static inline void waitq_entry_init(struct waitq_entry *we)
{
	mutex_init(&we->mtx, NULL);
	pthread_cond_init(&we->cv, NULL);
}

static inline void waitq_entry_destroy(struct waitq_entry *we)
{
	mutex_destroy(&we->mtx);
	cond_destroy(&we->cv);
}

static inline void waitq_head_init(struct waitq_head *wqe)
{
	TAILQ_INIT(&wqe->waitq);
	waitq_entry_init(&wqe->lwe);
	waitq_entry_init(&wqe->rwe);
}

static inline void waitq_head_destroy(struct waitq_head *wqe)
{
	TAILQ_INIT(&wqe->waitq);
	waitq_entry_destroy(&wqe->lwe);
	waitq_entry_destroy(&wqe->rwe);
}

#endif				/* WAIT_QUEUE_H */
