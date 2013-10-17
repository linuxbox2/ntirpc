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

#ifndef TIRPC_SVC_RQST_H
#define TIRPC_SVC_RQST_H

#include <misc/portable.h>
#include <misc/rbtree_x.h>

struct svc_rqst_rec;		/* forward decl */

struct svc_xprt_ev {
	/*
	 * union of event processor types
	 */
	enum svc_event_type ev_type;
	union {
#if defined(TIRPC_EPOLL)
		struct {
			struct epoll_event event;
		} epoll;
#endif
	} ev_u;

	/* state */
	uint32_t flags;

	/*
	 * thread on svc_xprt_rec
	 */
	struct opr_rbtree_node node_k;
	SVCXPRT *xprt;		/* contains this */
	struct svc_rqst_rec *sr_rec;	/* container of this */
};

struct svc_rqst_rec {
	/*
	 * union of event processor types
	 */
	enum svc_event_type ev_type;

	uint32_t flags;

	union {
#if defined(TIRPC_EPOLL)
		struct {
			int epoll_fd;
			struct epoll_event ctrl_ev;
			struct epoll_event *events;
			u_int max_events;	/* max epoll events */
		} epoll;
#endif
		struct {
			fd_set set;	/* select/fd_set (currently unhooked) */
		} fd;
	} ev_u;

	int sv[2];
	uint32_t id_k;		/* chan id */
	uint32_t states;
	uint32_t signals;
	void *u_data;		/* user-installable opaque data */
	struct opr_rbtree xprt_q;	/* sorted list of xprt handles */
	struct opr_rbtree_node node_k;
	uint32_t refcnt;
	uint64_t gen;		/* generation number */
	mutex_t mtx;
};

struct svc_rqst_set {
	mutex_t mtx;
	struct rbtree_x xt;
	uint32_t next_id;
};

static inline int rqst_thrd_cmpf(const struct opr_rbtree_node *lhs,
				 const struct opr_rbtree_node *rhs)
{
	struct svc_rqst_rec *lk, *rk;

	lk = opr_containerof(lhs, struct svc_rqst_rec, node_k);
	rk = opr_containerof(rhs, struct svc_rqst_rec, node_k);

	if (lk->id_k < rk->id_k)
		return (-1);

	if (lk->id_k == rk->id_k)
		return (0);

	return (1);
}

static inline int rqst_xprt_cmpf(const struct opr_rbtree_node *lhs,
				 const struct opr_rbtree_node *rhs)
{
	SVCXPRT *lk, *rk;

	lk = opr_containerof(lhs, struct svc_xprt_ev, node_k)->xprt;
	rk = opr_containerof(rhs, struct svc_xprt_ev, node_k)->xprt;

	/* XXX we just want each xprt handle unique, in some order
	 * (just use the address) */
	if (lk < rk)
		return (-1);

	if (lk == rk)
		return (0);

	return (1);
}

/*
 * exported interface:
 *
 *  svc_rqst_init -- init module (optional)
 *  svc_rqst_init_xprt -- init svc_rqst part of xprt handle
 *  svc_rqst_finalize_xprt -- free it
 *  svc_rqst_new_evchan -- create event channel
 *  svc_rqst_delete_evchan -- delete event channel
 *  svc_rqst_evchan_reg -- set {xprt, dispatcher} mapping 
 *  svc_rqst_evchan_runreg -- unset {xprt, dispatcher} mapping
 *  svc_rqst_foreach_xprt -- scan registered xprts at id (or 0 for all)
 *  svc_rqst_thrd_run -- enter dispatch loop at id
 *  svc_rqst_thrd_signal --request thread to run a callout function which
 *   can cause the thread to return
 *  svc_rqst_shutdown -- cause all threads to return
 *
 * callback function interface:
 *
 * svc_xprt_rendezvous -- called when a new transport connection is accepted,
 *  can be used by the application to chose a correct request handler, or do
 *  other adaptation
 */
void svc_rqst_init();
void svc_rqst_init_xprt(SVCXPRT * xprt);
void svc_rqst_finalize_xprt(SVCXPRT * xprt, uint32_t flags);
int svc_rqst_new_evchan(uint32_t * chan_id /* OUT */ , void *u_data,
			uint32_t flags);
int svc_rqst_delete_evchan(uint32_t chan_id, uint32_t flags);
int svc_rqst_evchan_reg(uint32_t chan_id, SVCXPRT * xprt, uint32_t flags);
int svc_rqst_evchan_unreg(uint32_t chan_id, SVCXPRT * xprt, uint32_t flags);
int svc_rqst_rearm_events(SVCXPRT * xprt, uint32_t flags);
int svc_rqst_xprt_register(SVCXPRT * xprt, SVCXPRT * newxprt);
int svc_rqst_xprt_unregister(SVCXPRT * xprt, uint32_t flags);
int svc_rqst_thrd_run(uint32_t chan_id, uint32_t flags);
int svc_rqst_thrd_signal(uint32_t chan_id, uint32_t flags);

/* xprt/connection rendezvous callout */
typedef int (*svc_rqst_rendezvous_t)
 (SVCXPRT * oxprt, SVCXPRT * nxprt, uint32_t flags);

/* iterator callback prototype */
typedef void (*svc_rqst_xprt_each_func_t) (uint32_t chan_id, SVCXPRT * xprt,
					   void *arg);
int svc_rqst_foreach_xprt(uint32_t chan_id, svc_rqst_xprt_each_func_t each_f,
			  void *arg);

#define XP_EV_FLAG_NONE               0x00000
#define XP_EV_FLAG_ADDED              0x00001
#define XP_EV_FLAG_BLOCKED            0x00002

#define SVC_RQST_FLAG_NONE            0x00000
#define SVC_RQST_FLAG_LOCK            0x00002
#define SVC_RQST_FLAG_UNLOCK          0x00004
#define SVC_RQST_FLAG_EPOLL           0x00008
#define SVC_RQST_FLAG_FDSET           0x00010
#define SVC_RQST_FLAG_SREC_LOCKED     0x00020
#define SVC_RQST_FLAG_MUTEX_LOCKED    0x00040
#define SVC_RQST_FLAG_REC_LOCK        0x00080

#define SVC_RQST_FLAG_CHAN_AFFINITY   0x01000	/* bind new conn to parent chan */
#define SVC_RQST_FLAG_CHAN_ACCEPT_CB  0x02000	/* make rendezvous callout? */

#define SVC_RQST_FLAG_XPRT_UREG       0x04000
#define SVC_RQST_FLAG_XPRT_DTOR       0x08000
#define SVC_RQST_FLAG_XPRT_GCHAN      0x10000

#define SVC_RQST_STATE_NONE           0x00000
#define SVC_RQST_STATE_ACTIVE         0x00001	/* thrd in event loop */
#define SVC_RQST_STATE_BLOCKED        0x00002	/* channel blocked */
#define SVC_RQST_STATE_DESTROYED      0x00004

#define SVC_RQST_SIGNAL_SHUTDOWN      0x00008	/* chan shutdown */

/* ie, masks unused bits */
#define SVC_RQST_SIGNAL_MASK ~(SVC_RQST_SIGNAL_SHUTDOWN)

#endif				/* TIRPC_SVC_RQST_H */
