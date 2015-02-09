/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet@cea.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <limits.h>	//INT_MAX
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <sys/socket.h> //sockaddr
#include <sys/un.h>     //sockaddr_un
#include <pthread.h>	//pthread_* (think it's included by another one)
#include <semaphore.h>  //sem_* (is it a good idea to mix sem and pthread_cond/mutex?)
#include <arpa/inet.h>  //inet_ntop
#include <netinet/in.h> //sock_addr_in
#include <unistd.h>	//fcntl
#include <fcntl.h>	//fcntl
#include <sys/epoll.h>
#include <sys/eventfd.h>

#define EPOLL_MAX_EVENTS 16
#define NUM_WQ_PER_POLL 16

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "utils.h"
#include "mooshika.h"

#ifdef HAVE_VALGRIND_MEMCHECK_H
#  include <valgrind/memcheck.h>
#  ifndef VALGRIND_MAKE_MEM_DEFINED
#    warning "Valgrind support requested, but VALGRIND_MAKE_MEM_DEFINED not available"
#  endif
#endif
#ifndef VALGRIND_MAKE_MEM_DEFINED
#  define VALGRIND_MAKE_MEM_DEFINED(addr, len)
#endif

/**
 * \struct msk_ctx
 * Context data we can use during recv/send callbacks
 */
struct msk_ctx {
	enum msk_ctx_used {
		MSK_CTX_FREE = 0,
		MSK_CTX_PENDING,
		MSK_CTX_PROCESSING
	} used;				/**< 0 if we can use it for a new recv/send */
	msk_data_t *data;
	ctx_callback_t callback;
	ctx_callback_t err_callback;
	void *callback_arg;
	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;
	struct ibv_sge sg_list[0]; 		/**< this is actually an array. note that when you malloc you have to add its size */
};

struct msk_worker_data {
	struct msk_trans *trans;
	struct msk_ctx *ctx;
	enum ibv_wc_status status;
	enum ibv_wc_opcode opcode;
};

struct worker_pool {
	pthread_t *thrids;
	struct msk_worker_data *wd_queue;
	int worker_count;
	int size;
	int w_head;
	int w_count;
	int w_efd;
	int m_tail;
	int m_count;
	int m_efd;
};

struct msk_global_state {
	pthread_mutex_t lock;
	int debug;
	pthread_t cm_thread;		/**< Thread id for connection manager */
	pthread_t cq_thread;		/**< Thread id for completion queue handler */
	pthread_t stats_thread;
	unsigned int run_threads;
	int cm_epollfd;
	int cq_epollfd;
	int stats_epollfd;
	struct worker_pool worker_pool;
};

/* GLOBAL VARIABLES */

static struct msk_global_state *msk_global_state = NULL;

void __attribute__ ((constructor)) msk_internals_init(void) {
	msk_global_state = malloc(sizeof(*msk_global_state));
	if (!msk_global_state) {
		ERROR_LOG("Out of memory");
	}

	memset(msk_global_state, 0, sizeof(*msk_global_state));

	msk_global_state->run_threads = 0;
	pthread_mutex_init(&msk_global_state->lock, NULL);
}

void __attribute__ ((destructor)) msk_internals_fini(void) {

	if (msk_global_state) {
		msk_global_state->run_threads = 0;

		if (msk_global_state->cm_thread) {
			pthread_join(msk_global_state->cm_thread, NULL);
			msk_global_state->cm_thread = 0;
		}
		if (msk_global_state->cq_thread) {
			pthread_join(msk_global_state->cq_thread, NULL);
			msk_global_state->cq_thread = 0;
		}
		if (msk_global_state->stats_thread) {
			pthread_join(msk_global_state->stats_thread, NULL);
			msk_global_state->stats_thread = 0;
		}

		pthread_mutex_destroy(&msk_global_state->lock);
		free(msk_global_state);
		msk_global_state = NULL;
	}
}

/* forward declarations */

static void *msk_cq_thread(void *arg);


/* UTILITY FUNCTIONS */


static inline int msk_mutex_lock(int debug, pthread_mutex_t *mutex) {
	INFO_LOG(debug, "locking   %p", mutex);
	return pthread_mutex_lock(mutex);
}
static inline int msk_mutex_unlock(int debug, pthread_mutex_t *mutex) {
	INFO_LOG(debug, "unlocking %p", mutex);
	return pthread_mutex_unlock(mutex);
}
static inline int msk_cond_wait(int debug,
				pthread_cond_t *cond,
				pthread_mutex_t *mutex) {
	int rc;
	INFO_LOG(debug, "unlocking %p", mutex);
	rc = pthread_cond_wait(cond, mutex);
	INFO_LOG(debug, "locked    %p", mutex);
	return rc;
}
static inline int msk_cond_timedwait(int debug,
				     pthread_cond_t *cond,
				     pthread_mutex_t *mutex,
				     const struct timespec *abstime) {
	int rc;
	INFO_LOG(debug, "unlocking %p", mutex);
	rc = pthread_cond_timedwait(cond, mutex, abstime);
	INFO_LOG(debug, "locked    %p", mutex);
	return rc;
}


/**
 * msk_getpd: helper function to get the right pd for a given trans
 *
 * @param trans [IN] the connection handle
 *
 * @return NULL if nothing is available, next free pd if none fit, correct one if any match
 */
struct msk_pd *msk_getpd(struct msk_trans *trans) {
	int i = 0;

	if (!trans->pd)
		return NULL;

	while (trans->pd[i].context != PD_GUARD) {
		if (trans->pd[i].context == trans->cm_id->verbs) {
			/* Got a match, woo. */
			return &trans->pd[i];
		}

		if (!trans->pd[i].context) {
			/* Not found, but we have room. */
			if (atomic_postinc(trans->pd[i].used) != 0
			    || trans->pd[i].context != NULL) {
				/* We got raced, try again */
				atomic_dec(trans->pd[i].used);
				continue;
			} else {
				trans->pd[i].context = trans->cm_id->verbs;
				return &trans->pd[i];
			}
		}
		i++;
	}

	/* No space left, too bad. */
	return NULL;
}


/* Prepare for first child to allocate pd
 * Cases:
 *  - pd already provided in attr,
 *  - context already set (bound to one uverb)
 *  - need to prepare for all of them
 */
static int msk_setup_pd(struct msk_trans *trans) {
	int ret;
	struct ibv_device **devlist;

	if (!trans->pd) {
		if (trans->cm_id->verbs) {
			ret = 1;
		} else {
			devlist = ibv_get_device_list(&ret);
			if (!devlist) {
				ret = errno;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_get_device_list: %s (%d)", strerror(ret), ret);
				return ret;
			}
			/* We only care about the number of devices */
			ibv_free_device_list(devlist);
		}

		++ret; /* for guard */
		trans->pd = malloc(ret * sizeof(*trans->pd));
		if (!trans->pd) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc msk pd (%u elems)", ret);
			ret = ENOMEM;
			return ret;
		}
		memset(trans->pd, 0, ret * sizeof(*trans->pd));
		trans->pd[0].refcnt = 1; /* only use refcnt of the first one */
		trans->pd[ret-1].context = PD_GUARD;
	}

	return 0;
}

/**
 * msk_reg_mr: registers memory for rdma use (almost the same as ibv_reg_mr)
 *
 * @param trans   [IN]
 * @param memaddr [IN] the address to register
 * @param size    [IN] the size of the area to register
 * @param access  [IN] the access to grants to the mr (e.g. IBV_ACCESS_LOCAL_WRITE)
 *
 * @return a pointer to the mr if registered correctly or NULL on failure
 */

struct ibv_mr *msk_reg_mr(struct msk_trans *trans, void *memaddr, size_t size, int access) {
	struct msk_pd *pd = msk_getpd(trans);
	if (!pd)
		return NULL;
	if (!pd->pd) {
		pd->used = 0;
		return NULL;
	}
	return ibv_reg_mr(pd->pd, memaddr, size, access);
}

/**
 * msk_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int msk_dereg_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

/**
 * msk_make_rloc: makes a rkey to send it for remote host use
 *
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
	msk_rloc_t *rloc;
	rloc = malloc(sizeof(msk_rloc_t));
	if (!rloc) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Out of memory!");
		return NULL;
	}

	rloc->raddr = addr;
	rloc->rkey = mr->rkey;
	rloc->size = size;

	return rloc;
}

void msk_print_devinfo(struct msk_trans *trans) {
	struct ibv_device_attr device_attr;
	ibv_query_device(trans->cm_id->verbs, &device_attr);
	uint64_t node_guid = ntohll(device_attr.node_guid);
	printf("guid: %04x:%04x:%04x:%04x\n",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
}


static inline struct msk_ctx *msk_next_ctx(struct msk_ctx *ctx, int n_sge) {
	return (struct msk_ctx*)((uint8_t*)ctx + sizeof(struct msk_ctx) + n_sge*sizeof(struct ibv_sge));
}

/**
 * msk_create_thread: Simple wrapper around pthread_create
 */
#define THREAD_STACK_SIZE 2116488
static inline int msk_create_thread(pthread_t *thrid, void *(*start_routine)(void*), void *arg) {

	pthread_attr_t attr;
	int ret;

	/* Init for thread parameter (mostly for scheduling) */
	if ((ret = pthread_attr_init(&attr)) != 0) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "can't init pthread's attributes: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) != 0) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "can't set pthread's scope: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) != 0) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "can't set pthread's join state: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE)) != 0) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "can't set pthread's stack size: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return pthread_create(thrid, &attr, start_routine, arg);
}

static inline int msk_check_create_epoll_thread(pthread_t *thrid, void *(*start_routine)(void*), void *arg, int *epollfd) {
	int ret;

	pthread_mutex_lock(&msk_global_state->lock);
	if (*thrid == 0) do {
		*epollfd = epoll_create(10);
		if (*epollfd == -1) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "epoll_create failed: %s (%d)", strerror(ret), ret);
			break;
		}

		if ((ret = msk_create_thread(thrid, start_routine, arg))) {
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Could not create thread: %s (%d)", strerror(ret), ret);
			*thrid = 0;
			break;
		}
	} while (0);

	pthread_mutex_unlock(&msk_global_state->lock);
	return 0;
}


static inline void msk_worker_callback(struct msk_trans *trans, struct msk_ctx *ctx, enum ibv_wc_status status, enum ibv_wc_opcode opcode) {
	struct timespec ts_start, ts_end;

	if (status) {
		if (ctx->err_callback) {
			if (trans->debug & MSK_DEBUG_SPEED)
				clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ctx->err_callback(trans, ctx->data, ctx->callback_arg);
			if (trans->debug & MSK_DEBUG_SPEED) {
				clock_gettime(CLOCK_MONOTONIC, &ts_end);
				sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
			}
		}
	} else switch (opcode) {
		case IBV_WC_SEND:
		case IBV_WC_RDMA_WRITE:
		case IBV_WC_RDMA_READ:
			if (ctx->callback) {
				if (trans->debug & MSK_DEBUG_SPEED)
					clock_gettime(CLOCK_MONOTONIC, &ts_start);
				ctx->callback(trans, ctx->data, ctx->callback_arg);
				if (trans->debug & MSK_DEBUG_SPEED) {
					clock_gettime(CLOCK_MONOTONIC, &ts_end);
					sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
				}
			}
			break;

		case IBV_WC_RECV:
		case IBV_WC_RECV_RDMA_WITH_IMM:
			if (ctx->callback) {
				if (trans->debug & MSK_DEBUG_SPEED)
					clock_gettime(CLOCK_MONOTONIC, &ts_start);
				ctx->callback(trans, ctx->data, ctx->callback_arg);
				if (trans->debug & MSK_DEBUG_SPEED) {
					clock_gettime(CLOCK_MONOTONIC, &ts_end);
					sub_timespec(&trans->stats.nsec_callback, &ts_start, &ts_end);
				}
			}
			break;

		default:
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "worker thread got weird opcode: %d", opcode);
	}

	atomic_store(&ctx->used, MSK_CTX_FREE);

	return;
}

static void* msk_worker_thread(void *arg) {
	struct worker_pool *pool = arg;
	struct msk_worker_data wd;
	uint64_t n;
	int i;

	while (msk_global_state->run_threads > 0) {

		if (pool->w_count > 0) {
			i = atomic_dec(pool->w_count);
			if (i < 0) {
				atomic_inc(pool->w_count);
				continue;
			}
			i = atomic_postinc(pool->w_head);
			if (i >= pool->size) {
				i = i & (pool->size-1);
				atomic_mask(pool->w_head, pool->size-1);
			}
		} else {
			if (eventfd_read(pool->w_efd, &n) || n > INT_MAX) {
				INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "eventfd_read failed");
				continue;
			}
			if (msk_global_state->run_threads == 0)
				break;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_WORKERS, "worker: %d", (int)n);
			atomic_add(pool->w_count, (int)n);
			continue;
		}

		INFO_LOG(msk_global_state->debug & MSK_DEBUG_WORKERS, "thread %lx, depopping wd index %i, count %i, trans %p, ctx %p, used %i", pthread_self(), pool->w_head, pool->w_count, pool->wd_queue[i].trans, pool->wd_queue[i].ctx, pool->wd_queue[i].ctx->used);

		memcpy(&wd, &pool->wd_queue[i], sizeof(struct msk_worker_data));

		if (eventfd_write(pool->m_efd, 1))
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "eventfd_write failed");

		msk_worker_callback(wd.trans, wd.ctx, wd.status, wd.opcode);
	}

	pthread_exit(NULL);
}

static int msk_spawn_worker_threads() {
	int i, ret = 0;
	/* alloc and stuff */

	pthread_mutex_lock(&msk_global_state->lock);
	do {
		if (msk_global_state->worker_pool.thrids != NULL || msk_global_state->worker_pool.worker_count == -1) {
			break;
		}

		msk_global_state->worker_pool.thrids = malloc(msk_global_state->worker_pool.worker_count*sizeof(pthread_t));
		if (msk_global_state->worker_pool.thrids == NULL) {
			ret = ENOMEM;
			break;
		}
		msk_global_state->worker_pool.wd_queue = malloc(msk_global_state->worker_pool.size*sizeof(struct msk_worker_data));
		if (msk_global_state->worker_pool.wd_queue == NULL) {
			ret = ENOMEM;
			break;
		}

		msk_global_state->worker_pool.w_head = 0;
		msk_global_state->worker_pool.w_count = 0;
		msk_global_state->worker_pool.m_tail = 0;
		msk_global_state->worker_pool.m_count = 0;
		msk_global_state->worker_pool.w_efd = eventfd(0, 0);
		msk_global_state->worker_pool.m_efd = eventfd(0, 0);

		for (i=0; i < msk_global_state->worker_pool.worker_count; i++) {
			ret = msk_create_thread(&msk_global_state->worker_pool.thrids[i], msk_worker_thread, &msk_global_state->worker_pool);
			if (ret)
				break;
		}
	} while (0);
	if (ret) {
		// join stuff?
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Could not create workers: %s (%d)", strerror(ret), ret);
		if (msk_global_state->worker_pool.wd_queue) {
			free(msk_global_state->worker_pool.wd_queue);
			msk_global_state->worker_pool.wd_queue = NULL;
		}
		if (msk_global_state->worker_pool.thrids) {
			free(msk_global_state->worker_pool.thrids);
			msk_global_state->worker_pool.thrids = NULL;
		}
	}
	pthread_mutex_unlock(&msk_global_state->lock);
	return ret;
}

/** msk_kill_worker_threads: stops and joins worker threads, assume that we hold msk_global_state->lock and msk_global_state->run_thread == 0;
 */
static int msk_kill_worker_threads() {
	int i;

	if (msk_global_state->worker_pool.thrids == NULL || msk_global_state->worker_pool.worker_count == -1) {
		return 0;
	}

	/* wake up all threads - this value guarantees that we wait till a thread woke up before sending it again... */
	/* FIXME make sure none is awake before sending that :) */
	for (i=0; i < msk_global_state->worker_pool.worker_count; i++)
		eventfd_write(msk_global_state->worker_pool.w_efd, 0xfffffffffffffffe);

	for (i=0; i < msk_global_state->worker_pool.worker_count; i++) {
		pthread_join(msk_global_state->worker_pool.thrids[i], NULL);
	}

	close(msk_global_state->worker_pool.w_efd);
	close(msk_global_state->worker_pool.m_efd);

	free(msk_global_state->worker_pool.thrids);
	msk_global_state->worker_pool.thrids = NULL;
	free(msk_global_state->worker_pool.wd_queue);
	msk_global_state->worker_pool.wd_queue = NULL;


	return 0;
}

/* called under trans cm lock */
static int msk_signal_worker(struct msk_trans *trans, struct msk_ctx *ctx, enum ibv_wc_status status, enum ibv_wc_opcode opcode) {
	struct msk_worker_data *wd;
	int i;

	INFO_LOG(trans->debug & MSK_DEBUG_WORKERS, "signaling trans %p, ctx %p", trans, ctx);

	// Don't signal and do it directly if no worker
	if (msk_global_state->worker_pool.worker_count == -1) {
		msk_worker_callback(trans, ctx, status, opcode);
		return 0;
	}

	/*
	 * Only need this done in async mode because we don't leave trans cm lock otherwise.
	 * Likewise, doesn't need an atomic_bool_compare_and_swap because it only matters where this lock is held
	 * e.g. if (!atomic_bool_compare_and_swap(&ctx->used, MSK_CTX_PENDING, MSK_CTX_PROCESSING))
	 */
	if (ctx->used != MSK_CTX_PENDING) {
		// nothing to do
		return 0;
	}
	ctx->used = MSK_CTX_PROCESSING;

	while (atomic_inc(msk_global_state->worker_pool.m_count) > msk_global_state->worker_pool.size
	    && msk_global_state->run_threads > 0) {
		uint64_t n;
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

		if (eventfd_read(msk_global_state->worker_pool.m_efd, &n) || n > INT_MAX) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "eventfd_read failed");
		} else {
			INFO_LOG(trans->debug & MSK_DEBUG_WORKERS, "master: %d\n", (int)n);
			atomic_sub(msk_global_state->worker_pool.m_count, (int)n+1 /* we're doing inc again */);
		}
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
	}

	do {
		if (msk_global_state->run_threads == 0) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Had something to do but threads stopping?");
			break;
		}

		i = atomic_postinc(msk_global_state->worker_pool.m_tail);
		if (i >= msk_global_state->worker_pool.size) {
			i = i & (msk_global_state->worker_pool.size-1);
			atomic_mask(msk_global_state->worker_pool.w_head, msk_global_state->worker_pool.size-1);
		}

		wd = &msk_global_state->worker_pool.wd_queue[i];
		wd->trans = trans;
		wd->ctx = ctx;
		wd->status = status;
		wd->opcode = opcode;

		if (eventfd_write(msk_global_state->worker_pool.w_efd, 1))
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "eventfd_write failed");

	} while (0);

	return 0;
}


/**
 * msk_cq_addfd: Adds trans' completion queue fd to the epoll wait
 * Returns 0 on success, errno value on error.
 */
static int msk_addfd(struct msk_trans *trans, int fd, int epollfd) {
	int flags, ret;
	struct epoll_event ev;

	//make get_cq_event_nonblocking for poll
	flags = fcntl(fd, F_GETFL);
	ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Failed to make the comp channel nonblock");
		return ret;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = trans;

	ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
	if (ret == -1) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Failed to add fd to epoll: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

static int msk_delfd(int fd, int epollfd) {
	int ret;

	ret = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
	/* Let epoll deal with multiple deletes of the same fd */
	if (ret == -1 && errno != ENOENT) {
		ret = errno;
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Failed to del fd to epoll: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

static inline int msk_cq_addfd(struct msk_trans *trans) {
	return msk_addfd(trans, trans->comp_channel->fd, msk_global_state->cq_epollfd);
}

static inline int msk_cq_delfd(struct msk_trans *trans) {
	return msk_delfd(trans->comp_channel->fd, msk_global_state->cq_epollfd);
}

static inline int msk_cm_addfd(struct msk_trans *trans) {
	return msk_addfd(trans, trans->event_channel->fd, msk_global_state->cm_epollfd);
}

static inline int msk_cm_delfd(struct msk_trans *trans) {
	return msk_delfd(trans->event_channel->fd, msk_global_state->cm_epollfd);
}

static inline int msk_stats_add(struct msk_trans *trans) {
	int rc;
	struct sockaddr_un sockaddr;

	/* no stats if no prefix */
	if (!trans->stats_prefix)
		return 0;

	/* setup trans->stats_sock here */
	if ( (trans->stats_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "socket on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sun_family = AF_UNIX;
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path)-1, "%s%p", trans->stats_prefix, trans);

	unlink(sockaddr.sun_path);

	if (bind(trans->stats_sock, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "bind on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}

	if (listen(trans->stats_sock, 5) == -1) {
		rc = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "listen on stats socket failed, quitting thread: %d (%s)", rc, strerror(rc));
		return rc;
	}


	return msk_addfd(trans, trans->stats_sock, msk_global_state->stats_epollfd);
}

static inline int msk_stats_del(struct msk_trans *trans) {
	/* msk_delfd is called in stats thread when a close event comes in */
	char sun_path[108];
	snprintf(sun_path, sizeof(sun_path)-1, "%s%p", trans->stats_prefix, trans);
	unlink(sun_path);

	return close(trans->stats_sock);
}

/**
 * msk_stats_thread: unix socket thread
 *
 * Well, a thread. arg = trans
 */
void *msk_stats_thread(void *arg) {
	struct msk_trans *trans;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	char stats_str[256];
	int nfds, n, childfd;
	int ret;

	while (msk_global_state->run_threads > 0) {
		nfds = epoll_wait(msk_global_state->stats_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);
		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "epoll_pwait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (struct msk_trans*)epoll_events[n].data.ptr;

			if (!trans) {
				INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				msk_delfd(trans->stats_sock, msk_global_state->stats_epollfd);
				continue;
			}
			if ( (childfd = accept(trans->stats_sock, NULL, NULL)) == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "accept on stats socket failed: %d (%s).", errno, strerror(errno));
					continue;
				}
			}

			ret = snprintf(stats_str, sizeof(stats_str), "stats:\n"
				"	tx_bytes\ttx_pkt\ttx_err\n"
				"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
				"	rx_bytes\trx_pkt\trx_err\n"
				"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
				"	callback time:   %lu.%09lu s\n"
				"	completion time: %lu.%09lu s\n",
				trans->stats.tx_bytes, trans->stats.tx_pkt,
				trans->stats.tx_err, trans->stats.rx_bytes,
				trans->stats.rx_pkt, trans->stats.rx_err,
				trans->stats.nsec_callback / NSEC_IN_SEC, trans->stats.nsec_callback % NSEC_IN_SEC,
				trans->stats.nsec_compevent / NSEC_IN_SEC, trans->stats.nsec_compevent % NSEC_IN_SEC);
			ret = write(childfd, stats_str, ret);
			ret = close(childfd);
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_cq_event_handler: completion queue event handler.
 * marks contexts back out of use and calls the appropriate callbacks for each kind of event
 *
 * @return 0 on success, work completion status if not 0
 */
static int msk_cq_event_handler(struct msk_trans *trans) {
	struct ibv_wc wc[NUM_WQ_PER_POLL];
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	struct msk_ctx *ctx;
	msk_data_t *data;
	int ret, i;
	int npoll = 0;
	uint32_t len;

	ret = ibv_get_cq_event(trans->comp_channel, &ev_cq, &ev_ctx);
	if (ret) {
		ret = errno;
		if (ret != EAGAIN)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_get_cq_event failed: %d", ret);
		return ret;
	}
	if (ev_cq != trans->cq) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Unknown cq %p", ev_cq);
		ibv_ack_cq_events(ev_cq, 1);
		return EINVAL;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_req_notify_cq failed: %d.", ret);
	}

	while (ret == 0 && (npoll = ibv_poll_cq(trans->cq, NUM_WQ_PER_POLL, wc)) > 0) {
		for (i=0; i < npoll; i++) {
			if (trans->bad_recv_wr) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that recv");
			}
			if (trans->bad_send_wr) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that send");
			}
			if (wc[i].status) {
				switch (wc[i].opcode) {
					case IBV_WC_SEND:
					case IBV_WC_RDMA_WRITE:
					case IBV_WC_RDMA_READ:
						trans->stats.tx_err++;
						break;
					case IBV_WC_RECV:
					case IBV_WC_RECV_RDMA_WITH_IMM:
						trans->stats.rx_err++;
						break;
					default:
						break;
				}
				msk_signal_worker(trans, (struct msk_ctx *)wc[i].wr_id, wc[i].status, wc[i].opcode);

				if (trans->state != MSK_CLOSED && trans->state != MSK_CLOSING && trans->state != MSK_ERROR) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cq completion failed status: %s (%d)", ibv_wc_status_str(wc[i].status), wc[i].status);
					ret = wc[i].status;

				}
				continue;
			}

			switch (wc[i].opcode) {
			case IBV_WC_SEND:
			case IBV_WC_RDMA_WRITE:
			case IBV_WC_RDMA_READ:
				INFO_LOG(trans->debug & MSK_DEBUG_SEND, "WC_SEND/RDMA_WRITE/RDMA_READ: %d", wc[i].opcode);
				trans->stats.tx_pkt++;
				trans->stats.tx_bytes += wc[i].byte_len;

				ctx = (struct msk_ctx *)wc[i].wr_id;

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME ctx->data->imm_data = ntohl(wc.imm_data);
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "imm_data: %d", ntohl(wc[i].imm_data));
				}

				msk_signal_worker(trans, ctx, wc[i].status, wc[i].opcode);
				break;

			case IBV_WC_RECV:
			case IBV_WC_RECV_RDMA_WITH_IMM:
				INFO_LOG(trans->debug & MSK_DEBUG_RECV, "WC_RECV");
				trans->stats.rx_pkt++;
				trans->stats.rx_bytes += wc[i].byte_len;

				if (wc[i].wc_flags & IBV_WC_WITH_IMM) {
					//FIXME ctx->data->imm_data = ntohl(wc.imm_data);
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "imm_data: %d", ntohl(wc[i].imm_data));
				}

				ctx = (struct msk_ctx *)wc[i].wr_id;

				// fill all the sizes in case of multiple sge
				len = wc[i].byte_len;
				data = ctx->data;
				while (data && len > data->max_size) {
					data->size = data->max_size;
					VALGRIND_MAKE_MEM_DEFINED(data->data, data->size);
					len -= data->max_size;
					data = data->next;
				}
				if (data) {
					data->size = len;
					VALGRIND_MAKE_MEM_DEFINED(data->data, data->size);
				} else if (len) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "received more than could fit? %d leftover bytes", len);
				}

				msk_signal_worker(trans, ctx, wc[i].status, wc[i].opcode);
				break;

			default:
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unknown opcode: %d", wc[i].opcode);
				ret = EINVAL;
			}
		}
	}

	if (npoll < 0) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_poll_cq failed: %d (%s)", -npoll, strerror(-npoll));
		ret = -npoll;
	}

	ibv_ack_cq_events(trans->cq, 1);

	return -ret;
}

/**
 * msk_cq_thread: thread function which waits for new completion events and gives them to handler (then ack the event)
 *
 */
static void *msk_cq_thread(void *arg) {
	struct msk_trans *trans;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	struct timespec ts_start, ts_end;
	int nfds, n;
	int ret;

	while (msk_global_state->run_threads > 0) {
		nfds = epoll_wait(msk_global_state->cq_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);
		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "epoll_pwait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (struct msk_trans*)epoll_events[n].data.ptr;

			if (!trans) {
				INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "epoll error or hup (%d)", epoll_events[n].events);
				continue;
			}
			if (trans->debug & MSK_DEBUG_SPEED)
				clock_gettime(CLOCK_MONOTONIC, &ts_start);

			msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
			if (trans->state >= MSK_CLOSING) { /* CLOSING, CLOSED, ERROR */
				// closing trans, skip this, will be done on flush
				msk_cq_delfd(trans);
				msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
				continue;
			}

			ret = msk_cq_event_handler(trans);
			if (ret) {
				if (trans->state != MSK_CLOSED && trans->state != MSK_CLOSING && trans->state != MSK_ERROR) {
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "something went wrong with our cq_event_handler");
					trans->state = MSK_ERROR;
					pthread_cond_broadcast(&trans->cm_cond);
				}
			}
			msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

			if (trans->debug & MSK_DEBUG_SPEED) {
				clock_gettime(CLOCK_MONOTONIC, &ts_end);
				sub_timespec(&trans->stats.nsec_compevent, &ts_start, &ts_end);
			}
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_cma_event_handler: handles addr/route resolved events (client side) and disconnect (everyone)
 *
 */
static int msk_cma_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event) {
	int i;
	int ret = 0;
	struct msk_trans *trans = cm_id->context;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "cma_event type %s", rdma_event_str(event->event));

	if (trans->bad_recv_wr) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that recv");
	}
	if (trans->bad_send_wr) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Something was bad on that send");
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "ADDR_RESOLVED");
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		trans->state = MSK_ADDR_RESOLVED;
		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "ROUTE_RESOLVED");
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		trans->state = MSK_ROUTE_RESOLVED;
		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ESTABLISHED");
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		if ((ret = msk_check_create_epoll_thread(&msk_global_state->cq_thread, msk_cq_thread, trans, &msk_global_state->cq_epollfd))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread for cq failed: %s (%d)", strerror(ret), ret);
			trans->state = MSK_ERROR;
		} else if (trans->stats_prefix != NULL && (ret = msk_check_create_epoll_thread(&msk_global_state->stats_thread, msk_stats_thread, trans, &msk_global_state->stats_epollfd))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread for stats failed: %s (%d)", strerror(ret), ret);
			trans->state = MSK_ERROR;
		} else {
			trans->state = MSK_CONNECTED;
		}

		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "CONNECT_REQUEST");
		//even if the cm_id is new, trans is the good parent's trans.
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

		//FIXME don't run through this stupidely and remember last index written to and last index read, i.e. use as a queue
		/* Find an empty connection request slot */
		for (i = 0; i < trans->server; i++)
			if (!trans->conn_requests[i])
				break;

		if (i == trans->server) {
			msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not pile up new connection requests' cm_id!");
			ret = ENOBUFS;
			break;
		}

		// write down new cm_id and signal accept handler there's stuff to do
		trans->conn_requests[i] = cm_id;
		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cma event %s, error %d",
			rdma_event_str(event->event), event->status);
		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		trans->state = MSK_ERROR;
		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "DISCONNECT EVENT...");

		// don't call completion again
		if (trans->comp_channel)
			msk_cq_delfd(trans);

		msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		trans->state = MSK_CLOSED;
		pthread_cond_broadcast(&trans->cm_cond);
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

		if (trans->disconnect_callback)
			trans->disconnect_callback(trans);

		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "cma detected device removal!!!!");
		ret = ENODEV;
		break;

	default:
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

/**
 * msk_cm_thread: thread function which waits for new connection events and gives them to handler (then ack the event)
 *
 */
static void *msk_cm_thread(void *arg) {
	struct msk_trans *trans;
	struct rdma_cm_event *event;
	struct epoll_event epoll_events[EPOLL_MAX_EVENTS];
	int nfds, n;
	int ret;

	while (msk_global_state->run_threads > 0) {
		nfds = epoll_wait(msk_global_state->cm_epollfd, epoll_events, EPOLL_MAX_EVENTS, 100);

		if (nfds == 0 || (nfds == -1 && errno == EINTR))
			continue;

		if (nfds == -1) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "epoll_wait failed: %s (%d)", strerror(ret), ret);
			break;
		}

		for (n = 0; n < nfds; ++n) {
			trans = (struct msk_trans*)epoll_events[n].data.ptr;
			if (!trans) {
				INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "got an event on a fd that should have been removed! (no trans)");
				continue;
			}

			if (epoll_events[n].events == EPOLLERR || epoll_events[n].events == EPOLLHUP) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "epoll error or hup (%d)", epoll_events[n].events);
				continue;
			}
			if (trans->state == MSK_CLOSED) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "got a cm event on a closed trans?");
				continue;
			}

			if (!trans->event_channel) {
				if (trans->state != MSK_CLOSED)
					INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "no event channel? :D");
				continue;
			}

			ret = rdma_get_cm_event(trans->event_channel, &event);
			if (ret) {
				ret = errno;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_get_cm_event failed: %d.", ret);
				continue;
			}
			ret = msk_cma_event_handler(event->id, event);

			trans = event->id->context;
			if (ret && (trans->state != MSK_LISTENING || trans == event->id->context)) {
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "something happened in cma_event_handler: %d", ret);
			}
			rdma_ack_cm_event(event);

			if (trans->state == MSK_CLOSED && trans->destroy_on_disconnect)
				msk_destroy_trans(&trans);
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_flush_buffers: Flush all pending recv/send
 *
 * @param trans [IN]
 *
 * @return void
 */
static void msk_flush_buffers(struct msk_trans *trans) {
	struct msk_ctx *ctx;
	int i, wait, ret;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "flushing %p", trans);

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	if (trans->state != MSK_ERROR) {
		do {
			ret = msk_cq_event_handler(trans);
		} while (ret == 0);

		if (ret != EAGAIN)
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't flush pending data in cq: %d", ret);
	}

	/* only flush rx if client or accepting server */
	if (trans->server >= 0)
	    for (i = 0, ctx = trans->rctx;
		 i < trans->rq_depth;
		 i++, ctx = (struct msk_ctx*)((uint8_t*)ctx + sizeof(struct msk_ctx) + trans->max_recv_sge*sizeof(struct ibv_sge)))
			if (ctx->used == MSK_CTX_PENDING)
				msk_signal_worker(trans, ctx, IBV_WC_FATAL_ERR, IBV_WC_RECV);

	for (i = 0, ctx = (struct msk_ctx *)trans->wctx;
	     i < trans->sq_depth;
	     i++, ctx = (struct msk_ctx*)((uint8_t*)ctx + sizeof(struct msk_ctx) + trans->max_send_sge*sizeof(struct ibv_sge)))
		if (ctx->used == MSK_CTX_PENDING)
			msk_signal_worker(trans, ctx, IBV_WC_FATAL_ERR, IBV_WC_SEND);

	/* only flush rx if client or accepting server */
	if (trans->server >= 0) do {
		wait = 0;
			for (i = 0, ctx = trans->rctx;
			     i < trans->rq_depth;
			     i++, ctx = msk_next_ctx(ctx, trans->max_recv_sge))
				if (ctx->used != MSK_CTX_FREE)
					wait++;

	} while (wait && usleep(100000));
	do {
		wait = 0;
		for (i = 0, ctx = (struct msk_ctx *)trans->wctx;
		     i < trans->sq_depth;
		     i++, ctx = msk_next_ctx(ctx, trans->max_recv_sge))
			if (ctx->used != MSK_CTX_FREE)
				wait++;

	} while (wait && usleep(100000));

	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
}

/**
 * msk_destroy_qp: destroys all qp-related stuff for us
 *
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void msk_destroy_qp(struct msk_trans *trans) {
	if (trans->qp) {
		// flush all pending receive/send buffers to error callback
		msk_flush_buffers(trans);

		ibv_destroy_qp(trans->qp);
		trans->qp = NULL;
	}
	if (trans->cq) {
		ibv_destroy_cq(trans->cq);
		trans->cq = NULL;
	}
	if (trans->comp_channel) {
		ibv_destroy_comp_channel(trans->comp_channel);
		trans->comp_channel = NULL;
	}
	/* only dealloc PDs if client or accepting server */
	if (trans->server >= 0 && trans->pd) {
		if (atomic_dec(trans->pd->refcnt) == 0) {
			int i = 0;
			while (trans->pd[i].context != PD_GUARD && trans->pd[i].context != NULL) {
				if (trans->pd[i].pd)
					ibv_dealloc_pd(trans->pd[i].pd);
				trans->pd[i].pd = NULL;
				if (trans->pd[i].srq)
					ibv_destroy_srq(trans->pd[i].srq);
				trans->pd[i].srq = NULL;
				if (trans->pd[i].rctx)
					free(trans->pd[i].rctx);
				trans->pd[i].rctx = NULL;
				i++;
			}
			free(trans->pd);
			trans->pd = NULL;
		}
	}
	if (!trans->srq) {
		free(trans->rctx);
	}
	trans->rctx = NULL;
	if (trans->wctx) {
		free(trans->wctx);
		trans->wctx = NULL;
	}
}


/**
 * msk_destroy_trans: disconnects and free trans data
 *
 * @param ptrans [INOUT] pointer to the trans to destroy
 */
void msk_destroy_trans(struct msk_trans **ptrans) {
	struct msk_trans *trans = *ptrans;

	if (trans) {
		trans->destroy_on_disconnect = 0;
		if (trans->state == MSK_CONNECTED || trans->state == MSK_CLOSED) {
			msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
			if (trans->state != MSK_CLOSED && trans->state != MSK_LISTENING && trans->state != MSK_ERROR)
				trans->state = MSK_CLOSING;

			if (trans->cm_id && trans->cm_id->verbs)
				rdma_disconnect(trans->cm_id);

			while (trans->state != MSK_CLOSED && trans->state != MSK_LISTENING && trans->state != MSK_ERROR) {
				INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "we're not closed yet, waiting for disconnect_event");
				msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
			}
			trans->state = MSK_CLOSED;
			msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		}

		if (trans->cm_id) {
			rdma_destroy_id(trans->cm_id);
			trans->cm_id = NULL;
		}

		if (trans->stats_sock)
			msk_stats_del(trans);

		// event channel is shared between all children, so don't close it unless it's its own.
		if ((trans->server != MSK_SERVER_CHILD) && trans->event_channel) {
			msk_cm_delfd(trans);
			rdma_destroy_event_channel(trans->event_channel);
			trans->event_channel = NULL;

			// likewise for stats prefix
			if (trans->stats_prefix)
				free(trans->stats_prefix);

			if (trans->node)
				free(trans->node);
			if (trans->port)
				free(trans->port);

		}

		// these two functions do the proper if checks
		msk_destroy_qp(trans);

		pthread_mutex_lock(&msk_global_state->lock);
		msk_global_state->run_threads--;
		if (msk_global_state->run_threads == 0) {
			if (msk_global_state->cm_thread) {
				pthread_join(msk_global_state->cm_thread, NULL);
				msk_global_state->cm_thread = 0;
			}
			if (msk_global_state->cq_thread) {
				pthread_join(msk_global_state->cq_thread, NULL);
				msk_global_state->cq_thread = 0;
			}
			if (msk_global_state->stats_thread) {
				pthread_join(msk_global_state->stats_thread, NULL);
				msk_global_state->stats_thread = 0;
			}
			msk_kill_worker_threads();
		}
		pthread_mutex_unlock(&msk_global_state->lock);


		//FIXME check if it is init. if not should just return EINVAL but.. lock.__lock, cond.__lock might work.
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		pthread_mutex_destroy(&trans->cm_lock);
		pthread_cond_destroy(&trans->cm_cond);

		free(trans);
		*ptrans = NULL;
	}
}

/**
 * msk_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
 * @param attr   [IN]    attributes to set parameters in ptrans. attr->addr must be set, others can be either 0 or sane values.
 *
 * @return 0 on success, errno value on failure
 */
int msk_init(struct msk_trans **ptrans, struct msk_trans_attr *attr) {
	int ret;

	struct msk_trans *trans;

	if (!ptrans || !attr) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Invalid argument");
		return EINVAL;
	}

	trans = malloc(sizeof(struct msk_trans));
	if (!trans) {
		INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "Out of memory");
		return ENOMEM;
	}

	do {
		memset(trans, 0, sizeof(struct msk_trans));

		trans->event_channel = rdma_create_event_channel();
		if (!trans->event_channel) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "create_event_channel failed: %s (%d)", strerror(ret), ret);
			break;
		}

		trans->conn_type = (attr->conn_type ? attr->conn_type : RDMA_PS_TCP);

		ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, trans->conn_type);
		if (ret) {
			ret = errno;
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "create_id failed: %s (%d)", strerror(ret), ret);
			break;
		}

		trans->state = MSK_INIT;

		if (!attr->node || !attr->port) {
			INFO_LOG(msk_global_state->debug & MSK_DEBUG_EVENT, "node and port have to be defined");
			ret = EDESTADDRREQ;
			break;
		}
		trans->node = strdup(attr->node);
		if (!trans->node) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->node");
				break;
		}
		trans->port = strdup(attr->port);
		if (!trans->port) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->port");
				break;
		}
		/*memcpy(trans->qp_attr, attr->qp_attr, sizeof(struct ibv_qp_init_attr));*/

		/* fill in default values */
		trans->sq_depth = attr->sq_depth ? attr->sq_depth : 50;
		trans->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
		trans->rq_depth = attr->rq_depth ? attr->rq_depth : 50;
		trans->max_recv_sge = attr->max_recv_sge ? attr->max_recv_sge : 1;

		trans->server = attr->server;
		/* listening trans's srq is used as a bool.. */
		if (trans->server)
			trans->srq = attr->use_srq ? (void*)1 : NULL;

		trans->debug = attr->debug;
		trans->timeout = attr->timeout   ? attr->timeout  : 30000; // in ms
		trans->disconnect_callback = attr->disconnect_callback;
		trans->destroy_on_disconnect = attr->destroy_on_disconnect;
		if (attr->stats_prefix) {
			ret = strlen(attr->stats_prefix)+1;
			trans->stats_prefix = malloc(ret);
			if (!trans->stats_prefix) {
				ret = ENOMEM;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->stats_prefix");
				break;
			}

			strncpy(trans->stats_prefix, attr->stats_prefix, ret);
		}

		if (attr->pd) {
			if (atomic_postinc(attr->pd->refcnt) > 0)
				trans->pd = attr->pd;
			else
				atomic_dec(attr->pd->refcnt);
		}

		ret = pthread_mutex_init(&trans->cm_lock, NULL)
			|| pthread_cond_init(&trans->cm_cond, NULL);
		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "pthread_mutex/cond_init failed: %s (%d)", strerror(ret), ret);
			break;
		}

		pthread_mutex_lock(&msk_global_state->lock);
		msk_global_state->debug = trans->debug;
		if (msk_global_state->run_threads == 0) {
			msk_global_state->worker_pool.worker_count = attr->worker_count ? attr->worker_count : -1;

			/* round up worker_pool.size to the next bigger power of two */
			attr->worker_queue_size = attr->worker_queue_size ? attr->worker_queue_size : 64;
			msk_global_state->worker_pool.size = 2;
			while (msk_global_state->worker_pool.size < attr->worker_queue_size)
				msk_global_state->worker_pool.size *= 2;
		}
		msk_global_state->run_threads++;
		pthread_mutex_unlock(&msk_global_state->lock);
		ret = msk_spawn_worker_threads();

		if (ret) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not start worker threads: %s (%d)", strerror(ret), ret);
			break;
		}
	} while (0);

	if (ret) {
		msk_destroy_trans(&trans);
		return ret;
	}

	*ptrans = trans;

	return 0;
}

/**
 * msk_create_qp: create a qp associated with a trans
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @ret 0 on success, errno value on error
 */
static int msk_create_qp(struct msk_trans *trans, struct rdma_cm_id *cm_id) {
	int ret;
	struct ibv_qp_init_attr qp_attr = {
		.cap.max_send_wr = trans->sq_depth,
		.cap.max_send_sge = trans->max_send_sge,
		.cap.max_recv_wr = trans->rq_depth,
		.cap.max_recv_sge = trans->max_recv_sge,
		.cap.max_inline_data = 0, // change if IMM
		.qp_type = (trans->conn_type == RDMA_PS_UDP ? IBV_QPT_UD : IBV_QPT_RC),
		.sq_sig_all = 1,
		.send_cq = trans->cq,
		.recv_cq = trans->cq,
		.srq = trans->srq,
	};

	if (rdma_create_qp(cm_id, msk_getpd(trans)->pd, &qp_attr)) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_create_qp: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->qp = cm_id->qp;
	return 0;
}

/**
 * msk_setup_qp: setups pd, qp an' stuff
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int msk_setup_qp(struct msk_trans *trans) {
	int ret;

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "trans: %p", trans);

	trans->comp_channel = ibv_create_comp_channel(trans->cm_id->verbs);
	if (!trans->comp_channel) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_create_comp_channel failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	trans->cq = ibv_create_cq(trans->cm_id->verbs, trans->sq_depth + trans->rq_depth,
				  trans, trans->comp_channel, 0);
	if (!trans->cq) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_create_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_req_notify_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	ret = msk_create_qp(trans, trans->cm_id);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "our own create_qp failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "created qp %p", trans->qp);
	return 0;
}


/**
 * msk_setup_*ctx
 */
static int msk_setup_rctx(struct msk_trans *trans) {
	trans->rctx = malloc(trans->rq_depth * (sizeof(struct msk_ctx) + trans->max_recv_sge * sizeof(struct ibv_sge)));
	if (!trans->rctx) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->rctx");
		return ENOMEM;
	}
	memset(trans->rctx, 0, trans->rq_depth * (sizeof(struct msk_ctx) + trans->max_recv_sge * sizeof(struct ibv_sge)));
	return 0;
}

static int msk_setup_wctx(struct msk_trans *trans) {
	trans->wctx = malloc(trans->sq_depth * (sizeof(struct msk_ctx) + trans->max_send_sge * sizeof(struct ibv_sge)));
	if (!trans->wctx) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "couldn't malloc trans->wctx");
		return ENOMEM;
	}
	memset(trans->wctx, 0, trans->sq_depth * (sizeof(struct msk_ctx) + trans->max_send_sge * sizeof(struct ibv_sge)));

	return 0;
}

/**
 * msk_bind_server
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
int msk_bind_server(struct msk_trans *trans) {
	struct rdma_addrinfo hints, *res;
	int ret;

	if (!trans || trans->state != MSK_INIT) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server <= 0) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Must be on server side to call this function");
		return EINVAL;
	}


	trans->conn_requests = malloc(trans->server * sizeof(struct rdma_cm_id*));
	if (!trans->conn_requests) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not allocate conn_requests buffer");
		return ENOMEM;
	}

	memset(trans->conn_requests, 0, trans->server * sizeof(struct rdma_cm_id*));

	memset(&hints, 0, sizeof(struct rdma_addrinfo));
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_port_space = trans->conn_type;

	ret = rdma_getaddrinfo(trans->node, trans->port, &hints, &res);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_getaddrinfo: %s (%d)", strerror(ret), ret);
		return ret;
	}
	ret = rdma_bind_addr(trans->cm_id, res->ai_src_addr);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_bind_addr: %s (%d)", strerror(ret), ret);
		return ret;
	}
	rdma_freeaddrinfo(res);

	ret = msk_setup_pd(trans);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "setup pd failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	ret = rdma_listen(trans->cm_id, trans->server);
	if (ret) {
		ret = errno;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_listen failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->state = MSK_LISTENING;

	if ((ret = msk_check_create_epoll_thread(&msk_global_state->cm_thread, msk_cm_thread, trans, &msk_global_state->cm_epollfd))) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread failed: %s (%d)", strerror(ret), ret);
		return ret;
	}
	msk_cm_addfd(trans);

	return 0;
}


static struct msk_trans *clone_trans(struct msk_trans *listening_trans, struct rdma_cm_id *cm_id) {
	struct msk_trans *trans = malloc(sizeof(struct msk_trans));
	struct msk_pd *pd;
	int ret;

	if (!trans) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "malloc failed");
		return NULL;
	}

	memcpy(trans, listening_trans, sizeof(struct msk_trans));

	trans->cm_id = cm_id;
	trans->cm_id->context = trans;
	trans->state = MSK_CONNECT_REQUEST;
	trans->server = MSK_SERVER_CHILD;

	pd = msk_getpd(trans);
	if (!pd) {
		ret = ENOSPC;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "No space left in msk pd, multiple contexts per device?");
		return NULL;
	}
	if (!pd->pd) {
		pd->pd = ibv_alloc_pd(trans->cm_id->verbs);
		if (!pd->pd) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_alloc_pd failed: %s (%d)", strerror(ret), ret);
			return NULL;
		}
	}
	if (listening_trans->srq) {
		if (!pd->srq) {
			struct ibv_srq_init_attr srq_attr = {
				.attr.max_wr = trans->rq_depth,
				.attr.max_sge = trans->max_recv_sge,
			};
			pd->srq = ibv_create_srq(pd->pd, &srq_attr);
			if (!pd->srq) {
				ret = errno;
				INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_create_srq failed: %s (%d)", strerror(ret), ret);
				return NULL;
			}
		}

		if (!pd->rctx) {
			msk_setup_rctx(trans);
			pd->rctx = trans->rctx;
		} else {
			trans->rctx = pd->rctx;
		}
	} else {
		msk_setup_rctx(trans);
	}

	trans->srq = pd->srq;

	memset(&trans->cm_lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->cm_cond, 0, sizeof(pthread_cond_t));

	ret = pthread_mutex_init(&trans->cm_lock, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cm_cond, NULL);
	if (ret) {
		INFO_LOG(listening_trans->debug & MSK_DEBUG_EVENT, "pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}

	pthread_mutex_lock(&msk_global_state->lock);
	msk_global_state->run_threads++;
	pthread_mutex_unlock(&msk_global_state->lock);

	return trans;
}

/**
 * msk_finalize_accept: does the real connection acceptance and wait for other side to be ready
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int msk_finalize_accept(struct msk_trans *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_CONNECT_REQUEST) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans isn't from a connection request?");
		return EINVAL;
	}

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.private_data = NULL;
	conn_param.private_data_len = 0;
	conn_param.rnr_retry_count = 10;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
	do {
		ret = rdma_accept(trans->cm_id, &conn_param);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_accept failed: %s (%d)", strerror(ret), ret);
			break;
		}
		while (trans->state == MSK_CONNECT_REQUEST) {
			msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state == MSK_CONNECTED) {
			msk_cq_addfd(trans);
			msk_stats_add(trans);
		} else {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Accept failed");
			ret = ECONNRESET;
			break;
		}
	} while (0);

	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	return ret;
}

/**
 * msk_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param trans [IN] the parent trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
struct msk_trans *msk_accept_one_timedwait(struct msk_trans *trans, struct timespec *abstime) { //TODO make it return an int an' use trans as argument

	//TODO: timeout?

	struct rdma_cm_id *cm_id = NULL;
	struct msk_trans *child_trans = NULL;
	int i, ret;

	if (!trans || trans->state != MSK_LISTENING) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans isn't listening (after bind_server)?");
		return NULL;
	}

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
	ret = 0;
	while (!cm_id && ret == 0) {
		/* See if one of the slots has been taken */
		for (i = 0; i < trans->server; i++)
			if (trans->conn_requests[i])
				break;

		if (i == trans->server) {
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Waiting for a connection to come in");
			if (abstime)
				ret = msk_cond_timedwait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock, abstime);
			else
				ret = msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
		} else {
			cm_id = trans->conn_requests[i];
			trans->conn_requests[i] = NULL;
		}
	}

	if (ret) {
		msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);
		return NULL;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a connection request - creating child");
	child_trans = clone_trans(trans, cm_id);

	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	if (child_trans) {
		if ((ret = msk_setup_qp(child_trans))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not setup child trans's qp: %s (%d)", strerror(ret), ret);
			msk_destroy_trans(&child_trans);
			return NULL;
		}
		if ((ret = msk_setup_wctx(child_trans))) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not setup child trans's buffer: %s (%d)", strerror(ret), ret);
			msk_destroy_trans(&child_trans);
			return NULL;
		}
	}
	return child_trans;
}

struct msk_trans *msk_accept_one_wait(struct msk_trans *trans, int msleep) {
	struct timespec ts;

	if (msleep == 0)
		return msk_accept_one(trans);

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += msleep / 1000;
	ts.tv_nsec += (msleep % 1000) * NSEC_IN_SEC;
	if (ts.tv_nsec >= NSEC_IN_SEC) {
		ts.tv_nsec -= NSEC_IN_SEC;
		ts.tv_sec++;
	}

	return msk_accept_one_timedwait(trans, &ts);
}

/**
 * msk_bind_client: resolve addr and route for the client and waits till it's done
 * (the route and pthread_cond_signal is done in the cm thread)
 *
 */
static int msk_bind_client(struct msk_trans *trans) {
	struct rdma_addrinfo hints, *res;
	int ret;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	do {
		memset(&hints, 0, sizeof(struct rdma_addrinfo));
		hints.ai_port_space = trans->conn_type;

		ret = rdma_getaddrinfo(trans->node, trans->port, &hints, &res);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_getaddrinfo: %s (%d)", strerror(ret), ret);
			break;
		}

		ret = rdma_resolve_addr(trans->cm_id, res->ai_src_addr, res->ai_dst_addr, trans->timeout);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_resolve_addr failed: %s (%d)", strerror(ret), ret);
			break;
		}
		rdma_freeaddrinfo(res);


		while (trans->state == MSK_INIT) {
			msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}
		if (trans->state != MSK_ADDR_RESOLVED) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not resolve addr");
			ret = EINVAL;
			break;
		}

		ret = rdma_resolve_route(trans->cm_id, trans->timeout);
		if (ret) {
			trans->state = MSK_ERROR;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_resolve_route failed: %s (%d)", strerror(ret), ret);
			break;
		}

		while (trans->state == MSK_ADDR_RESOLVED) {
			msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state != MSK_ROUTE_RESOLVED) {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Could not resolve route");
			ret = EINVAL;
			break;
		}
	} while (0);

	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	return ret;
}

/**
 * msk_finalize_connect: tells the other side we're ready to receive stuff (does the actual rdma_connect) and waits for its ack
 *
 * @param trans [IN]
 *
 * @return 0 on success, errno value on failure
 */
int msk_finalize_connect(struct msk_trans *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_ROUTE_RESOLVED) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans isn't half-connected?");
		return EINVAL;
	}


	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 10;
	conn_param.retry_count = 10;

	msk_mutex_lock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	do {
		ret = rdma_connect(trans->cm_id, &conn_param);
		if (ret) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "rdma_connect failed: %s (%d)", strerror(ret), ret);
			break;
		}

		while (trans->state == MSK_ROUTE_RESOLVED) {
			msk_cond_wait(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_cond, &trans->cm_lock);
			INFO_LOG(trans->debug & MSK_DEBUG_SETUP, "Got a cond, state: %i", trans->state);
		}

		if (trans->state == MSK_CONNECTED) {
			msk_cq_addfd(trans);
			msk_stats_add(trans);
		} else {
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Connection failed");
			ret = ECONNREFUSED;
			break;
		}
	} while (0);

	msk_mutex_unlock(trans->debug & MSK_DEBUG_CM_LOCKS, &trans->cm_lock);

	return ret;
}

/**
 * msk_connect: connects a client to a server
 *
 * @param trans [INOUT] trans must be init first
 *
 * @return 0 on success, the value of errno on error
 */
int msk_connect(struct msk_trans *trans) {
	int ret;
	struct msk_pd *pd;

	if (!trans || trans->state != MSK_INIT) {
		INFO_LOG((trans ? trans->debug : 0) & MSK_DEBUG_EVENT, "trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "Must be on client side to call this function");
		return EINVAL;
	}

	if ((ret = msk_check_create_epoll_thread(&msk_global_state->cm_thread, msk_cm_thread, trans, &msk_global_state->cm_epollfd))) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "msk_check_create_epoll_thread failed: %s (%d)", strerror(ret), ret);
		return ret;
	}
	msk_cm_addfd(trans);

	if ((ret = msk_bind_client(trans)))
		return ret;

	/* pick the right pd if we already have one, allocate otherwise */
	ret = msk_setup_pd(trans);
	if (ret) {
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "setup pd failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	pd = msk_getpd(trans);
	if (!pd) {
		ret = ENOSPC;
		INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "No space left in msk pd, multiple contexts per device?");
		return ret;
	}
	if (!pd->pd) {
		pd->pd = ibv_alloc_pd(trans->cm_id->verbs);
		if (!pd->pd) {
			ret = errno;
			INFO_LOG(trans->debug & MSK_DEBUG_EVENT, "ibv_alloc_pd failed: %s (%d)", strerror(ret), ret);
			return ret;
		}
	}

	if ((ret = msk_setup_qp(trans)))
		return ret;
	if ((ret = msk_setup_wctx(trans)) || (ret = msk_setup_rctx(trans)))
		return ret;

	return 0;
}
