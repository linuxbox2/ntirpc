
/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * svc.c, Server-side remote procedure call interface.
 *
 * There are two sets of procedures here.  The xprt routines are
 * for handling transport handles.  The svc routines handle the
 * list of service routines.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */
#include <config.h>

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#if !defined (_WIN32)
#include <err.h>
#include <sys/poll.h>
#endif

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif				/* PORTMAP */

#include "rpc_com.h"

#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include <rpc/svc_rqst.h>
#include "svc_ioq.h"

#define SVC_VERSQUIET 0x0001	/* keep quiet about vers mismatch */
#define version_keepquiet(xp) ((u_long)(xp)->xp_p3 & SVC_VERSQUIET)

/* svc_internal.h */
#ifdef IOV_MAX
int __svc_maxiov = IOV_MAX;
#else
int __svc_maxiov = 1024; /* UIO_MAXIOV value from sys/uio.h */
#endif
int __svc_maxrec = 0;

extern tirpc_pkg_params __pkg_params;

struct svc_params __svc_params[1] = {
	{
	 false /* !initialized */ ,
	 MUTEX_INITIALIZER}
};

/*
 * The services list
 * Each entry represents a set of procedures (an rpc program).
 * The dispatch routine takes request structs and runs the
 * apropriate procedure.
 *
 * The service record is factored out to permit exporting the find
 * routines without exposing the db implementation.
 */
static struct svc_callout {
	struct svc_callout *sc_next;
	struct svc_record rec;
} *svc_head;

extern rwlock_t svc_lock;
extern rwlock_t svc_fd_lock;

static struct svc_callout *svc_find(rpcprog_t, rpcvers_t, struct svc_callout **,
				    char *);

struct work_pool svc_work_pool;

static void
svc_work_pool_init()
{
	struct work_pool_params params = {
		.thrd_max = __svc_params->ioq.thrd_max,
		.thrd_min = 2
	};

	(void)work_pool_init(&svc_work_pool, "svc_work_pool", &params);
}

/* Package init function.
 * It is intended that applications which must make use of global state
 * will call svc_init() before accessing such state and before executing
 * any svc exported functions.   Traditional TI-RPC programs need not
 * call the function as presently integrated. */
void
svc_init(svc_init_params *params)
{
	mutex_lock(&__svc_params->mtx);
	if (__svc_params->initialized) {
		__warnx(TIRPC_DEBUG_FLAG_SVC,
			"svc_init: multiple initialization attempt (nothing happens)");
		mutex_unlock(&__svc_params->mtx);
		return;
	}
	__svc_params->max_connections =
	    (params->max_connections) ? params->max_connections : FD_SETSIZE;

	if (params->flags & SVC_INIT_WARNX)
		__pkg_params.warnx = params->warnx;
	else
		__pkg_params.warnx = warnx;

	/* svc_vc */
	__svc_params->xprt_u.vc.nconns = 0;
	mutex_init(&__svc_params->xprt_u.vc.mtx, NULL);

#if defined(TIRPC_EPOLL)
	if (params->flags & SVC_INIT_EPOLL) {
		__svc_params->ev_type = SVC_EVENT_EPOLL;
		__svc_params->ev_u.evchan.max_events = params->max_events;
	}
#else
	/* XXX formerly select/fd_set case, now placeholder for new
	 * event systems, reworked select, etc. */
#endif
	__svc_params->idle_timeout = params->idle_timeout;

	/* XXX defaults to the legacy max sendsz */
	__svc_params->svc_ioq_maxbuf =
	    (params->svc_ioq_maxbuf) ? (params->svc_ioq_maxbuf) : 262144;

	/* allow consumers to manage all xprt registration */
	if (params->flags & SVC_INIT_NOREG_XPRTS)
		__svc_params->flags |= SVC_FLAG_NOREG_XPRTS;

	if (params->ioq_thrd_max)
		__svc_params->ioq.thrd_max = params->ioq_thrd_max;
	else
		__svc_params->ioq.thrd_max = 200;

	/* uses ioq.thrd_max */
	svc_work_pool_init();

	if (params->gss_ctx_hash_partitions)
		__svc_params->gss.ctx_hash_partitions =
		    params->gss_ctx_hash_partitions;
	else
		__svc_params->gss.ctx_hash_partitions = 13;

	if (params->gss_max_ctx)
		__svc_params->gss.max_ctx = params->gss_max_ctx;
	else
		__svc_params->gss.max_ctx = 1024;

	if (params->gss_max_idle_gen)
		__svc_params->gss.max_idle_gen = params->gss_max_idle_gen;
	else
		__svc_params->gss.max_idle_gen = 1024;

	if (params->gss_max_gc)
		__svc_params->gss.max_gc = params->gss_max_gc;
	else
		__svc_params->gss.max_gc = 200;

	__svc_params->initialized = true;

	mutex_unlock(&__svc_params->mtx);

#if defined(_SC_IOV_MAX) /* IRIX, MacOS X, FreeBSD, Solaris, ... */
	__svc_maxiov = sysconf(_SC_IOV_MAX);
#endif
	return;
}

struct rpc_msg *
alloc_rpc_msg(void)
{
	struct rpc_msg *msg = mem_alloc(sizeof(struct rpc_msg));
	if (!msg)
		goto out;

	TAILQ_INIT_ENTRY(msg, msg_q);

	/* avoid separate alloc/free */
	msg->rm_call.cb_cred.oa_base = msg->cb_cred_body;
	msg->rm_call.cb_verf.oa_base = msg->cb_verf_body;

	/* required for REPLY decodes */
	msg->acpted_rply.ar_verf = _null_auth;
	msg->acpted_rply.ar_results.where = NULL;
	msg->acpted_rply.ar_results.proc = (xdrproc_t) xdr_void;

 out:
	return (msg);
}

void
free_rpc_msg(struct rpc_msg *msg)
{
	mem_free(msg, sizeof(struct rpc_msg));
}

static inline void
free_req_rpc_msg(struct svc_req *req)
{
	if (req->rq_msg) {
		struct rpc_msg *msg = req->rq_msg;
		mem_free(msg, sizeof(struct rpc_msg));
		req->rq_msg = NULL;
	}
}

/* ***************  SVCXPRT related stuff **************** */

/*
 * This is used to set local and remote addresses in a way legacy
 * apps can deal with, at the same time setting up a corresponding
 * netbuf -- with no alloc/free needed.
 */
void
__rpc_set_address(struct rpc_address *rpca, const struct sockaddr_storage *ss,
		  socklen_t sl)
{
	socklen_t l;

	switch (ss->ss_family) {
	case AF_INET6:
		l = sl ? sl : sizeof(struct sockaddr_in6);
		memcpy(&rpca->ss, ss, l);
		break;
	case AF_INET:
		l = sl ? sl : sizeof(struct sockaddr_in);
		memcpy(&rpca->ss, ss, l);
		break;
	case AF_LOCAL:
		l = sl ? sl : sizeof(struct sockaddr);
		memcpy(&rpca->ss, ss, l);
		break;
	default:
		memset(&rpca->ss, 0xfe, sizeof(struct sockaddr_storage));
		l = sl ? sl : sizeof(struct sockaddr);
		rpca->ss.ss_family = AF_UNSPEC;
		break;
	}
	rpca->nb.buf = &rpca->ss;
	rpca->nb.len = l;
	rpca->nb.maxlen = sizeof(struct sockaddr_storage);
}

/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool
svc_reg(SVCXPRT *xprt, const rpcprog_t prog, const rpcvers_t vers,
	void (*dispatch) (struct svc_req *req, SVCXPRT *xprt),
	const struct netconfig *nconf)
{
	bool dummy;
	struct svc_callout *prev;
	struct svc_callout *s;
	struct netconfig *tnconf;
	char *netid = NULL;
	int flag = 0;

	/* VARIABLES PROTECTED BY svc_lock: s, prev, svc_head */
	if (xprt->xp_netid) {
		netid = rpc_strdup(xprt->xp_netid);
		flag = 1;
	} else if (nconf) {
		netid = rpc_strdup(nconf->nc_netid);
		flag = 1;
	} else {
		tnconf = __rpcgettp(xprt->xp_fd);
		if (tnconf) {
			netid = rpc_strdup(tnconf->nc_netid);
			flag = 1;
			freenetconfigent(tnconf);
		}
	} /* must have been created with svc_raw_create */
	if ((netid == NULL) && (flag == 1))
		return (false);

	rwlock_wrlock(&svc_lock);
	s = svc_find(prog, vers, &prev, netid);
	if (s) {
		if (netid)
			mem_free(netid, 0);
		if (s->rec.sc_dispatch == dispatch)
			goto rpcb_it;	/* he is registering another xptr */
		rwlock_unlock(&svc_lock);
		return (false);
	}
	s = mem_alloc(sizeof(struct svc_callout));
	if (s == NULL) {
		if (netid)
			mem_free(netid, 0);
		rwlock_unlock(&svc_lock);
		return (false);
	}

	s->rec.sc_prog = prog;
	s->rec.sc_vers = vers;
	s->rec.sc_dispatch = dispatch;
	s->rec.sc_netid = netid;
	s->sc_next = svc_head;
	svc_head = s;

	if ((xprt->xp_netid == NULL) && (flag == 1) && netid)
		((SVCXPRT *) xprt)->xp_netid = rpc_strdup(netid);

 rpcb_it:
	rwlock_unlock(&svc_lock);
	/* now register the information with the local binder service */
	if (nconf) {
		/*LINTED const castaway */
		dummy =
		    rpcb_set(prog, vers, (struct netconfig *)nconf,
			     &((SVCXPRT *) xprt)->xp_local.nb);
		return (dummy);
	}
	return (true);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unreg(const rpcprog_t prog, const rpcvers_t vers)
{
	struct svc_callout *prev;
	struct svc_callout *s;

	/* unregister the information anyway */
	(void)rpcb_unset(prog, vers, NULL);
	rwlock_wrlock(&svc_lock);
	while ((s = svc_find(prog, vers, &prev, NULL)) != NULL) {
		if (prev == NULL)
			svc_head = s->sc_next;
		else
			prev->sc_next = s->sc_next;
		s->sc_next = NULL;
		if (s->rec.sc_netid)
			mem_free(s->rec.sc_netid, sizeof(s->rec.sc_netid) + 1);
		mem_free(s, sizeof(struct svc_callout));
	}
	rwlock_unlock(&svc_lock);
}

/* ********************** CALLOUT list related stuff ************* */

#ifdef PORTMAP
/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool
svc_register(SVCXPRT *xprt, u_long prog, u_long vers,
	     void (*dispatch) (struct svc_req *req, SVCXPRT *xprt),
	     int protocol)
{
	struct svc_callout *prev;
	struct svc_callout *s;

	assert(xprt != NULL);
	assert(dispatch != NULL);

	s = svc_find((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL);
	if (s) {
		if (s->rec.sc_dispatch == dispatch)
			goto pmap_it;	/* he is registering another xprt */
		return (false);
	}
	s = mem_alloc(sizeof(struct svc_callout));
	if (!s)
		return (false);
	s->rec.sc_prog = (rpcprog_t) prog;
	s->rec.sc_vers = (rpcvers_t) vers;
	s->rec.sc_dispatch = dispatch;
	s->sc_next = svc_head;
	svc_head = s;

 pmap_it:
	/* now register the information with the local binder service */
	if (protocol)
		return (pmap_set(prog, vers, protocol, xprt->xp_port));

	return (true);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unregister(u_long prog, u_long vers)
{
	struct svc_callout *prev;
	struct svc_callout *s;

	s = svc_find((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL);
	if (!s)
		return;
	if (prev == NULL)
		svc_head = s->sc_next;
	else
		prev->sc_next = s->sc_next;
	s->sc_next = NULL;
	mem_free(s, sizeof(struct svc_callout));
	/* now unregister the information with the local binder service */
	(void)pmap_unset(prog, vers);
}
#endif				/* PORTMAP */

/*
 * Search the callout list for a program number, return the callout
 * struct.
 */
static struct svc_callout *
svc_find(rpcprog_t prog, rpcvers_t vers,
	 struct svc_callout **prev, char *netid)
{
	struct svc_callout *s, *p;

	assert(prev != NULL);

	p = NULL;
	for (s = svc_head; s != NULL; s = s->sc_next) {
		if (((s->rec.sc_prog == prog) && (s->rec.sc_vers == vers))
		    && ((netid == NULL) || (s->rec.sc_netid == NULL)
			|| (strcmp(netid, s->rec.sc_netid) == 0)))
			break;
		p = s;
	}
	*prev = p;
	return (s);
}

/* An exported search routing similar to svc_find, but with error reporting
 * needed by svc_getreq routines. */
svc_lookup_result_t
svc_lookup(svc_rec_t **rec, svc_vers_range_t *vrange,
	   rpcprog_t prog, rpcvers_t vers, char *netid,
	   u_int flags)
{
	struct svc_callout *s, *p;
	svc_lookup_result_t code = SVC_LKP_ERR;
	bool prog_found, vers_found, netid_found;

	p = NULL;
	prog_found = vers_found = netid_found = false;
	vrange->lowvers = vrange->highvers = 0;

	for (s = svc_head; s != NULL; s = s->sc_next) {
		if (s->rec.sc_prog == prog) {
			prog_found = true;
			/* track supported versions for SVC_LKP_VERS_NOTFOUND */
			if (s->rec.sc_vers > vrange->highvers)
				vrange->highvers = s->rec.sc_vers;
			if (vrange->lowvers < s->rec.sc_vers)
				vrange->lowvers = s->rec.sc_vers;
			/* vers match */
			if (s->rec.sc_vers == vers) {
				vers_found = true;
				/* the following semantics are unchanged */
				if ((netid == NULL) || (s->rec.sc_netid == NULL)
				    || (strcmp(netid, s->rec.sc_netid) == 0)) {
					netid_found = true;
					p = s;
				}	/* netid */
			}	/* vers */
		}		/* prog */
	}			/* for */

	if (p != NULL) {
		*rec = &(p->rec);
		code = SVC_LKP_SUCCESS;
		goto out;
	}

	if (!prog_found) {
		code = SVC_LKP_PROG_NOTFOUND;
		goto out;
	}

	if (!vers_found) {
		code = SVC_LKP_VERS_NOTFOUND;
		goto out;
	}

	if ((netid != NULL) && (!netid_found)) {
		code = SVC_LKP_NETID_NOTFOUND;
		goto out;
	}

 out:
	return (code);
}

/* ******************* REPLY GENERATION ROUTINES  ************ */

/*
 * Send a reply to an rpc request (MT-SAFE).
 *
 * XXX Rather than marshal an rpc_msg on the stack, we may
 * want another variant which takes a message, perhaps one from
 * a reply cache (e.g., CITI Windows NFS client).
 *
 */
bool
svc_sendreply(SVCXPRT *xprt, struct svc_req *req, xdrproc_t xdr_results,
	      void *xdr_location)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = SUCCESS;
	rply.acpted_rply.ar_results.where = xdr_location;
	rply.acpted_rply.ar_results.proc = xdr_results;
	return (SVC_REPLY(xprt, req, &rply));
}

/*
 * No procedure error reply (MT-SAFE)
 */
void
svcerr_noproc(SVCXPRT *xprt, struct svc_req *req)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = PROC_UNAVAIL;
	SVC_REPLY(xprt, req, &rply);
}

/*
 * Can't decode args error reply (MT-SAFE)
 */
void
svcerr_decode(SVCXPRT *xprt, struct svc_req *req)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = GARBAGE_ARGS;
	SVC_REPLY(xprt, req, &rply);
}

/*
 * Some system error (MT-SAFE)
 */
void
svcerr_systemerr(SVCXPRT *xprt, struct svc_req *req)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = SYSTEM_ERR;
	SVC_REPLY(xprt, req, &rply);
}

#if 0
/*
 * Tell RPC package to not complain about version errors to the client.  This
 * is useful when revving broadcast protocols that sit on a fixed address.
 * There is really one (or should be only one) example of this kind of
 * protocol: the portmapper (or rpc binder).
 */
void
__svc_versquiet_on(SVCXPRT *xprt)
{
	u_long tmp;

	tmp = ((u_long) xprt->xp_p3) | SVC_VERSQUIET;
	xprt->xp_p3 = tmp;
}

void
__svc_versquiet_off(SVCXPRT *xprt)
{
	u_long tmp;

	tmp = ((u_long) xprt->xp_p3) & ~SVC_VERSQUIET;
	xprt->xp_p3 = tmp;
}

void svc_versquiet(xprt)
SVCXPRT *xprt;
{
	__svc_versquiet_on(xprt);
}

int
__svc_versquiet_get(SVCXPRT *xprt)
{
	return ((int)xprt->xp_p3) & SVC_VERSQUIET;
}
#endif

/*
 * Authentication error reply (MT-SAFE)
 */
void
svcerr_auth(SVCXPRT *xprt, struct svc_req *req, enum auth_stat why)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_DENIED;
	rply.rm_xid = req->rq_xid;
	rply.rjcted_rply.rj_stat = AUTH_ERROR;
	rply.rjcted_rply.rj_why = why;
	SVC_REPLY(xprt, req, &rply);
}

/*
 * Auth too weak error reply (MT-SAFE)
 */
void
svcerr_weakauth(SVCXPRT *xprt, struct svc_req *req)
{
	assert(xprt != NULL);

	svcerr_auth(xprt, req, AUTH_TOOWEAK);
}

/*
 * Program unavailable error reply (MT-SAFE)
 */
void
svcerr_noprog(SVCXPRT *xprt, struct svc_req *req)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = PROG_UNAVAIL;
	SVC_REPLY(xprt, req, &rply);
}

/*
 * Program version mismatch error reply
 */
void
svcerr_progvers(SVCXPRT *xprt, struct svc_req *req, rpcvers_t low_vers,
		rpcvers_t high_vers)
{
	struct rpc_msg rply;

	assert(xprt != NULL);

	rply.rm_direction = REPLY;
	rply.rm_reply.rp_stat = MSG_ACCEPTED;
	rply.rm_xid = req->rq_xid;
	rply.acpted_rply.ar_verf = req->rq_verf;
	rply.acpted_rply.ar_stat = PROG_MISMATCH;
	rply.acpted_rply.ar_vers.low = (u_int32_t) low_vers;
	rply.acpted_rply.ar_vers.high = (u_int32_t) high_vers;
	SVC_REPLY(xprt, req, &rply);
}

/* ******************* SERVER INPUT STUFF ******************* */

/*
 * Get server side input from some transport.
 */

#if !defined(_WIN32)		/* XXX */
void
svc_getreq(int rdfds)
{
	fd_set readfds;

	FD_ZERO(&readfds);
	readfds.fds_bits[0] = rdfds;
	svc_getreqset(&readfds);
}

void
svc_getreqset(fd_set *readfds)
{
	int bit, fd;
	fd_mask mask, *maskp;
	int sock;

	assert(readfds != NULL);

	maskp = readfds->fds_bits;
	for (sock = 0; sock < FD_SETSIZE; sock += NFDBITS) {
		for (mask = *maskp++; (bit = ffsl(mask)) != 0;
		     mask ^= (1L << (bit - 1))) {
			/* sock has input waiting */
			fd = sock + bit - 1;
			svc_getreq_common(fd);
		}
	}
}

void
svc_getreq_poll(struct pollfd *pfdp, int pollretval)
{
	int i;
	int fds_found;

	for (i = fds_found = 0; fds_found < pollretval; i++) {
		struct pollfd *p = &pfdp[i];

		if (p->revents) {
			/* fd has input waiting */
			fds_found++;
			/*
			 *      We assume that this function is only called
			 *      via someone _select()ing from svc_fdset or
			 *      _poll()ing from svc_pollset[].  Thus it's safe
			 *      to handle the POLLNVAL event by simply turning
			 *      the corresponding bit off in svc_fdset.  The
			 *      svc_pollset[] array is derived from svc_fdset
			 *      and so will also be updated eventually.
			 *
			 *      XXX Should we do an xprt_unregister() instead?
			 */
			if (!(p->revents & POLLNVAL))
				svc_getreq_common(p->fd);
		}
	}
}
#endif				/* _WIN32 */

#if defined(TIRPC_EPOLL)
void
svc_getreqset_epoll(struct epoll_event *events, int nfds)
{
	int ix, code __attribute__ ((unused)) = 0;
	SVCXPRT *xprt;

	assert(events != NULL);

	for (ix = 0; ix < nfds; ++ix) {
		/* XXX should do constant-time validity check */
		xprt = (SVCXPRT *) events[ix].data.ptr;
		code = xprt->xp_ops->xp_getreq(xprt);
	}
}
#endif				/* TIRPC_EPOLL */

void
svc_getreq_common(int fd)
{
	SVCXPRT *xprt;
	bool code __attribute__ ((unused));

	xprt = svc_xprt_get(fd);

	if (xprt == NULL)
		return;

	code = xprt->xp_ops->xp_getreq(xprt);

	return;
}

/* Allow internal or external getreq routines to validate xprt
 * has not been recursively disconnected.  (I don't completely buy the
 * logic, but it should be unchanged (Matt)) */
bool
svc_validate_xprt_list(SVCXPRT *xprt)
{
	bool code;

	code = (xprt == svc_xprt_get(xprt->xp_fd));

	return (code);
}

extern enum auth_stat
svc_auth_authenticate(struct svc_req *, struct rpc_msg *, bool *);

void
svc_dispatch_default(SVCXPRT *xprt, struct rpc_msg **ind_msg)
{
	struct svc_req r;
	struct rpc_msg *msg = *ind_msg;
	svc_vers_range_t vrange;
	svc_lookup_result_t lkp_res;
	svc_rec_t *svc_rec;
	enum auth_stat why;
	bool no_dispatch = false;

	r.rq_xprt = xprt;
	r.rq_prog = msg->rm_call.cb_prog;
	r.rq_vers = msg->rm_call.cb_vers;
	r.rq_proc = msg->rm_call.cb_proc;
	r.rq_cred = msg->rm_call.cb_cred;
	r.rq_xid = msg->rm_xid;

	/* first authenticate the message */
	why = svc_auth_authenticate(&r, msg, &no_dispatch);
	if ((why != AUTH_OK) || no_dispatch) {
		svcerr_auth(xprt, &r, why);
		return;
	}

	lkp_res = svc_lookup(&svc_rec, &vrange, r.rq_prog, r.rq_vers, NULL, 0);
	switch (lkp_res) {
	case SVC_LKP_SUCCESS:
		/* call it */
		(*svc_rec->sc_dispatch) (&r, xprt);
		return;
	case SVC_LKP_VERS_NOTFOUND:
		svcerr_progvers(xprt, &r, vrange.lowvers, vrange.highvers);
		break;
	default:
		svcerr_noprog(xprt, &r);
		break;
	}
}

bool
svc_getreq_default(SVCXPRT *xprt)
{
	enum xprt_stat stat;
	struct svc_req req = {.rq_xprt = xprt };
	bool no_dispatch = false;

	/* XXX !MT-SAFE */

	/* now receive msgs from xprt (support batch calls) */
	do {
		if (SVC_RECV(xprt, &req)) {

			/* now find the exported program and call it */
			svc_vers_range_t vrange;
			svc_lookup_result_t lkp_res;
			svc_rec_t *svc_rec;
			enum auth_stat why;

			/* first authenticate the message */
			why =
			    svc_auth_authenticate(&req, req.rq_msg,
						  &no_dispatch);
			if ((why != AUTH_OK) || no_dispatch) {
				svcerr_auth(xprt, &req, why);
				goto call_done;
			}

			lkp_res =
			    svc_lookup(&svc_rec, &vrange, req.rq_prog,
				       req.rq_vers, NULL, 0);
			switch (lkp_res) {
			case SVC_LKP_SUCCESS:
				(*svc_rec->sc_dispatch) (&req, xprt);
				goto call_done;
				break;
			case SVC_LKP_VERS_NOTFOUND:
				__warnx(TIRPC_DEBUG_FLAG_SVC,
					"%s: dispatch prog vers notfound\n",
					__func__);
				svcerr_progvers(xprt, &req, vrange.lowvers,
						vrange.highvers);
				break;
			default:
				__warnx(TIRPC_DEBUG_FLAG_SVC,
					"%s: dispatch prog notfound\n",
					__func__);
				svcerr_noprog(xprt, &req);
				break;
			}

			/* dispose RPC header */
			free_req_rpc_msg(&req);

		}
		/* SVC_RECV again? */
 call_done:

		/* dispose RPC header */
		free_req_rpc_msg(&req); /* SAFE */

		stat = SVC_STAT(xprt);
		if (stat == XPRT_DIED) {
			__warnx(TIRPC_DEBUG_FLAG_SVC,
				"%s: stat == XPRT_DIED (%p)\n", __func__,
				xprt);
			SVC_DESTROY(xprt);
			SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			xprt = 0;
			break;

		} else {
			/*
			 * Check if the xprt has been disconnected in a
			 * recursive call in the service dispatch routine.
			 * If so, then break.
			 */
			if (!svc_validate_xprt_list(xprt)) {
				SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
				xprt = 0;
				break;
			}
		}

	} while (stat == XPRT_MOREREQS);

	if (xprt) {
		svc_rqst_rearm_events(xprt, SVC_RQST_FLAG_NONE);
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
	}

	return (stat);
}

bool
rpc_control(int what, void *arg)
{
	int val;

	switch (what) {
	case RPC_SVC_CONNMAXREC_SET:
		val = *(int *)arg;
		if (val <= 0)
			return false;
		__svc_maxrec = val;
		break;
	case RPC_SVC_CONNMAXREC_GET:
		*(int *)arg = __svc_maxrec;
		break;
	default:
		return (false);
	}

	return (true);
}

int
svc_shutdown(u_long flags)
{
	int code = 0;

	/* finalize ioq */
	work_pool_shutdown(&svc_work_pool);

	/* dispose all xprts and support */
	svc_xprt_shutdown();

	/* release fd lock records */
	rpc_dplx_shutdown();

	/* release request event channels */
	svc_rqst_shutdown();

	/* XXX assert quiescent */

	return (code);
}
