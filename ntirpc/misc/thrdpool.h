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

#ifndef THRDPOOL_H
#define THRDPOOL_H

#include <misc/queue.h>
#include <misc/wait_queue.h>

#define THRD_FLAG_NONE        0x0000
#define THRD_FLAG_SHUTDOWN    0x0001
#define THRD_FLAG_ACTIVE      0x0002

struct work {
	TAILQ_ENTRY(work) tailq;
	void (*func) (void*);
	void *arg;
};

struct thrdpool;

struct thrd {
	TAILQ_ENTRY(thrd) tailq;
	struct thrd_context {
		pthread_t id;
		struct wait_entry we;
		struct work *work;
	} ctx;
	struct thrdpool *pool;
	bool idle;
};

struct thrdpool_params {
	int32_t thrd_max;
	int32_t thrd_min;
};

struct thrdpool {
	char *name;
	uint32_t flags;
	struct thrdpool_params params;
	pthread_attr_t attr;
	struct wait_entry we;
	 TAILQ_HEAD(idle_tailq, thrd) idle_q;
	 TAILQ_HEAD(work_tailq, work) work_q;
	int32_t n_idle;
	int32_t n_threads;
};

typedef void (*thrd_func_t) (void *);

int thrdpool_init(struct thrdpool *, const char *, struct thrdpool_params *);
int thrdpool_submit_work(struct thrdpool *, thrd_func_t, void *);
int thrdpool_shutdown(struct thrdpool *);

#endif				/* THRDPOOL_H */
