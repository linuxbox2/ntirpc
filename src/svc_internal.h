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

#include <sys/socket.h>
#include <netinet/in.h>
#include <misc/os_epoll.h>
#include <rpc/rpc_msg.h>

#include "rpc_dplx_internal.h"

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
			(void)svc_init(&(svc_init_params) \
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

/*
 * The following union is defined just to use SVC_CMSG_SIZE macro for an array
 * length. _GNU_SOURCE must be defined to get in6_pktinfo declaration!
 */
union pktinfo_u {
	struct in_pktinfo x;
	struct in6_pktinfo y;
};
#define SVC_CMSG_SIZE CMSG_SPACE(sizeof(union pktinfo_u))

/**
 * \struct svc_dg_xprt
 * DG transport instance
 *
 * Replaces old struct svc_dg_data by locally wrapping struct rpc_dplx_rec,
 * which wraps struct rpc_svcxprt indexed by fd.
 */
struct svc_dg_xprt {
	struct rpc_dplx_rec su_dr;	/* SVCXPRT indexed by fd */

	struct msghdr su_msghdr;	/* msghdr received from clnt */
	size_t su_iosz;			/* size of send.recv buffer */
	u_int su_recvsz;
	u_int su_sendsz;

	unsigned char su_cmsg[SVC_CMSG_SIZE];	/* cmsghdr received from clnt */
};
#define DG_DR(p) (opr_containerof((p), struct svc_dg_xprt, su_dr))
#define su_data(xprt) (DG_DR(REC_XPRT(xprt)))

/**
 * \struct svc_vc_xprt
 * VC transport instance
 *
 * Replaces old struct x_vc_data by locally wrapping struct rpc_dplx_rec,
 * which wraps struct rpc_svcxprt indexed by fd.
 */
struct svc_vc_xprt {
	struct rpc_dplx_rec sx_dr;	/* SVCXPRT indexed by fd */
	struct {
		struct timeval cx_wait;	/* wait interval in milliseconds */
		bool cx_waitset;	/* wait set by clnt_control? */
	} cx;
	struct {
		enum xprt_stat strm_stat;
		struct timespec last_recv;	/* XXX move to shared? */
		int32_t maxrec;
	} sx;
	struct {
		u_int sendsz;
		u_int recvsz;
		bool nonblock;
	} shared;
};
#define VC_DR(p) (opr_containerof((p), struct svc_vc_xprt, sx_dr))

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
