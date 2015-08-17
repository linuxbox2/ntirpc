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

#ifndef TIRPC_SVC_INTERNAL_H
#define TIRPC_SVC_INTERNAL_H

#include <misc/os_epoll.h>

extern int __svc_maxiov;
extern int __svc_maxrec;

/* threading fdsets around is annoying */
struct svc_params {
	bool initialized;
	mutex_t mtx;
	u_long flags;

	/* package global event handling--may be overridden using the
	 * svc_rqst interface */
	enum svc_event_type ev_type;
	union {
		struct {
			uint32_t id;
			uint32_t max_events;
		} evchan;
		struct {
			fd_set set;	/* select/fd_set (currently unhooked) */
		} fd;
	} ev_u;

	int32_t idle_timeout;
	u_int max_connections;
	u_int svc_ioq_maxbuf;

	union {
		struct {
			mutex_t mtx;
			u_int nconns;
		} vc;
	} xprt_u;

	struct {
		int ctx_hash_partitions;
		int max_ctx;
		int max_idle_gen;
		int max_gc;
	} gss;

	struct {
		u_int thrd_max;
	} ioq;
};

extern struct svc_params __svc_params[1];

#define svc_cond_init()	\
	do { \
		if (!__svc_params->initialized) { \
			svc_init(&(svc_init_params) \
				 {       .flags = SVC_INIT_EPOLL, \
						 .max_connections = 8192, \
						 .max_events = 512, \
						 .gss_ctx_hash_partitions = 13,\
						 .gss_max_ctx = 1024,	\
						 .gss_max_idle_gen = 1024, \
						 .gss_max_gc = 200 \
						 }); \
		} \
	} while (0)


static inline bool
svc_vc_new_conn_ok(void)
{
	bool ok = false;
	svc_cond_init();
	mutex_lock((&__svc_params->xprt_u.vc.mtx));
	if (__svc_params->xprt_u.vc.nconns < __svc_params->max_connections) {
		++(__svc_params->xprt_u.vc.nconns);
		ok = true;
	}
	mutex_unlock(&(__svc_params->xprt_u.vc.mtx));
	return (ok);
}

#define svc_vc_dec_nconns() \
	do { \
		mutex_lock((&__svc_params->xprt_u.vc.mtx)); \
		--(__svc_params->xprt_u.vc.nconns); \
		mutex_unlock(&(__svc_params->xprt_u.vc.mtx)); \
	} while (0)

struct __svc_ops {
	bool (*svc_clean_idle) (fd_set *fds, int timeout, bool cleanblock);
	void (*svc_run) (void);
	void (*svc_getreq) (int rdfds);	/* XXX */
	void (*svc_getreqset) (fd_set *readfds); /* XXX */
	void (*svc_exit) (void);
};

extern struct __svc_ops *svc_ops;

#define	su_data(xprt)	((struct svc_dg_data *)(xprt->xp_p2))

/*  The CACHING COMPONENT */

/*
 * Could have been a separate file, but some part of it depends upon the
 * private structure of the client handle.
 *
 * Fifo cache for cl server
 * Copies pointers to reply buffers into fifo cache
 * Buffers are sent again if retransmissions are detected.
 */

#define	SPARSENESS 4		/* 75% sparse */

#define	ALLOC(type, size) \
	((type *) mem_alloc((sizeof(type) * (size))))

#define	MEMZERO(addr, type, size) \
	((void) memset((void *) (addr), 0, sizeof(type) * (int) (size)))

#define	FREE(addr, type, size)	\
	(mem_free((addr), (sizeof(type) * (size))))

/*
 * An entry in the cache
 */
typedef struct cache_node *cache_ptr;
struct cache_node {
	/*
	 * Index into cache is xid, proc, vers, prog and address
	 */
	u_int32_t cache_xid;
	rpcproc_t cache_proc;
	rpcvers_t cache_vers;
	rpcprog_t cache_prog;
	struct netbuf cache_addr;
	/*
	 * The cached reply and length
	 */
	char *cache_reply;
	size_t cache_replylen;
	/*
	 * Next node on the list, if there is a collision
	 */
	cache_ptr cache_next;
};

/*
 * the hashing function
 */
#define	CACHE_LOC(transp, xid)	\
	(xid % (SPARSENESS * ((struct cl_cache *) \
		su_data(transp)->su_cache)->uc_size))

extern mutex_t dupreq_lock;

/*
 * The entire cache
 */
struct cl_cache {
	u_int uc_size;		/* size of cache */
	cache_ptr *uc_entries;	/* hash table of entries in cache */
	cache_ptr *uc_fifo;	/* fifo list of entries in cache */
	u_int uc_nextvictim;	/* points to next victim in fifo list */
	rpcprog_t uc_prog;	/* saved program number */
	rpcvers_t uc_vers;	/* saved version number */
	rpcproc_t uc_proc;	/* saved procedure number */
};

/* Epoll interface change */
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 02000000
static inline int
epoll_create_wr(size_t size, int flags)
{
	return (epoll_create(size));
}
#else
static inline int
epoll_create_wr(size_t size, int flags)
{
	return (epoll_create1(flags));
}
#endif

#if defined(HAVE_BLKIN)
extern void __rpc_set_blkin_endpoint(SVCXPRT *xprt, const char *tag);
#endif

void svc_rqst_shutdown(void);

#endif				/* TIRPC_SVC_INTERNAL_H */
