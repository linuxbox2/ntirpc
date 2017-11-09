/*
 * Copyright (c) 2010, Oracle America, Inc.
 * Copyright (c) 2012-2017 Red Hat, Inc. and/or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the "Oracle America, Inc." nor the names of its
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

#include <config.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <err.h>
#include <rpc/rpc.h>
#include <rpc/nettype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "rpc_com.h"
#include "clnt_internal.h"

int __rpc_raise_fd(int);

#ifndef NETIDLEN
#define NETIDLEN 32
#endif

/* retry timeout default to the moon and back */
static const struct timespec to = { 3, 0 };

/*
 * Generic client creation with version checking the value of
 * vers_out is set to the highest server supported value
 * vers_low <= vers_out <= vers_high  AND an error results
 * if this can not be done.
 *
 * This routine has the same definition as clnt_create_vers(),
 * except it takes an additional timeout parameter - a pointer to
 * a timeval structure.  A NULL value for the pointer indicates
 * that the default timeout value should be used.
 */
CLIENT *
clnt_ncreate_vers_timed(const char *hostname, rpcprog_t prog,
			rpcvers_t *vers_out, rpcvers_t vers_low,
			rpcvers_t vers_high, const char *nettype,
			const struct timeval *tp)
{
	CLIENT *clnt;
	struct clnt_req *cc;
	AUTH *auth;
	struct rpc_err rpcerr;
	enum clnt_stat rpc_stat;

	clnt = clnt_ncreate_timed(hostname, prog, vers_high, nettype, tp);
	if (clnt == NULL)
		return (NULL);

	auth = authnone_ncreate();	/* idempotent */
	cc = mem_alloc(sizeof(*cc));
	clnt_req_fill(cc, clnt, auth, NULLPROC,
		      (xdrproc_t) xdr_void, NULL,
		      (xdrproc_t) xdr_void, NULL);
	if (!clnt_req_setup(cc, to)) {
		rpc_stat = RPC_TLIERROR;
		clnt_req_release(cc);
		goto geterror;
	}

	rpc_stat = CLNT_CALL(cc);
	clnt_req_release(cc);
	if (rpc_stat == RPC_SUCCESS) {
		*vers_out = vers_high;
		return (clnt);
	}

	while (rpc_stat == RPC_PROGVERSMISMATCH && vers_high > vers_low) {
		unsigned int minvers, maxvers;

		clnt_geterr(clnt, &rpcerr);
		minvers = rpcerr.re_vers.low;
		maxvers = rpcerr.re_vers.high;
		if (maxvers < vers_high)
			vers_high = maxvers;
		else
			vers_high--;
		if (minvers > vers_low)
			vers_low = minvers;
		if (vers_low > vers_high)
			goto error;
		CLNT_CONTROL(clnt, CLSET_VERS, (char *)&vers_high);

		cc = mem_alloc(sizeof(*cc));
		clnt_req_fill(cc, clnt, auth, NULLPROC,
			      (xdrproc_t) xdr_void, NULL,
			      (xdrproc_t) xdr_void, NULL);
		if (!clnt_req_setup(cc, to)) {
			rpc_stat = RPC_TLIERROR;
			clnt_req_release(cc);
			goto geterror;
		}

		rpc_stat = CLNT_CALL(cc);
		clnt_req_release(cc);
		if (rpc_stat == RPC_SUCCESS) {
			*vers_out = vers_high;
			return (clnt);
		}
	}
 geterror:
	clnt_geterr(clnt, &rpcerr);

 error:
	rpc_createerr.cf_stat = rpc_stat;
	rpc_createerr.cf_error = rpcerr;
	clnt_destroy(clnt);
	return (NULL);
}

/*
 * Top level client creation routine.
 * Generic client creation: takes (servers name, program-number, nettype) and
 * returns client handle. Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s.
 *
 * It tries for all the netids in that particular class of netid until
 * it succeeds.
 * XXX The error message in the case of failure will be the one
 * pertaining to the last create error.
 *
 * This routine has the same definition as clnt_create(),
 * except it takes an additional timeout parameter - a pointer to
 * a timeval structure.  A NULL value for the pointer indicates
 * that the default timeout value should be used.
 *
 * This function calls clnt_tp_create_timed().
 */
CLIENT *
clnt_ncreate_timed(const char *hostname, rpcprog_t prog, rpcvers_t vers,
		   const char *netclass, const struct timeval *tp)
{
	struct netconfig *nconf;
	CLIENT *clnt = NULL;
	void *handle;
	enum clnt_stat save_cf_stat = RPC_SUCCESS;
	struct rpc_err save_cf_error;
	char nettype_array[NETIDLEN];
	char *nettype = &nettype_array[0];

	if (netclass == NULL)
		nettype = NULL;
	else {
		size_t len = strlen(netclass);
		if (len >= sizeof(nettype_array)) {
			rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			return (NULL);
		}
		strcpy(nettype, netclass);
	}

	handle = __rpc_setconf((char *)nettype);
	if (handle == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (NULL);
	}
	rpc_createerr.cf_stat = RPC_SUCCESS;
	while (clnt == NULL) {
		nconf = __rpc_getconf(handle);
		if (nconf == NULL) {
			if (rpc_createerr.cf_stat == RPC_SUCCESS)
				rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			break;
		}
#ifdef CLNT_DEBUG
		__warnx(TIRPC_DEBUG_FLAG_CLNT_GEN, "%s: trying netid %s",
			__func__, nconf->nc_netid);
#endif
		clnt = clnt_tp_ncreate_timed(hostname, prog, vers, nconf, tp);
		if (clnt)
			break;
		else {
			/*
			 * Since we didn't get a name-to-address
			 * translation failure here, we remember
			 * this particular error.  The object of
			 * this is to enable us to return to the
			 * caller a more-specific error than the
			 * unhelpful ``Name to address translation
			 * failed'' which might well occur if we
			 * merely returned the last error (because
			 * the local loopbacks are typically the
			 * last ones in /etc/netconfig and the most
			 * likely to be unable to translate a host
			 * name).  We also check for a more
			 * meaningful error than ``unknown host
			 * name'' for the same reasons.
			 */
			if (rpc_createerr.cf_stat != RPC_N2AXLATEFAILURE
			    && rpc_createerr.cf_stat != RPC_UNKNOWNHOST) {
				save_cf_stat = rpc_createerr.cf_stat;
				save_cf_error = rpc_createerr.cf_error;
			}
		}
	}

	/*
	 * Attempt to return an error more specific than ``Name to address
	 * translation failed'' or ``unknown host name''
	 */
	if ((rpc_createerr.cf_stat == RPC_N2AXLATEFAILURE
	     || rpc_createerr.cf_stat == RPC_UNKNOWNHOST)
	    && (save_cf_stat != RPC_SUCCESS)) {
		rpc_createerr.cf_stat = save_cf_stat;
		rpc_createerr.cf_error = save_cf_error;
	}
	__rpc_endconf(handle);
	return (clnt);
}

/*
 * Generic client creation: takes (servers name, program-number, netconf) and
 * returns client handle. Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s : clnt_control()
 * It finds out the server address from rpcbind and calls clnt_tli_create().
 *
 * This has the same definition as clnt_tp_ncreate(), except it
 * takes an additional parameter - a pointer to a timeval structure.
 * A NULL value for the timeout pointer indicates that the default
 * value for the timeout should be used.
 */
CLIENT *
clnt_tp_ncreate_timed(const char *hostname, rpcprog_t prog,
		      rpcvers_t vers, const struct netconfig *nconf,
		      const struct timeval *tp)
{
	struct netbuf *svcaddr;	/* servers address */
	CLIENT *cl = NULL;	/* client handle */

	if (nconf == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (NULL);
	}

	/*
	 * Get the address of the server
	 */
	svcaddr =
		__rpcb_findaddr_timed(prog, vers, (struct netconfig *)nconf,
				      (char *)hostname, &cl,
				      (struct timeval *)tp);
	if (svcaddr == NULL) {
		/* appropriate error number is set by rpcbind libraries */
		return (NULL);
	}
	if (cl == NULL) {
		/* __rpc_findaddr_timed failed? */
		cl = clnt_tli_ncreate(RPC_ANYFD, nconf, svcaddr, prog, vers, 0,
				      0);
	} else {
		/* Reuse the CLIENT handle and change the appropriate fields */
		if (CLNT_CONTROL(cl, CLSET_SVC_ADDR, (void *)svcaddr) == true) {
			if (cl->cl_netid == NULL)
				cl->cl_netid = mem_strdup(nconf->nc_netid);
			if (cl->cl_tp == NULL)
				cl->cl_tp = mem_strdup(nconf->nc_device);
			(void)CLNT_CONTROL(cl, CLSET_PROG, (void *)&prog);
			(void)CLNT_CONTROL(cl, CLSET_VERS, (void *)&vers);
		} else {
			CLNT_DESTROY(cl);
			cl = clnt_tli_ncreate(RPC_ANYFD, nconf, svcaddr, prog,
					      vers, 0, 0);
		}
	}
	mem_free(svcaddr->buf, sizeof(*svcaddr->buf));
	mem_free(svcaddr, sizeof(*svcaddr));
	return (cl);
}

/*
 * Generic client creation:  returns client handle.
 * Default options are set, which the user can
 * change using the rpc equivalent of _ioctl()'s : clnt_control().
 * If fd is RPC_ANYFD, it will be opened using nconf.
 * It will be bound if not so.
 * If sizes are 0; appropriate defaults will be chosen.
 */
CLIENT *
clnt_tli_ncreate(int fd, const struct netconfig *nconf,
		 struct netbuf *svcaddr, rpcprog_t prog,
		 rpcvers_t vers, u_int sendsz, u_int recvsz)
{
	CLIENT *cl;		/* client handle */
	bool madefd = false;	/* whether fd opened here */
	long servtype;
	int one = 1;
	struct __rpc_sockinfo si;
	extern int __rpc_minfd;

	if (fd == RPC_ANYFD) {
		if (nconf == NULL) {
			rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			return (NULL);
		}

		fd = __rpc_nconf2fd(nconf);

		if (fd == -1)
			goto err;
		if (fd < __rpc_minfd)
			fd = __rpc_raise_fd(fd);
		madefd = true;
		servtype = nconf->nc_semantics;
		if (!__rpc_fd2sockinfo(fd, &si))
			goto err;
		bindresvport(fd, NULL);
	} else {
		if (!__rpc_fd2sockinfo(fd, &si))
			goto err;
		servtype = __rpc_socktype2seman(si.si_socktype);
		if (servtype == -1) {
			rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			return (NULL);
		}
	}

	if (si.si_af != ((struct sockaddr *)svcaddr->buf)->sa_family) {
		rpc_createerr.cf_stat = RPC_UNKNOWNHOST;	/* XXX */
		goto err1;
	}

	switch (servtype) {
	case NC_TPI_COTS:
		cl = clnt_vc_ncreate(fd, svcaddr, prog, vers, sendsz, recvsz);
		break;
	case NC_TPI_COTS_ORD:
		if (nconf && ((strcmp(nconf->nc_protofmly, "inet") == 0)
			      || (strcmp(nconf->nc_protofmly, "inet6") == 0))) {
			(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
					  sizeof(one));
		}
		cl = clnt_vc_ncreate(fd, svcaddr, prog, vers, sendsz, recvsz);
		break;
	case NC_TPI_CLTS:
		cl = clnt_dg_ncreate(fd, svcaddr, prog, vers, sendsz, recvsz);
		break;
	default:
		goto err;
	}

	if (cl == NULL)
		goto err1;	/* borrow errors from clnt_dg/vc ncreates */
	if (nconf) {
		cl->cl_netid = mem_strdup(nconf->nc_netid);
		cl->cl_tp = mem_strdup(nconf->nc_device);
	} else {
		cl->cl_netid = "";
		cl->cl_tp = "";
	}
	if (madefd) {
		(void)CLNT_CONTROL(cl, CLSET_FD_CLOSE, NULL);
/*  (void) CLNT_CONTROL(cl, CLSET_POP_TIMOD, NULL);  */
	};

	return (cl);

 err:
	rpc_createerr.cf_stat = RPC_SYSTEMERROR;
	rpc_createerr.cf_error.re_errno = errno;
 err1:	if (madefd)
		(void)close(fd);
	return (NULL);
}

int
clnt_req_xid_cmpf(const struct opr_rbtree_node *lhs,
		  const struct opr_rbtree_node *rhs)
{
	struct clnt_req *lk, *rk;

	lk = opr_containerof(lhs, struct clnt_req, cc_node);
	rk = opr_containerof(rhs, struct clnt_req, cc_node);

	if (lk->cc_xid < rk->cc_xid)
		return (-1);

	if (lk->cc_xid == rk->cc_xid)
		return (0);

	return (1);
}

bool
clnt_req_setup(struct clnt_req *cc, struct timespec timeout)
{
	CLIENT *clnt = cc->cc_clnt;
	struct cx_data *cx = CX_DATA(clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct opr_rbtree_node *nv;

	rpc_msg_init(&cc->cc_msg);

	/* protects this */
	mutex_init(&cc->cc_we.mtx, NULL);
	mutex_lock(&cc->cc_we.mtx);
	cond_init(&cc->cc_we.cv, 0, NULL);

	cc->cc_error.re_errno = 0;
	cc->cc_error.re_status = RPC_SUCCESS;
	cc->cc_flags = CLNT_REQ_FLAG_NONE;
	cc->cc_refcount = 1;
	cc->cc_refreshes = 2;
	cc->cc_timeout = timeout;

	if (timeout.tv_nsec < 0 || timeout.tv_nsec > 999999999
	 || timeout.tv_sec < 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d bad timeout (%ld.%09ld)",
			__func__, &rec->xprt, rec->xprt.xp_fd,
			timeout.tv_sec, timeout.tv_nsec);
		return (false);
	}
	if (timeout.tv_sec > 10) {
		__warnx(TIRPC_DEBUG_FLAG_WARN,
			"%s: tv_sec %ld > 10",
			__func__, timeout.tv_sec);
	}

	/* this lock protects both xid and rbtree */
	rpc_dplx_rli(rec);
	cc->cc_xid = ++(rec->call_xid);
	nv = opr_rbtree_insert(&rec->call_replies, &cc->cc_node);
	rpc_dplx_rui(rec);
	if (nv) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d insert failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid);
		return (false);
	}

	return (true);
}

/*
 * unlocked
 */
enum xprt_stat
clnt_req_process_reply(SVCXPRT *xprt, struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;
	struct rpc_dplx_rec *rec = REC_XPRT(xprt);
	struct opr_rbtree_node *nv;
	struct clnt_req *cc;
	struct clnt_req cc_k;

	rpc_dplx_rli(rec);
	cc_k.cc_xid = req->rq_msg.rm_xid;
	nv = opr_rbtree_lookup(&rec->call_replies, &cc_k.cc_node);
	rpc_dplx_rui(rec);
	if (!nv) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: %p fd %d lookup failed xid %" PRIu32,
			__func__, &rec->xprt, rec->xprt.xp_fd, cc_k.cc_xid);
		return SVC_STAT(xprt);
	}

	cc = opr_containerof(nv, struct clnt_req, cc_node);
	atomic_set_uint16_t_bits(&cc->cc_flags, CLNT_REQ_FLAG_ACKSYNC);

	_seterr_reply(&req->rq_msg, &(cc->cc_error));
	if (cc->cc_error.re_status == RPC_SUCCESS) {
		if (!AUTH_VALIDATE(cc->cc_auth,
				   &(cc->cc_msg.RPCM_ack.ar_verf))) {
			cc->cc_error.re_status = RPC_AUTHERROR;
			cc->cc_error.re_why = AUTH_INVALIDRESP;
		} else if (cc->cc_msg.rm_xdr.proc
			   && !AUTH_UNWRAP(cc->cc_auth, xdrs,
					   cc->cc_msg.rm_xdr.proc,
					   cc->cc_msg.rm_xdr.where)) {
			if (cc->cc_error.re_status == RPC_SUCCESS)
				cc->cc_error.re_status = RPC_CANTDECODERES;
		}
		cc->cc_refreshes = 0;
	} else if (cc->cc_refreshes-- > 0
		   && cc->cc_error.re_status == RPC_AUTHERROR
		   && AUTH_REFRESH(cc->cc_auth, &(cc->cc_msg))) {
		/* maybe our credentials need to be refreshed ... */
		rpc_dplx_rli(rec);
		opr_rbtree_remove(&rec->call_replies, &cc->cc_node);
		cc->cc_xid = ++(rec->call_xid);
		nv = opr_rbtree_insert(&rec->call_replies, &cc->cc_node);
		rpc_dplx_rui(rec);
		if (nv) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s: %p fd %d insert failed xid %" PRIu32,
				__func__, xprt, xprt->xp_fd, cc->cc_xid);
			cc->cc_error.re_status = RPC_TLIERROR;
			cc->cc_refreshes = 0;
		}
	}

	mutex_lock(&cc->cc_we.mtx);
	cond_signal(&cc->cc_we.cv);
	mutex_unlock(&cc->cc_we.mtx);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d acknowledged xid %" PRIu32,
		__func__, xprt, xprt->xp_fd, cc->cc_xid);

	return SVC_STAT(xprt);
}

/*
 * waitq_entry is locked in clnt_req_setup()
 */
int
clnt_req_wait_reply(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);
	struct rpc_dplx_rec *rec = cx->cx_rec;
	struct timespec ts;
	int code;

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d xid %" PRIu32 " (%ld.%09ld)",
		__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid,
		cc->cc_timeout.tv_sec, cc->cc_timeout.tv_nsec);

	if (!(cc->cc_timeout.tv_sec + cc->cc_timeout.tv_nsec)) {
		return (RPC_SUCCESS);
	}

	(void)clock_gettime(CLOCK_REALTIME_FAST, &ts);
	timespecadd(&ts, &cc->cc_timeout);
	code = cond_timedwait(&cc->cc_we.cv, &cc->cc_we.mtx, &ts);

	__warnx(TIRPC_DEBUG_FLAG_CLNT_REQ,
		"%s: %p fd %d replied xid %" PRIu32,
		__func__, &rec->xprt, rec->xprt.xp_fd, cc->cc_xid);

	if (!(atomic_fetch_uint16_t(&cc->cc_flags) & CLNT_REQ_FLAG_ACKSYNC)
	 && (code == ETIMEDOUT)) {
		if (rec->xprt.xp_flags & SVC_XPRT_FLAG_DESTROYED) {
			/* XXX should also set error.re_why, but the
			 * facility is not well developed. */
			cc->cc_error.re_status = RPC_TIMEDOUT;
		}
	}

	return (code);
}

void
clnt_req_release(struct clnt_req *cc)
{
	struct cx_data *cx = CX_DATA(cc->cc_clnt);

	if (atomic_dec_uint32_t(&cc->cc_refcount))
		return;

	rpc_dplx_rli(cx->cx_rec);
	opr_rbtree_remove(&cx->cx_rec->call_replies, &cc->cc_node);
	rpc_dplx_rui(cx->cx_rec);
	mutex_unlock(&cc->cc_we.mtx);
	mutex_destroy(&cc->cc_we.mtx);
	cond_destroy(&cc->cc_we.cv);
	mem_free(cc, sizeof(*cc));
}

/*
 *  To avoid conflicts with the "magic" file descriptors (0, 1, and 2),
 *  we try to not use them.  The __rpc_raise_fd() routine will dup
 *  a descriptor to a higher value.  If we fail to do it, we continue
 *  to use the old one (and hope for the best).
 */
int __rpc_minfd = 3;

int __rpc_raise_fd(int fd)
{
	int nfd;

	if (fd >= __rpc_minfd)
		return (fd);

	nfd = fcntl(fd, F_DUPFD, __rpc_minfd);
	if (nfd == -1)
		return (fd);

	if (fsync(nfd) == -1) {
		close(nfd);
		return (fd);
	}

	if (close(fd) == -1) {
		/* this is okay, we will log an error, then use the new fd */
		__warnx(TIRPC_DEBUG_FLAG_CLNT_GEN,
			"could not close() fd %d; mem & fd leak", fd);
	}

	return (nfd);
}
