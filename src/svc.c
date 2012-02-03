
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

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h>
#endif
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"

#include <rpc/svc.h>

#include "clnt_internal.h"
#include "svc_xprt.h"

#define	RQCRED_SIZE	400	/* this size is excessive */

#define SVC_VERSQUIET 0x0001	/* keep quiet about vers mismatch */
#define version_keepquiet(xp) ((u_long)(xp)->xp_p3 & SVC_VERSQUIET)

#define max(a, b) (a > b ? a : b)

extern tirpc_pkg_params __pkg_params;
svc_params __svc_params[1];

/*
 * The services list
 * Each entry represents a set of procedures (an rpc program).
 * The dispatch routine takes request structs and runs the
 * apropriate procedure.
 *
 * The service record is factored out to permit exporting the find
 * routines without exposing the db implementation.
 */
static struct svc_callout
{
    struct svc_callout *sc_next;
    struct svc_record rec;
} *svc_head;

extern rwlock_t svc_lock;
extern rwlock_t svc_fd_lock;

static struct svc_callout *svc_find (rpcprog_t, rpcvers_t,
				     struct svc_callout **, char *);
static void __xprt_do_unregister (SVCXPRT * xprt, bool_t dolock);

/* Package init function.
 * It is intended that applications which must make use of global state
 * will call svc_init() before accessing such state and before executing
 * any svc exported functions.   Traditional TI-RPC programs need not
 * call the function as presently integrated. */
void
svc_init (svc_init_params * params)
{
    __svc_params->max_connections = FD_SETSIZE;

    if (params->flags & SVC_INIT_WARNX)
        __pkg_params.warnx = params->warnx;
    else
        __pkg_params.warnx = warnx;

#if defined(TIRPC_EPOLL)
    if (params->flags & SVC_INIT_EPOLL) {
        __svc_params->ev_type = SVC_EVENT_EPOLL;
        __svc_params->max_connections = params->max_connections; /* XXX going? */
        __svc_params->ev_u.epoll.max_events = params->max_events;
        __svc_params->ev_u.epoll.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (__svc_params->ev_u.epoll.epoll_fd == -1) {
            warnx("svc_init:  epoll_create failed");
            return;
        }
    }
#else
    /* XXX formerly select/fd_set case, now placeholder for new
     * event systems, reworked select, etc. */
#endif

    return;
}

struct rpc_msg *alloc_rpc_msg(void)
{
    /* XXX pool allocators */
    struct rpc_msg *msg = mem_alloc(sizeof(struct rpc_msg));
    if (! msg)
        goto out;
    msg->rm_call.cb_cred.oa_base = mem_alloc(2 * MAX_AUTH_BYTES + RQCRED_SIZE);
    if (! msg->rm_call.cb_cred.oa_base) {
        mem_free(msg, sizeof(struct rpc_msg));
        msg = NULL;
        goto out;
    }
    msg->rm_call.cb_verf.oa_base =
        msg->rm_call.cb_cred.oa_base + MAX_AUTH_BYTES;
out:
    return (msg);
}

void free_rpc_msg(struct rpc_msg *msg)
{
    mem_free(msg->rm_call.cb_cred.oa_base, 2 * MAX_AUTH_BYTES + RQCRED_SIZE);
    mem_free(msg, sizeof(struct rpc_msg));
}


/* ***************  SVCXPRT related stuff **************** */

/*
 * This is used to set xprt->xp_raddr in a way legacy
 * apps can deal with
 */
void
__xprt_set_raddr(SVCXPRT *xprt, const struct sockaddr_storage *ss)
{
	switch (ss->ss_family) {
	case AF_INET6:
		memcpy(&xprt->xp_raddr, ss, sizeof(struct sockaddr_in6));
		xprt->xp_addrlen = sizeof (struct sockaddr_in6);
		break;
	case AF_INET:
		memcpy(&xprt->xp_raddr, ss, sizeof(struct sockaddr_in));
		xprt->xp_addrlen = sizeof (struct sockaddr_in);
		break;
	default:
		xprt->xp_raddr.sin6_family = AF_UNSPEC;
		xprt->xp_addrlen = sizeof (struct sockaddr);
		break;
	}
}

/*
 * Activate a transport handle.
 */
void
xprt_register (SVCXPRT * xprt)
{
    int code;

    assert (xprt != NULL);

    rwlock_wrlock (&svc_fd_lock); /* XXX protecting event registration */

    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        /* set up epoll user data */
        ((struct epoll_event *) xprt->xp_ev)->data.fd = xprt->xp_fd;
        /* wait for read events, level triggered */
        ((struct epoll_event *) xprt->xp_ev)->events = EPOLLIN;
        /* add to epoll vector */
        assert(xprt->xp_ev);
        code = epoll_ctl(__svc_params->ev_u.epoll.epoll_fd,
                         EPOLL_CTL_ADD,
                         xprt->xp_fd,
                         (struct epoll_event *) xprt->xp_ev);
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        break;
    } /* switch */

    rwlock_unlock (&svc_fd_lock);

} /* xprt_register */

void
xprt_unregister (SVCXPRT * xprt)
{
  __xprt_do_unregister (xprt, TRUE);
}

void
__xprt_unregister_unlocked (SVCXPRT * xprt)
{
  __xprt_do_unregister (xprt, FALSE);
}

/*
 * De-activate a transport handle.
 */
static void
__xprt_do_unregister (SVCXPRT *xprt, bool_t dolock)
{
    int code = 0;
    SVCXPRT *xprt2;

    assert (xprt != NULL);

    if (dolock)
        rwlock_wrlock (&svc_fd_lock); /* XXX protect registration */

    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        assert(xprt->xp_ev);
        code = epoll_ctl(__svc_params->ev_u.epoll.epoll_fd,
                         EPOLL_CTL_DEL,
                         xprt->xp_fd,
                         (struct epoll_event *) xprt->xp_ev);
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        break;
    }

    if (dolock)
        rwlock_unlock (&svc_fd_lock);

    /* xprt2 holds the address we displaced, it would be of interest
     * if xprt2 != xprt */
    xprt2 = svc_xprt_clear(xprt);

}

/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool_t
svc_reg (xprt, prog, vers, dispatch, nconf)
     SVCXPRT *xprt;
     const rpcprog_t prog;
     const rpcvers_t vers;
     void (*dispatch) (struct svc_req *, SVCXPRT *);
     const struct netconfig *nconf;
{
  bool_t dummy;
  struct svc_callout *prev;
  struct svc_callout *s;
  struct netconfig *tnconf;
  char *netid = NULL;
  int flag = 0;

/* VARIABLES PROTECTED BY svc_lock: s, prev, svc_head */
  if (xprt->xp_netid)
    {
      netid = strdup (xprt->xp_netid);
      flag = 1;
    }
  else if (nconf && nconf->nc_netid)
    {
      netid = strdup (nconf->nc_netid);
      flag = 1;
    }
  else if ((tnconf = __rpcgettp (xprt->xp_fd)) != NULL)
    {
      netid = strdup (tnconf->nc_netid);
      flag = 1;
      freenetconfigent (tnconf);
    }				/* must have been created with svc_raw_create */
  if ((netid == NULL) && (flag == 1))
    {
      return (FALSE);
    }

  rwlock_wrlock (&svc_lock);
  if ((s = svc_find (prog, vers, &prev, netid)) != NULL)
    {
      if (netid)
	free (netid);
      if (s->rec.sc_dispatch == dispatch)
	goto rpcb_it;		/* he is registering another xptr */
      rwlock_unlock (&svc_lock);
      return (FALSE);
    }
  s = mem_alloc (sizeof (struct svc_callout));
  if (s == NULL)
    {
      if (netid)
	free (netid);
      rwlock_unlock (&svc_lock);
      return (FALSE);
    }

  s->rec.sc_prog = prog;
  s->rec.sc_vers = vers;
  s->rec.sc_dispatch = dispatch;
  s->rec.sc_netid = netid;
  s->sc_next = svc_head;
  svc_head = s;

  if ((xprt->xp_netid == NULL) && (flag == 1) && netid)
    ((SVCXPRT *) xprt)->xp_netid = strdup (netid);

rpcb_it:
  rwlock_unlock (&svc_lock);
  /* now register the information with the local binder service */
  if (nconf)
    {
      /*LINTED const castaway */
      dummy = rpcb_set (prog, vers, (struct netconfig *) nconf,
			&((SVCXPRT *) xprt)->xp_ltaddr);
      return (dummy);
    }
  return (TRUE);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unreg (prog, vers)
     const rpcprog_t prog;
     const rpcvers_t vers;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  /* unregister the information anyway */
  (void) rpcb_unset (prog, vers, NULL);
  rwlock_wrlock (&svc_lock);
  while ((s = svc_find (prog, vers, &prev, NULL)) != NULL)
    {
      if (prev == NULL)
	{
	  svc_head = s->sc_next;
	}
      else
	{
	  prev->sc_next = s->sc_next;
	}
      s->sc_next = NULL;
      if (s->rec.sc_netid)
	mem_free (s->rec.sc_netid, sizeof (s->rec.sc_netid) + 1);
      mem_free (s, sizeof (struct svc_callout));
    }
  rwlock_unlock (&svc_lock);
}

/* ********************** CALLOUT list related stuff ************* */

#ifdef PORTMAP
/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool_t
svc_register (xprt, prog, vers, dispatch, protocol)
     SVCXPRT *xprt;
     u_long prog;
     u_long vers;
     void (*dispatch) (struct svc_req *, SVCXPRT *);
     int protocol;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  assert (xprt != NULL);
  assert (dispatch != NULL);

  if ((s = svc_find ((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL)) !=
      NULL)
    {
      if (s->rec.sc_dispatch == dispatch)
	goto pmap_it;		/* he is registering another xprt */
      return (FALSE);
    }
  s = mem_alloc (sizeof (struct svc_callout));
  if (s == NULL)
    {
      return (FALSE);
    }
  s->rec.sc_prog = (rpcprog_t) prog;
  s->rec.sc_vers = (rpcvers_t) vers;
  s->rec.sc_dispatch = dispatch;
  s->sc_next = svc_head;
  svc_head = s;
pmap_it:
  /* now register the information with the local binder service */
  if (protocol)
    {
      return (pmap_set (prog, vers, protocol, xprt->xp_port));
    }
  return (TRUE);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unregister (prog, vers)
     u_long prog;
     u_long vers;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  if ((s = svc_find ((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL)) ==
      NULL)
    return;
  if (prev == NULL)
    {
      svc_head = s->sc_next;
    }
  else
    {
      prev->sc_next = s->sc_next;
    }
  s->sc_next = NULL;
  mem_free (s, sizeof (struct svc_callout));
  /* now unregister the information with the local binder service */
  (void) pmap_unset (prog, vers);
}
#endif /* PORTMAP */

/*
 * Search the callout list for a program number, return the callout
 * struct.
 */
static struct svc_callout *
svc_find (prog, vers, prev, netid)
     rpcprog_t prog;
     rpcvers_t vers;
     struct svc_callout **prev;
     char *netid;
{
  struct svc_callout *s, *p;

  assert (prev != NULL);

  p = NULL;
  for (s = svc_head; s != NULL; s = s->sc_next)
    {
      if (((s->rec.sc_prog == prog) && (s->rec.sc_vers == vers)) &&
	  ((netid == NULL) || (s->rec.sc_netid == NULL) ||
	   (strcmp (netid, s->rec.sc_netid) == 0)))
	break;
      p = s;
    }
  *prev = p;
  return (s);
}

/* An exported search routing similar to svc_find, but with error reporting
 * needed by svc_getreq routines. */
svc_lookup_result_t
svc_lookup(svc_rec_t **rec, svc_vers_range_t *vrange, rpcprog_t prog,
           rpcvers_t vers, char *netid, u_int flags)
{
    struct svc_callout *s, *p;
    svc_lookup_result_t code = SVC_LKP_ERR;
    bool_t prog_found, vers_found, netid_found;

    p = NULL;
    prog_found = vers_found = netid_found = FALSE;
    vrange->lowvers = vrange->highvers = 0;

    for (s = svc_head; s != NULL; s = s->sc_next) {
        if (s->rec.sc_prog == prog) {
            prog_found = TRUE;
            /* track supported versions for SVC_LKP_VERS_NOTFOUND */
            if (s->rec.sc_vers > vrange->highvers)
                vrange->highvers = s->rec.sc_vers;
            if (vrange->lowvers < s->rec.sc_vers)
                vrange->lowvers = s->rec.sc_vers;
            /* vers match*/
            if (s->rec.sc_vers == vers) {
                vers_found = TRUE;
                /* the following semantics are unchanged */
                if ((netid == NULL) || (s->rec.sc_netid == NULL) ||
                    (strcmp (netid, s->rec.sc_netid) == 0)) {
                    netid_found = TRUE;
                    p = s;
                } /* netid */
            } /* vers */
        } /* prog */
    } /* for */

    if (p != NULL) {
        *rec = &(p->rec);
        code = SVC_LKP_SUCCESS;
        goto out;
    }

    if (! prog_found) {
        code = SVC_LKP_PROG_NOTFOUND;
        goto out;
    }

    if (! vers_found){
        code = SVC_LKP_VERS_NOTFOUND;
        goto out;
    }

    if ((netid != NULL) && (! netid_found)) {
        code = SVC_LKP_NETID_NOTFOUND;
        goto out;
    }
    
out:
    return (code);
}


/* ******************* REPLY GENERATION ROUTINES  ************ */

/*
 * Send a reply to an rpc request
 */
bool_t
svc_sendreply (xprt, xdr_results, xdr_location)
     SVCXPRT *xprt;
     xdrproc_t xdr_results;
     void *xdr_location;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = SUCCESS;
  rply.acpted_rply.ar_results.where = xdr_location;
  rply.acpted_rply.ar_results.proc = xdr_results;
  return (SVC_REPLY (xprt, &rply));
}

/*
 * No procedure error reply
 */
void
svcerr_noproc (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROC_UNAVAIL;
  SVC_REPLY (xprt, &rply);
}

/*
 * Can't decode args error reply
 */
void
svcerr_decode (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = GARBAGE_ARGS;
  SVC_REPLY (xprt, &rply);
}

/*
 * Some system error
 */
void
svcerr_systemerr (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = SYSTEM_ERR;
  SVC_REPLY (xprt, &rply);
}

#if 0
/*
 * Tell RPC package to not complain about version errors to the client.	 This
 * is useful when revving broadcast protocols that sit on a fixed address.
 * There is really one (or should be only one) example of this kind of
 * protocol: the portmapper (or rpc binder).
 */
void
__svc_versquiet_on (xprt)
     SVCXPRT *xprt;
{
  u_long tmp;

  tmp = ((u_long) xprt->xp_p3) | SVC_VERSQUIET;
  xprt->xp_p3 = tmp;
}

void
__svc_versquiet_off (xprt)
     SVCXPRT *xprt;
{
  u_long tmp;

  tmp = ((u_long) xprt->xp_p3) & ~SVC_VERSQUIET;
  xprt->xp_p3 = tmp;
}

void
svc_versquiet (xprt)
     SVCXPRT *xprt;
{
  __svc_versquiet_on (xprt);
}

int
__svc_versquiet_get (xprt)
     SVCXPRT *xprt;
{
  return ((int) xprt->xp_p3) & SVC_VERSQUIET;
}
#endif

/*
 * Authentication error reply
 */
void
svcerr_auth (xprt, why)
     SVCXPRT *xprt;
     enum auth_stat why;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_DENIED;
  rply.rjcted_rply.rj_stat = AUTH_ERROR;
  rply.rjcted_rply.rj_why = why;
  SVC_REPLY (xprt, &rply);
}

/*
 * Auth too weak error reply
 */
void
svcerr_weakauth (xprt)
     SVCXPRT *xprt;
{

  assert (xprt != NULL);

  svcerr_auth (xprt, AUTH_TOOWEAK);
}

/*
 * Program unavailable error reply
 */
void
svcerr_noprog (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROG_UNAVAIL;
  SVC_REPLY (xprt, &rply);
}

/*
 * Program version mismatch error reply
 */
void
svcerr_progvers (xprt, low_vers, high_vers)
     SVCXPRT *xprt;
     rpcvers_t low_vers;
     rpcvers_t high_vers;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROG_MISMATCH;
  rply.acpted_rply.ar_vers.low = (u_int32_t) low_vers;
  rply.acpted_rply.ar_vers.high = (u_int32_t) high_vers;
  SVC_REPLY (xprt, &rply);
}

/* ******************* SERVER INPUT STUFF ******************* */

/*
 * Get server side input from some transport.
 *
 * Statement of authentication parameters management:
 * This function owns and manages all authentication parameters, specifically
 * the "raw" parameters (msg.rm_call.cb_cred and msg.rm_call.cb_verf) and
 * the "cooked" credentials (rqst->rq_clntcred).
 * However, this function does not know the structure of the cooked
 * credentials, so it make the following assumptions:
 *   a) the structure is contiguous (no pointers), and
 *   b) the cred structure size does not exceed RQCRED_SIZE bytes.
 * In all events, all three parameters are freed upon exit from this routine.
 * The storage is trivially management on the call stack in user land, but
 * is mallocated in kernel land.
 */

void
svc_getreq (rdfds)
     int rdfds;
{
  fd_set readfds;

  FD_ZERO (&readfds);
  readfds.fds_bits[0] = rdfds;
  svc_getreqset (&readfds);
}

void
svc_getreqset (readfds)
     fd_set *readfds;
{
  int bit, fd;
  fd_mask mask, *maskp;
  int sock;

  assert (readfds != NULL);

  maskp = readfds->fds_bits;
  for (sock = 0; sock < FD_SETSIZE; sock += NFDBITS)
    {
      for (mask = *maskp++; (bit = ffsl(mask)) != 0; mask ^= (1L << (bit - 1)))
	{
	  /* sock has input waiting */
	  fd = sock + bit - 1;
	  svc_getreq_common (fd);
	}
    }
}

#if defined(TIRPC_EPOLL)
void
svc_getreqset_epoll (struct epoll_event *events, int nfds)
{
  int ix;

  assert (events != NULL);

  for (ix = 0; ix < nfds; ++ix) {
        svc_getreq_common (events[ix].data.fd);
  }

}
#endif /* TIRPC_EPOLL */

void
svc_getreq_common (fd)
     int fd;
{
  SVCXPRT *xprt;
  bool_t code;

  xprt = svc_xprt_get(fd);

  if (xprt == NULL)
    return;

  code = xprt->xp_ops2->xp_getreq(xprt);

  return;
}

/* Allow internal or external getreq routines to validate xprt
 * has not been recursively disconnected. (I don't completely buy the
 * logic, but it should be unchanged (Matt) */
bool_t
svc_validate_xprt_list(SVCXPRT *xprt)
{
    bool_t code;

    code = (xprt == svc_xprt_get(xprt->xp_fd));

    return (code);
}

void
svc_dispatch_default(SVCXPRT *xprt, struct rpc_msg **ind_msg)
{
    struct svc_req r;
    struct rpc_msg *msg = *ind_msg;
    svc_vers_range_t vrange;
    svc_lookup_result_t lkp_res;
    svc_rec_t *svc_rec;
    enum auth_stat why;

    r.rq_xprt = xprt;
    r.rq_prog = msg->rm_call.cb_prog;
    r.rq_vers = msg->rm_call.cb_vers;
    r.rq_proc = msg->rm_call.cb_proc;
    r.rq_cred = msg->rm_call.cb_cred;

    /* first authenticate the message */
    if ((why = _authenticate (&r, msg)) != AUTH_OK)
    {
        svcerr_auth (xprt, why);
        return;
    }

    lkp_res = svc_lookup(&svc_rec, &vrange, r.rq_prog, r.rq_vers,
                         NULL, 0);
    switch (lkp_res) {
    case SVC_LKP_SUCCESS:
        /* call it */
        (*svc_rec->sc_dispatch) (&r, xprt);
        return;
    case SVC_LKP_VERS_NOTFOUND:
        svcerr_progvers (xprt, vrange.lowvers, vrange.highvers);
    default:
        svcerr_noprog (xprt);
        break;
    }
}

bool_t
svc_getreq_default(SVCXPRT *xprt)
{
  struct svc_req r;
  struct rpc_msg msg;
  enum xprt_stat stat;
  char cred_area[2 * MAX_AUTH_BYTES + RQCRED_SIZE];

  msg.rm_call.cb_cred.oa_base = cred_area;
  msg.rm_call.cb_verf.oa_base = &(cred_area[MAX_AUTH_BYTES]);
  r.rq_clntcred = &(cred_area[2 * MAX_AUTH_BYTES]);

  /* now receive msgs from xprt (support batch calls) */
  do
    {
      if (SVC_RECV (xprt, &msg))
        {
	  /* now find the exported program and call it */
          svc_vers_range_t vrange;
          svc_lookup_result_t lkp_res;
          svc_rec_t *svc_rec;
	  enum auth_stat why;

	  r.rq_xprt = xprt;
	  r.rq_prog = msg.rm_call.cb_prog;
	  r.rq_vers = msg.rm_call.cb_vers;
	  r.rq_proc = msg.rm_call.cb_proc;
	  r.rq_cred = msg.rm_call.cb_cred;

	  /* first authenticate the message */
	  if ((why = _authenticate (&r, &msg)) != AUTH_OK)
	    {
	      svcerr_auth (xprt, why);
	      goto call_done;
	    }

          lkp_res = svc_lookup(&svc_rec, &vrange, r.rq_prog, r.rq_vers,
                               NULL, 0);
          switch (lkp_res) {
          case SVC_LKP_SUCCESS:
              (*svc_rec->sc_dispatch) (&r, xprt);
              goto call_done;
              break;
          case SVC_LKP_VERS_NOTFOUND:
              svcerr_progvers (xprt, vrange.lowvers, vrange.highvers);
          default:
              svcerr_noprog (xprt);
              break;
          }
        } /* SVC_RECV again? */

      /*
       * Check if the xprt has been disconnected in a
       * recursive call in the service dispatch routine.
       * If so, then break.
       */
      if (!svc_validate_xprt_list(xprt))
          break;
    call_done:
      if ((stat = SVC_STAT (xprt)) == XPRT_DIED)
	{
	  SVC_DESTROY (xprt);
	  break;
	}
    else if ((xprt->xp_auth != NULL) &&
	     (xprt->xp_auth->svc_ah_private == NULL))
	{
	  xprt->xp_auth = NULL;
	}
    }
  while (stat == XPRT_MOREREQS);

  return (stat);
}

void
svc_getreq_poll (pfdp, pollretval)
     struct pollfd *pfdp;
     int pollretval;
{
  int i;
  int fds_found;

  for (i = fds_found = 0; fds_found < pollretval; i++)
    {
      struct pollfd *p = &pfdp[i];

      if (p->revents)
	{
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
	  if (! (p->revents & POLLNVAL))
              svc_getreq_common (p->fd);
	}
    }
}

bool_t
rpc_control (int what, void *arg)
{
  int val;

  switch (what) {
  case RPC_SVC_CONNMAXREC_SET:
      val = *(int *) arg;
      if (val <= 0)
	  return FALSE;
      __svc_maxrec = val;
      break;
  case RPC_SVC_CONNMAXREC_GET:
      *(int *) arg = __svc_maxrec;
      break;
  default:
      return (FALSE);
  }

  return (TRUE);
}
