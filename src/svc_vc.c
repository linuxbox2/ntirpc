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

#include <config.h>

/*
 * svc_vc.c, Server side for Connection Oriented RPC.
 *
 * Actually implements two flavors of transporter -
 * a tcp rendezvouser (a listner and connection establisher)
 * and a record/tcp stream.
 */
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_vrec.h>
#include <getpeereid.h>

#define XDR_VREC 0

extern struct svc_params __svc_params[1];

static bool rendezvous_request(SVCXPRT *, struct svc_req *);
static enum xprt_stat rendezvous_stat(SVCXPRT *);
static bool svc_vc_ref(SVCXPRT *xprt, u_int flags);
static void svc_vc_release(SVCXPRT *xprt, u_int flags);
static void svc_vc_destroy(SVCXPRT *);
int generic_read_vc(XDR *, void *, void *, int);
int generic_write_vc(XDR *, void *, void *, int);
static size_t readv_vc(void *xprtp, struct iovec *iov, int iovcnt,
		       u_int flags) __attribute__((unused));
static size_t writev_vc(void *xprtp, struct iovec *iov, int iovcnt,
			u_int flags) __attribute__((unused));
static size_t writev_vc(void *xprtp, struct iovec *iov, int iovcnt,
                        u_int flags);
static enum xprt_stat svc_vc_stat(SVCXPRT *);
static bool svc_vc_recv(SVCXPRT *, struct svc_req *);
static bool svc_vc_getargs(SVCXPRT *, struct svc_req *, xdrproc_t, void *,
			   void *);
static void svc_vc_lock(SVCXPRT *, uint32_t, const char *, int);
static void svc_vc_unlock(SVCXPRT *, uint32_t, const char *, int);
static bool svc_vc_freeargs(SVCXPRT *, xdrproc_t, void *);
static bool svc_vc_reply(SVCXPRT *, struct svc_req *, struct rpc_msg *);
static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_ops(SVCXPRT *);
static void svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *newxprt);
static bool svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in);
static bool svc_vc_rendezvous_control (SVCXPRT *xprt, const u_int rq, void *in);
bool __svc_clean_idle2(int timeout, bool cleanblock);
static SVCXPRT * makefd_xprt(int fd, u_int sendsz, u_int recvsz,
                             bool *allocated);

extern pthread_mutex_t svc_ctr_lock;

static size_t readv_vc(void *xprtp, struct iovec *iov, int iovcnt,
                       u_int flags)
{
    /* To be written */
    abort();
}

static size_t writev_vc(void *xprtp, struct iovec *iov, int iovcnt,
                        u_int flags)
{
    /* To be written */
    abort();
}


static void map_ipv4_to_ipv6(sin, sin6)
    struct sockaddr_in *sin;
    struct sockaddr_in6 *sin6;
{
#if defined(__linux__)
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = sin->sin_port;
    sin6->sin6_addr.s6_addr32[0] = 0;
    sin6->sin6_addr.s6_addr32[1] = 0;
    sin6->sin6_addr.s6_addr32[2] = htonl(0xffff);
    sin6->sin6_addr.s6_addr32[3] = *(uint32_t *) & sin->sin_addr; /* XXX strict */
#endif
}

/*
 * Usage:
 * xprt = svc_vc_ncreate(sock, send_buf_size, recv_buf_size);
 *
 * Creates, registers, and returns a (rpc) tcp based transporter.
 * Once *xprt is initialized, it is registered as a transporter
 * see (svc.h, xprt_register).  This routine returns
 * a NULL if a problem occurred.
 *
 * The filedescriptor passed in is expected to refer to a bound, but
 * not yet connected socket.
 *
 * Since streams do buffered io similar to stdio, the caller can specify
 * how big the send and receive buffers are via the second and third parms;
 * 0 => use the system default.
 *
 * Added svc_vc_ncreate2 with flags argument, has the behavior of the
 * original function if flags are SVC_VC_FLAG_NONE (0).
 *
 */
SVCXPRT *
svc_vc_ncreate2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
    SVCXPRT *xprt = NULL;
    struct cf_rendezvous *rdvs = NULL;
    struct __rpc_sockinfo si;
    struct sockaddr_storage sslocal;
    struct sockaddr *salocal;
    struct sockaddr_in *salocal_in;
    struct sockaddr_in6 *salocal_in6;
    struct rpc_dplx_rec *rec = NULL;
    uint32_t oflags;
    socklen_t slen;

    if (!__rpc_fd2sockinfo(fd, &si))
        return NULL;

    rdvs = mem_alloc(sizeof(struct cf_rendezvous));
    if (rdvs == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_ncreate: out of memory");
        goto err;
    }
    rdvs->sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
    rdvs->recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);
    rdvs->maxrec = __svc_maxrec;

    /* atomically find or create shared fd state */
    rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
    if (! rec) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc: makefd_xprt: rpc_dplx_lookup_rec failed");
        goto err;
    }

    /* if a svcxprt handle exists, return it ref'd */
    if (! (oflags & RPC_DPLX_LKP_OFLAG_ALLOC)) {
        if (rec->hdl.xprt) {
            struct x_vc_data *xd = (struct x_vc_data *) rec->hdl.xprt->xp_p1;
            /* dont return destroyed xprts */
            if (! (xd->flags & X_VC_DATA_FLAG_SVC_DESTROYED)) {
                xprt = rec->hdl.xprt;
                /* inc xprt refcnt */
                SVC_REF(xprt, SVC_REF_FLAG_NONE);
            }
            /* return extra ref */
            if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
                mutex_unlock(&rec->mtx);
            goto done;
        }
    }

    xprt = mem_zalloc(sizeof(SVCXPRT));
    if (xprt == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_ncreate: out of memory");
        goto err;
    }
    xprt->xp_flags = SVC_XPRT_FLAG_NONE;
    xprt->xp_refcnt = 1;
    svc_vc_rendezvous_ops(xprt);
    xprt->xp_p1 = rdvs;
    xprt->xp_p5 = rec;
    xprt->xp_fd = fd;
    mutex_init(&xprt->xp_lock, NULL);

    /* caller should know what it's doing */
    if (flags & SVC_VC_CREATE_LISTEN)
        listen(fd, SOMAXCONN);

    slen = sizeof (struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&sslocal, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_create: could not retrieve local addr");
        goto err;
    }

    /* XXX following breaks strict aliasing? */
    salocal = (struct sockaddr *) &sslocal;
    switch (salocal->sa_family) {
    case AF_INET:
        salocal_in = (struct sockaddr_in *) salocal;
        xprt->xp_port = ntohs(salocal_in->sin_port);
        break;
    case AF_INET6:
        salocal_in6 = (struct sockaddr_in6 *) salocal;
        xprt->xp_port = ntohs(salocal_in6->sin6_port);
        break;
    }
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &sslocal, sizeof(sslocal))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_ncreate: no mem for local addr");
        goto err;
    }

    /* make reachable from rec */
    rec->hdl.xprt = xprt;

    /* release rec */
    mutex_unlock(&rec->mtx);

    /* make reachable from xprt list */
    svc_rqst_init_xprt(xprt);

    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_XPRT_NOREG)))
        xprt_register(xprt);

done:
    return (xprt);

err:
    if (rdvs != NULL)
        mem_free(rdvs, sizeof(struct cf_rendezvous));

    if (xprt)
        mem_free(xprt, sizeof(SVCXPRT));

    if (rec) {
        if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
            mutex_unlock(&rec->mtx);
    }

    return (NULL);
}

SVCXPRT *
svc_vc_ncreate(int fd, u_int sendsize, u_int recvsize)
{
    return (svc_vc_ncreate2(fd, sendsize, recvsize, SVC_VC_CREATE_NONE));
}

/*
 * Like svtcp_ncreate(), except the routine takes any *open* UNIX file
 * descriptor as its first input.
 */
SVCXPRT *
svc_fd_ncreate(int fd, u_int sendsize, u_int recvsize)
{
    struct sockaddr_storage ss;
    socklen_t slen;
    SVCXPRT *xprt;
    bool xprt_allocd;

    assert(fd != -1);

    xprt = makefd_xprt(fd, sendsize, recvsize, &xprt_allocd);
    if ((! xprt) ||
        (! xprt_allocd)) /* ref'd existing xprt handle */
        goto done;

    /* conditional xprt_register */
    if (! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
        xprt_register(xprt);

    slen = sizeof(struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve local addr");
        goto freedata;
    }
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    slen = sizeof(struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve remote addr");
        goto freedata;
    }
    if (!__rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    /* Set xp_raddr for compatibility */
    __xprt_set_raddr(xprt, &ss);

done:
    return (xprt);

freedata:
    if (xprt->xp_ltaddr.buf != NULL)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);

    return (NULL);
}

/*
 * Like sv_fd_ncreate(), except export flags for additional control.  Add
 * special handling for AF_INET and AFS_INET6.  Possibly not needed,
 * because no longer called in Ganesha.
 */
SVCXPRT *
svc_fd_ncreate2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
    struct sockaddr_storage ss;
    struct sockaddr_in6 sin6;
    struct netbuf *addr;
    socklen_t slen;
    SVCXPRT *xprt;
    bool xprt_allocd;
    int af;

    assert(fd != -1);

    xprt = makefd_xprt(fd, sendsize, recvsize, &xprt_allocd);
    if ((! xprt) ||
        (! xprt_allocd)) /* ref'd existing xprt handle */
        goto done;

    slen = sizeof (struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_ncreate: could not retrieve local addr");
        goto err;
    }
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_ncreate: no mem for local addr");
        goto err;
    }

    slen = sizeof (struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_ncreate: could not retrieve remote addr");
        goto err;
    }
    af = ss.ss_family;

    /* XXX Ganesha concepts, and apparently no longer used, check */
    if (flags & SVC_VCCR_MAP6_V1) {
        if (af == AF_INET) {
            map_ipv4_to_ipv6((struct sockaddr_in *)&ss, &sin6);
            addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss));
        }
        else
            addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &sin6, sizeof(ss));
    } else
        addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss));
    if (!addr) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_ncreate: no mem for local addr");
        goto err;
    }

    /* XXX Ganesha concepts, check */
    if (flags & SVC_VCCR_RADDR) {
        switch (af) {
        case AF_INET:
            if (! (flags & SVC_VCCR_RADDR_INET))
                goto reg;
            break;
        case AF_INET6:
            if (! (flags & SVC_VCCR_RADDR_INET6))
                goto reg;
            break;
        case AF_LOCAL:
            if (! (flags & SVC_VCCR_RADDR_LOCAL))
                goto reg;
            break;
        default:
            break;
        }
        /* Set xp_raddr for compatibility */
        __xprt_set_raddr(xprt, &ss);
    }

reg:
    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_XPRT_NOREG)))
        xprt_register(xprt);

done:
    return (xprt);

err:
    if (xprt->xp_ltaddr.buf != NULL)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);

    return (NULL);
}

static SVCXPRT *
makefd_xprt(int fd, u_int sendsz, u_int recvsz, bool *allocated)
{
    SVCXPRT *xprt = NULL;
    struct x_vc_data *xd = NULL;
    struct rpc_dplx_rec *rec;
    struct __rpc_sockinfo si;
    const char *netid;
    uint32_t oflags;

    assert(fd != -1);

    if (! svc_vc_new_conn_ok()) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                    "%s: makefd_xprt: max_connections exceeded\n",
                    __func__);
                xprt = NULL;
                goto done;
    }

    /* atomically find or create shared fd state */
    rec = rpc_dplx_lookup_rec(fd, RPC_DPLX_LKP_IFLAG_LOCKREC, &oflags);
    if (! rec) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc: makefd_xprt: rpc_dplx_lookup_rec failed");
        goto done;
    }

    /* if a svcxprt handle exists, return it ref'd (rec is ref'd) */
    if (! (oflags & RPC_DPLX_LKP_OFLAG_ALLOC)) {
        if (rec->hdl.xprt) {
            xd = (struct x_vc_data *) rec->hdl.xprt->xp_p1;
            /* dont return destroyed xprts */
            if (! (xd->flags & X_VC_DATA_FLAG_SVC_DESTROYED)) {
                xprt = rec->hdl.xprt;
                /* inc xprt refcnt */
                SVC_REF(xprt, SVC_REF_FLAG_NONE);
            }
            /* return extra ref */
            if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
                mutex_unlock(&rec->mtx);
            *allocated = FALSE;
            goto done;
        }
    }

    /* XXX bi-directional?  initially I had assumed that explicit
     * routines to create a clnt or svc handle from an already-connected
     * handle of the other type, but perhaps it is more natural to
     * just discover it
     */

    /* new xprt (the common case) */
    xprt = mem_zalloc(sizeof(SVCXPRT));
    if (xprt == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc: makefd_xprt: out of memory");
        /* return extra ref */
        if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
            mutex_unlock(&rec->mtx);
        goto done;
    }

    *allocated = TRUE;
    xprt->xp_p5 = rec;
    mutex_init(&xprt->xp_lock, NULL);
    /* XXX take xp_lock? */
    mutex_init(&xprt->xp_auth_lock, NULL);
    xprt->xp_refcnt = 1;
    xprt->xp_fd = fd;

    /* other-direction shared state? */
    if (rec->hdl.clnt) {
        /* XXX check subtype of clnt handle? */
        xd = (struct x_vc_data *) rec->hdl.clnt->cl_p1;
        /* inc shared refcnt */
        ++(xd->refcnt);
    } else {
        xd = alloc_x_vc_data();
        if (xd == NULL) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                    "svc_vc: makefd_xprt: out of memory");
            /* return extra ref */
            if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
                mutex_unlock(&rec->mtx);
        mem_free(xprt, sizeof(SVCXPRT));
        goto done;
        }
        xd->rec = rec;
        /* XXX tracks outstanding calls */
        opr_rbtree_init(&xd->cx.calls.t, call_xid_cmpf);
        xd->cx.calls.xid = 0; /* next call xid is 1 */
        xd->refcnt = 1;
    }

    /* the SVCXPRT created in svc_vc_create accepts new connections
     * in its xp_recv op, the rendezvous_request method, but xprt is
     * a call channel */
    svc_vc_ops(xprt);

    xd->sx.strm_stat = XPRT_IDLE;

    if (! xd->rec->hdl.clnt) {
#if XDR_VREC
    /* duplex streams, plus buffer sharing, readv/writev */
    xdr_vrec_create(&(cd->xdrs_in),
                    XDR_VREC_IN, xprt, readv_vc, NULL, recvsz,
                    VREC_FLAG_NONE);

    xdr_vrec_create(&(cd->xdrs_out),
                    XDR_VREC_OUT, xprt, NULL, writev_vc, sendsz,
                    VREC_FLAG_NONE);
#else
    /* duplex streams */
    xdr_inrec_create(&(xd->shared.xdrs_in), recvsz, xd,
                     generic_read_vc);
    xd->shared.xdrs_in.x_op = XDR_DECODE;

    xdrrec_create(&(xd->shared.xdrs_out), sendsz, recvsz, xd,
                  generic_read_vc,
                  generic_write_vc);
    xd->shared.xdrs_out.x_op = XDR_ENCODE;
#endif
    } /* CLNT */

    xprt->xp_p1 = xd;
    if (__rpc_fd2sockinfo(fd, &si) && __rpc_sockinfo2netid(&si, &netid))
        xprt->xp_netid = rpc_strdup(netid);

    /* make reachable from rec */
    rec->hdl.xprt = xprt;

    /* release */
    mutex_unlock(&rec->mtx);

    /* Make reachable from xprt list.  Registration deferred. */
    svc_rqst_init_xprt(xprt);

done:
    return (xprt);
}

/*ARGSUSED*/
static bool
rendezvous_request(SVCXPRT *xprt, struct svc_req *req)
{
    int fd;
    socklen_t len;
    struct cf_rendezvous *rdvs;
    struct x_vc_data *xd;
    struct sockaddr_storage addr;
    struct __rpc_sockinfo si;
    SVCXPRT *newxprt;
    bool xprt_allocd;

    rdvs = (struct cf_rendezvous *)xprt->xp_p1;
again:
    len = sizeof addr;
    if ((fd = accept(xprt->xp_fd, (struct sockaddr *)(void *)&addr,
                     &len)) < 0) {
        if (errno == EINTR)
            goto again;
        /*
         * Clean out the most idle file descriptor when we're
         * running out.
         */
        if (errno == EMFILE || errno == ENFILE) {
            switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
            case SVC_EVENT_EPOLL:
                /* XXX we did implement a plug-out strategy for this--check
                 * whether svc_clean_idle2 should be called */
                break;
#endif
            default:
                /* XXX formerly select/fd_set case, now placeholder
                 * for new event systems, reworked select, etc. */
                abort(); /* XXX */
                break;
            } /* switch */
            goto again;
        }
        return (FALSE);
    }
    /*
     * make a new transport (re-uses xprt)
     */
    newxprt = makefd_xprt(fd, rdvs->sendsize, rdvs->recvsize, &xprt_allocd);
    if ((! newxprt) ||
        (! xprt_allocd)) /* ref'd existing xprt handle */
        return (FALSE);

    /*
     * propagate special ops
     */
    svc_vc_override_ops(xprt, newxprt);

    /* move xprt_register() out of makefd_xprt */
    (void) svc_rqst_xprt_register(xprt, newxprt);

    if (!__rpc_set_netbuf(&newxprt->xp_rtaddr, &addr, len)) {
        abort();
        return (FALSE);
    }

    __xprt_set_raddr(newxprt, &addr);

    /* XXX fvdl - is this useful? (Yes.  Matt) */
    if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
        len = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
    }

    xd = (struct x_vc_data *) newxprt->xp_p1;
    xd->shared.recvsz = rdvs->recvsize;
    xd->shared.sendsz = rdvs->sendsize;
    xd->sx.maxrec = rdvs->maxrec;

#if 0 /* XXX vrec wont support atm (and it seems to need work) */
    if (cd->maxrec != 0) {
        flags = fcntl(fd, F_GETFL, 0);
        if (flags  == -1)
            return (FALSE);
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            return (FALSE);
        if (xd->shared.recvsz > xd->sx.maxrec)
            xd->shared.recvsz = xd->sx.maxrec;
        xd->shared.nonblock = TRUE;
        __xdrrec_setnonblock(&xd->shared.xdrs_in, xd->sx.maxrec);
        __xdrrec_setnonblock(&xd->shared.xdrs_out, xd->sx.maxrec);
    } else
        cd->nonblock = FALSE;
#else
    xd->shared.nonblock = FALSE;
#endif
    (void) clock_gettime(CLOCK_MONOTONIC_FAST, &xd->sx.last_recv);

    /* if parent has xp_rdvs, use it */
    if (xprt->xp_ops2->xp_rdvs)
        xprt->xp_ops2->xp_rdvs(xprt, newxprt, SVC_RQST_FLAG_NONE, NULL);

    return (FALSE); /* there is never an rpc msg to be processed */
}

/*ARGSUSED*/
static enum xprt_stat
rendezvous_stat(SVCXPRT *xprt)
{
    return (XPRT_IDLE);
}

static bool
svc_vc_ref(SVCXPRT *xprt, u_int flags)
{
    uint32_t refcnt;

    if (! (flags & SVC_REF_FLAG_LOCKED))
        mutex_lock(&xprt->xp_lock);

    if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {        
        mutex_unlock(&xprt->xp_lock);
        return (false);
    }
    refcnt = ++(xprt->xp_refcnt);
    mutex_unlock(&xprt->xp_lock);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: postref %p xp_refcnt %u",
            __func__, xprt, refcnt);

    return (true);
}

/* XXX pending further unification 
 *
 * note:  currently, rdvs xprt handles have a rec structure,
 * but no xd structure, etc.
 *
 */

static inline void
rdvs_dodestroy(SVCXPRT *xprt)
{
    struct cf_rendezvous *rdvs = (struct cf_rendezvous *) xprt->xp_p1;
    struct rpc_dplx_rec *rec = (struct rpc_dplx_rec *) xprt->xp_p5;

    if (xprt->xp_fd != RPC_ANYFD)
        (void)close(xprt->xp_fd);

    /* call free hook */
    if (xprt->xp_ops2->xp_free_xprt)
        xprt->xp_ops2->xp_free_xprt(xprt);

    mutex_lock(&rec->mtx);

    mutex_destroy(&xprt->xp_lock);

    if (xprt->xp_rtaddr.buf)
        mem_free(xprt->xp_rtaddr.buf, xprt->xp_rtaddr.maxlen);
    if (xprt->xp_ltaddr.buf)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);
    if (xprt->xp_tp)
        mem_free(xprt->xp_tp, 0);
    if (xprt->xp_netid)
        mem_free(xprt->xp_netid, 0);

    mem_free(rdvs, sizeof (struct cf_rendezvous));
    mem_free(xprt, sizeof(SVCXPRT));

    if (rpc_dplx_unref(rec, RPC_DPLX_FLAG_LOCKED))
        mutex_unlock(&rec->mtx);
}

static void
svc_rdvs_release(SVCXPRT *xprt, u_int flags)
{
    uint32_t xp_refcnt;

    if (! (flags & SVC_RELEASE_FLAG_LOCKED))
        mutex_lock(&xprt->xp_lock);

    xp_refcnt = --(xprt->xp_refcnt);
    mutex_unlock(&xprt->xp_lock);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: postunref %p xp_refcnt %u", __func__, xprt, xp_refcnt);

    /* conditional destroy */
    if ((xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) &&
        (xp_refcnt == 0)) {
        __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                "%s: %p xp_refcnt %u calling rdvs_dodestroy",
                __func__, xprt, xp_refcnt);
        rdvs_dodestroy(xprt);
    } 
}

static void
svc_rdvs_destroy(SVCXPRT *xprt)
{
    uint32_t xp_refcnt = 0;
    uint32_t drefcnt = 0; /* debugging only */

    mutex_lock(&xprt->xp_lock);
    if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
        mutex_unlock(&xprt->xp_lock);
        goto out;
    }

    xprt->xp_flags |= SVC_XPRT_FLAG_DESTROYED;
    xp_refcnt = --(xprt->xp_refcnt); /* sentinel ref */
    mutex_unlock(&xprt->xp_lock);

    drefcnt = xprt->xp_refcnt;
    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: preunreg %p xp_refcnt %u", __func__, xprt, drefcnt);

    /* XXX prefer LOCKED? (would require lock order change) */
    (void) svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);

    drefcnt = xprt->xp_refcnt;
    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: prefinalize %p xp_refcnt %u", __func__, xprt, drefcnt);

    /* clears xprt from the xprt table (eg, idle scans) */
    svc_rqst_finalize_xprt(xprt, SVC_RQST_FLAG_NONE);

    drefcnt = xprt->xp_refcnt;
    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: postfinalize %p xp_refcnt %u", __func__, xprt, drefcnt);

    if (xp_refcnt == 0) {
        __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                "%s: %p xp_refcnt %u calling rdvs_dodestroy",
                __func__, xprt, xp_refcnt);
        rdvs_dodestroy(xprt);
    }

out:
    return;
}

static void
svc_vc_release(SVCXPRT *xprt, u_int flags)
{
    uint32_t xp_refcnt;

    if (! (flags & SVC_RELEASE_FLAG_LOCKED))
        mutex_lock(&xprt->xp_lock);

    xp_refcnt = --(xprt->xp_refcnt);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: postunref %p xp_refcnt %u", __func__, xprt, xp_refcnt);

    /* conditional destroy */
    if ((xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) &&
        (xp_refcnt == 0)) {
 
        struct x_vc_data *xd = (struct x_vc_data *) xprt->xp_p1;
        struct rpc_dplx_rec *rec = xd->rec;
        uint32_t xd_refcnt;

        mutex_unlock(&xprt->xp_lock);
        mutex_lock(&rec->mtx);

        xd_refcnt = xd->refcnt;
        if (xd_refcnt == 0) {
            __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                    "%s: xd_refcnt %u on destroyed %p %u calling "
                    "vc_shared_destroy",
                    __func__, xprt, xd_refcnt);
            vc_shared_destroy(xd); /* RECLOCKED */
        } else {
            __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                    "%s: xd_refcnt %u on destroyed %p omit "
                    "vc_shared_destroy",
                    __func__, xprt, xd_refcnt);
            mutex_unlock(&rec->mtx);
        }
    }
    else
        mutex_unlock(&xprt->xp_lock);
}

static void
svc_vc_destroy(SVCXPRT *xprt)
{
    struct rpc_dplx_rec *rec;
    struct x_vc_data *xd;
    uint32_t xp_refcnt = 0;
    uint32_t xd_refcnt = 0;
    uint32_t drefcnt = 0; /* debugging only */

    mutex_lock(&xprt->xp_lock);
    if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
        mutex_unlock(&xprt->xp_lock);
        goto out;
    }

    xd = (struct x_vc_data *) xprt->xp_p1;
    rec = xd->rec;

    xprt->xp_flags |= SVC_XPRT_FLAG_DESTROYED;
    xp_refcnt = --(xprt->xp_refcnt);
    mutex_unlock(&xprt->xp_lock);

    drefcnt = xprt->xp_refcnt;
    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: preunreg %p xp_refcnt %u", __func__, xprt, drefcnt);

    /* XXX prefer LOCKED? (would require lock order change) */
    (void) svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);

    /* connection tracking--decrement now, he's dead jim */
    svc_vc_dec_nconns();

    drefcnt = xprt->xp_refcnt;
    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: prefinalize %p xp_refcnt %u", __func__, xprt, drefcnt);

    /* clears xprt from the xprt table (eg, idle scans) */
    svc_rqst_finalize_xprt(xprt, SVC_RQST_FLAG_NONE);

    drefcnt = xprt->xp_refcnt;

    /* bidirectional */
    mutex_lock(&rec->mtx);
    xd->flags |= X_VC_DATA_FLAG_SVC_DESTROYED; /* destroyed handle is dead */
    xd_refcnt = --(xd->refcnt);

    __warnx(TIRPC_DEBUG_FLAG_REFCNT,
            "%s: postfinalize %p xp_refcnt %u xd_refcnt %u",
            __func__, xprt, drefcnt, xd_refcnt);

    /* conditional destroy */
    if ((xp_refcnt == 0) &&
        (xd_refcnt == 0)) {
        __warnx(TIRPC_DEBUG_FLAG_REFCNT,
                "%s: %p xp_refcnt %u xd_refcnt %u calling vc_shared_destroy",
                __func__, xprt, xp_refcnt, xd_refcnt);
        vc_shared_destroy(xd); /* RECLOCKED */
    } else
        mutex_unlock(&rec->mtx);

out:
    return;
 }

extern mutex_t ops_lock;

/*ARGSUSED*/
static bool
svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in)
{
    switch (rq) {
    case SVCGET_XP_FLAGS:
        *(u_int *)in = xprt->xp_flags;
        break;
    case SVCSET_XP_FLAGS:
        xprt->xp_flags = *(u_int *)in;
        break;
    case SVCGET_XP_RECV:
        mutex_lock(&ops_lock);
        *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RECV:
        mutex_lock(&ops_lock);
        xprt->xp_ops->xp_recv = *(xp_recv_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_GETREQ:
        mutex_lock(&ops_lock);
        *(xp_getreq_t *)in = xprt->xp_ops2->xp_getreq;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_GETREQ:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_getreq = *(xp_getreq_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        *(xp_dispatch_t *)in = xprt->xp_ops2->xp_dispatch;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_dispatch = *(xp_dispatch_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_RDVS:
        mutex_lock(&ops_lock);
        *(xp_rdvs_t *)in = xprt->xp_ops2->xp_rdvs;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RDVS:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_rdvs = *(xp_rdvs_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        *(xp_free_xprt_t *)in = xprt->xp_ops2->xp_free_xprt;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_free_xprt = *(xp_free_xprt_t)in;
        mutex_unlock(&ops_lock);
        break;
    default:
        return (FALSE);
    }
    return (TRUE);
}

static bool
svc_vc_rendezvous_control(SVCXPRT *xprt, const u_int rq, void *in)
{
    struct cf_rendezvous *cfp;

    cfp = (struct cf_rendezvous *)xprt->xp_p1;
    if (cfp == NULL)
        return (FALSE);
    switch (rq) {
    case SVCGET_CONNMAXREC:
        *(int *)in = cfp->maxrec;
        break;
    case SVCSET_CONNMAXREC:
        cfp->maxrec = *(int *)in;
        break;
    case SVCGET_XP_RECV:
        *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
        break;
    case SVCSET_XP_RECV:
        xprt->xp_ops->xp_recv = *(xp_recv_t)in;
        break;
    case SVCGET_XP_GETREQ:
        mutex_lock(&ops_lock);
        *(xp_getreq_t *)in = xprt->xp_ops2->xp_getreq;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_GETREQ:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_getreq = *(xp_getreq_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        *(xp_dispatch_t *)in = xprt->xp_ops2->xp_dispatch;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_dispatch = *(xp_dispatch_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_RDVS:
        mutex_lock(&ops_lock);
        *(xp_rdvs_t *)in = xprt->xp_ops2->xp_rdvs;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RDVS:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_rdvs = *(xp_rdvs_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        *(xp_free_xprt_t *)in = xprt->xp_ops2->xp_free_xprt;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_free_xprt = *(xp_free_xprt_t)in;
        mutex_unlock(&ops_lock);
        break;
    default:
        return (FALSE);
    }
    return (TRUE);
}

static enum xprt_stat
svc_vc_stat(SVCXPRT *xprt)
{
    struct x_vc_data *xd;
    uint32_t xp_flags;

    /* XXX overkill? */
    mutex_lock(&xprt->xp_lock);
    xd = (struct x_vc_data *) xprt->xp_p1;
    xp_flags = xprt->xp_flags;
    mutex_unlock(&xprt->xp_lock);

    if (xp_flags & SVC_XPRT_FLAG_DESTROYED)
        return (XPRT_DESTROYED);

    /* we hold the recv lock */
    if (xd->sx.strm_stat == XPRT_DIED)
        return (XPRT_DIED);

#if XDR_VREC
    /* SVC_STAT() only cares about the recv queue */
    if (! xdr_vrec_eof(&(xd->shared.xdrs_in)))
#else
    if (! xdr_inrec_eof(&(xd->shared.xdrs_in)))
#endif
        return (XPRT_MOREREQS);

    return (XPRT_IDLE);
}

static bool
svc_vc_recv(SVCXPRT *xprt, struct svc_req *req)
{
    struct x_vc_data *xd = (struct x_vc_data *) xprt->xp_p1;
    XDR *xdrs = &(xd->shared.xdrs_in); /* recv queue */

    /* XXX assert(! cd->nonblock) */
    if (xd->shared.nonblock) {
        if (!__xdrrec_getrec(xdrs, &xd->sx.strm_stat, TRUE))
            return FALSE;
    }

    xdrs->x_op = XDR_DECODE;

    xdrs->x_lib[0] = (void *) RPC_DPLX_SVC;
    xdrs->x_lib[1] = (void *) xprt; /* transiently thread xprt */

    /* Consumes any remaining -fragment- bytes, and clears last_frag */
#if XDR_VREC
    (void) xdr_vrec_skiprecord(xdrs);
#else
    (void) xdr_inrec_skiprecord(xdrs);
#endif

    req->rq_msg = alloc_rpc_msg();
    req->rq_clntcred = req->rq_msg->rm_call.cb_cred.oa_base +
        (2 * MAX_AUTH_BYTES);

    /* Advances to next record, will read up to 1024 bytes
     * into the stream. */
    (void) xdr_inrec_readahead(xdrs, 1024);

    if (xdr_dplx_msg(xdrs, req->rq_msg)) {
        switch (req->rq_msg->rm_direction) {
        case CALL:
            /* an ordinary call header */
            req->rq_xprt = xprt;
            req->rq_prog = req->rq_msg->rm_call.cb_prog;
            req->rq_vers = req->rq_msg->rm_call.cb_vers;
            req->rq_proc = req->rq_msg->rm_call.cb_proc;
            req->rq_xid = req->rq_msg->rm_xid;
            return (TRUE);
            break;
        case REPLY:
            /* reply header (xprt OK) */
            if (xd->rec->hdl.clnt) {
                rpc_ctx_xfer_replymsg(xd, req->rq_msg);
		req->rq_msg = NULL;
            }
            break;
        default:
            /* not good (but xprt OK) */
            break;
        }
        /* XXX skiprecord? */
        return (FALSE);
    }
    __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
            "%s: xdr_dplx_msg failed (will set dead)", __func__);
    return (FALSE);
}

static bool
svc_vc_getargs(SVCXPRT *xprt, struct svc_req *req, xdrproc_t xdr_args,
	       void *args_ptr, void *u_data)
{
    bool rslt = TRUE;
    struct x_vc_data *xd = (struct x_vc_data *) xprt->xp_p1;
    XDR *xdrs = &xd->shared.xdrs_in; /* recv queue */

    /* threads u_data for advanced decoders*/
    xdrs->x_public = u_data;

    if (! SVCAUTH_UNWRAP(req->rq_auth, req, xdrs, xdr_args, args_ptr))
        rslt = FALSE;

    /* XXX Upstream TI-RPC lacks this call, but -does- call svc_dg_freeargs
     * in svc_dg_getargs if SVCAUTH_UNWRAP fails. */
    if (! rslt)
        svc_vc_freeargs(xprt, xdr_args, args_ptr);
    else
        req->rq_cksum = xdr_inrec_cksum(xdrs);
            
    return (rslt);
}

static bool
svc_vc_freeargs(SVCXPRT *xprt, xdrproc_t xdr_args, void *args_ptr)
{
    XDR xdrs = {
        .x_public = NULL,
        .x_lib = { NULL, NULL }
    };
    xdrmem_create(&xdrs, args_ptr, ~0, XDR_FREE);
    return ((*xdr_args)(&xdrs, args_ptr));
}

static bool
svc_vc_reply(SVCXPRT *xprt, struct svc_req *req, struct rpc_msg *msg)
{
    struct x_vc_data *xd = (struct x_vc_data *) xprt->xp_p1;
    XDR *xdrs = &xd->shared.xdrs_out; /* send queue */
    xdrproc_t xdr_results;
    caddr_t xdr_location;
    bool rstat;
    bool has_args;

    if (msg->rm_reply.rp_stat == MSG_ACCEPTED &&
        msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
        has_args = TRUE;
        xdr_results = msg->acpted_rply.ar_results.proc;
        xdr_location = msg->acpted_rply.ar_results.where;

        msg->acpted_rply.ar_results.proc = (xdrproc_t)xdr_void;
        msg->acpted_rply.ar_results.where = NULL;
    } else {
        has_args = FALSE;
        xdr_results = NULL;
        xdr_location = NULL;
    }

    xdrs->x_op = XDR_ENCODE;

    xdrs->x_lib[0] = (void *) RPC_DPLX_SVC;
    xdrs->x_lib[1] = (void *) xprt; /* transiently thread xprt */

    rstat = FALSE;
    if (xdr_replymsg(xdrs, msg) &&
        (!has_args || (req->rq_auth &&
                       SVCAUTH_WRAP(req->rq_auth, req, xdrs, xdr_results,
                                    xdr_location)))) {
        rstat = TRUE;
    }
#if XDR_VREC
    (void)xdr_vrec_endofrecord(xdrs, TRUE);
#else
    (void)xdrrec_endofrecord(xdrs, TRUE);
#endif
    return (rstat);
}

static void
svc_vc_lock(SVCXPRT *xprt, uint32_t flags, const char *func, int line)
{
    if (flags & XP_LOCK_RECV)
        rpc_dplx_rlxi(xprt, func, line);

    if (flags & XP_LOCK_SEND)
        rpc_dplx_slxi(xprt, func, line);
}

static void
svc_vc_unlock(SVCXPRT *xprt, uint32_t flags, const char *func, int line)
{
    if (flags & XP_LOCK_RECV)
        rpc_dplx_rux(xprt);

    if (flags & XP_LOCK_SEND)
        rpc_dplx_sux(xprt);
}

static void
svc_vc_ops(SVCXPRT *xprt)
{
    static struct xp_ops ops;
    static struct xp_ops2 ops2;

/* VARIABLES PROTECTED BY ops_lock: ops, ops2, xp_type */

    mutex_lock(&ops_lock);
    xprt->xp_type = XPRT_TCP;
    if (ops.xp_recv == NULL) {
        ops.xp_recv = svc_vc_recv;
        ops.xp_stat = svc_vc_stat;
        ops.xp_getargs = svc_vc_getargs;
        ops.xp_lock = svc_vc_lock;
        ops.xp_unlock = svc_vc_unlock;
        ops.xp_reply = svc_vc_reply;
        ops.xp_freeargs = svc_vc_freeargs;
        ops.xp_ref = svc_vc_ref;
        ops.xp_release = svc_vc_release;
        ops.xp_destroy = svc_vc_destroy;
        ops2.xp_control = svc_vc_control;
        ops2.xp_getreq = svc_getreq_default;
        ops2.xp_dispatch = svc_dispatch_default;
        ops2.xp_rdvs = NULL; /* no default */
    }
    xprt->xp_ops = &ops;
    xprt->xp_ops2 = &ops2;
    mutex_unlock(&ops_lock);
}

static void
svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *newxprt)
{
    if (xprt->xp_ops2->xp_getreq)
        newxprt->xp_ops2->xp_getreq = xprt->xp_ops2->xp_getreq;
    if (xprt->xp_ops2->xp_dispatch)
        newxprt->xp_ops2->xp_dispatch = xprt->xp_ops2->xp_dispatch;
    if (xprt->xp_ops2->xp_rdvs)
        newxprt->xp_ops2->xp_rdvs = xprt->xp_ops2->xp_rdvs;
    if (xprt->xp_ops2->xp_free_xprt)
        newxprt->xp_ops2->xp_free_xprt = xprt->xp_ops2->xp_free_xprt;
}

static void
svc_vc_rendezvous_ops(SVCXPRT *xprt)
{
    static struct xp_ops ops;
    static struct xp_ops2 ops2;
    extern mutex_t ops_lock;

    mutex_lock(&ops_lock);
    xprt->xp_type = XPRT_TCP_RENDEZVOUS;
    if (ops.xp_recv == NULL) {
        ops.xp_recv = rendezvous_request;
        ops.xp_stat = rendezvous_stat;
        ops.xp_lock = svc_vc_lock;
        ops.xp_unlock = svc_vc_unlock;
        /* XXX wow */
        ops.xp_getargs =
		(bool (*)(SVCXPRT *, struct svc_req *,  xdrproc_t,
			  void *, void *))abort;
        ops.xp_reply =
		(bool (*)(SVCXPRT *, struct svc_req *req,
			  struct rpc_msg *))abort;
        ops.xp_freeargs =
		(bool (*)(SVCXPRT *, xdrproc_t, void *))abort;
        ops.xp_ref = svc_vc_ref;
        ops.xp_release = svc_rdvs_release;
	ops.xp_destroy = svc_rdvs_destroy;
        ops2.xp_control = svc_vc_rendezvous_control;
        ops2.xp_getreq = svc_getreq_default;
        ops2.xp_dispatch = svc_dispatch_default;
    }
    xprt->xp_ops = &ops;
    xprt->xp_ops2 = &ops2;
    mutex_unlock(&ops_lock);
}

/*
 * Get the effective UID of the sending process. Used by rpcbind, keyserv
 * and rpc.yppasswdd on AF_LOCAL.
 */
int
__rpc_get_local_uid(SVCXPRT *transp, uid_t *uid) {
    int sock, ret;
    gid_t egid;
    uid_t euid;
    struct sockaddr *sa;

    sock = transp->xp_fd;
    sa = (struct sockaddr *)transp->xp_rtaddr.buf;
    if (sa->sa_family == AF_LOCAL) {
        ret = getpeereid(sock, &euid, &egid);
        if (ret == 0)
            *uid = euid;
        return (ret);
    } else
        return (-1);
}

/*
 * Destroy xprts that have not have had any activity in 'timeout' seconds.
 * If 'cleanblock' is true, blocking connections (the default) are also
 * cleaned. If timeout is 0, the least active connection is picked.
 *
 * Though this is not a publicly documented interface, some versions of
 * rpcbind are known to call this function.  Do not alter or remove this
 * API without changing the library's sonum.
 */

bool
__svc_clean_idle(fd_set *fds, int timeout, bool cleanblock)
{
    return ( __svc_clean_idle2(timeout, cleanblock) );

} /* __svc_clean_idle */

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */

struct svc_clean_idle_arg
{
    SVCXPRT *least_active;
    struct timespec ts, tmax;
    int cleanblock, ncleaned, timeout;
};

static uint32_t
svc_clean_idle2_func(SVCXPRT *xprt, void *arg)
{
    struct timespec tdiff;
    struct svc_clean_idle_arg *acc = (struct svc_clean_idle_arg *) arg;
    uint32_t rflag = SVC_XPRT_FOREACH_NONE;

    if (! acc->cleanblock)
        goto out;

    mutex_lock(&xprt->xp_lock);

    /* invalid xprt (error) */
    if (xprt->xp_ops == NULL)
        goto unlock;

    if (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED) {
        /* XXX should not happen--but do no harm */
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s: destroyed xprt %p seen in clan idle\n",
                __func__, xprt);
        goto unlock;
    }

    if (xprt->xp_ops->xp_recv != svc_vc_recv)
        goto unlock;

    {
        /* XXX nb., safe because xprt type is verfied */
        struct x_vc_data *xd = xprt->xp_p1;
        if (! xd->shared.nonblock)
            goto unlock;

        if (acc->timeout == 0) {
            tdiff = acc->ts; timespecsub(&tdiff, &xd->sx.last_recv);
            if (timespeccmp(&tdiff, &acc->tmax, >)) {
                acc->tmax = tdiff;
                acc->least_active = xprt;
            }
            goto unlock;
        }
        if (acc->ts.tv_sec - xd->sx.last_recv.tv_sec > acc->timeout) {
            rflag = SVC_XPRT_FOREACH_CLEAR;
            mutex_unlock(&xprt->xp_lock);
            SVC_DESTROY(xprt);
            acc->ncleaned++;
            goto out;
        }
    }

unlock:
    mutex_unlock(&xprt->xp_lock);

out:
    return (rflag);
}

bool
__svc_clean_idle2(int timeout, bool cleanblock)
{
    struct svc_clean_idle_arg acc;
    static mutex_t active_mtx = MUTEX_INITIALIZER;
    static uint32_t active = 0;
    bool_t rslt = FALSE;

    if (mutex_trylock(&active_mtx) != 0)
        goto out;

    if (active > 0)
        goto unlock;

    ++active;

    memset(&acc, 0, sizeof(struct svc_clean_idle_arg));
    (void) clock_gettime(CLOCK_MONOTONIC_FAST, &acc.ts);
    acc.cleanblock = cleanblock;
    acc.timeout = timeout;

    svc_xprt_foreach(svc_clean_idle2_func, (void *) &acc);

    if (timeout == 0 && acc.least_active != NULL) {
        SVC_DESTROY(acc.least_active);
        acc.ncleaned++;
    }
    rslt = (acc.ncleaned > 0) ? TRUE : FALSE;
    --active;

unlock:
    mutex_unlock(&active_mtx);
out:
    return (rslt);

} /* __svc_clean_idle2 */

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 *
 * If flags & SVC_VC_CLNT_CREATE_DEDICATED, the supplied xprt will be
 * unregistered and disposed inline.
 */
CLIENT *
clnt_vc_ncreate_svc(SVCXPRT *xprt,
                    const rpcprog_t prog,
                    const rpcvers_t vers,
                    const uint32_t flags)
{
    struct x_vc_data *xd;
    CLIENT *clnt;

    mutex_lock(&xprt->xp_lock);

    xd = (struct x_vc_data *) xprt->xp_p1;

    /* XXX return allocated client structure, or allocate one if none
     * is currently allocated */

    clnt = clnt_vc_ncreate2(xprt->xp_fd,
                            &xprt->xp_rtaddr,
                            prog,
                            vers,
                            xd->shared.recvsz,
                            xd->shared.sendsz,
                            CLNT_CREATE_FLAG_SVCXPRT);
    if (! clnt)
        goto fail;

    mutex_unlock(&xprt->xp_lock);

fail:
    /* for a dedicated channel, unregister and free xprt */
    if ((flags & SVC_VC_CREATE_ONEWAY) &&
        (flags & SVC_VC_CREATE_DISPOSE)) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s:  disposing--calls svc_vc_destroy\n",
                __func__);
        svc_vc_destroy(xprt);
    }

    return (clnt);
}

/*
 * Create an RPC SVCXPRT handle from an active client transport
 * handle, i.e., to service RPC requests.
 *
 * If flags & SVC_VC_CREATE_CL_FLAG_DEDICATED, then clnt is also
 * deallocated without closing cl->cl_p1->ct_fd.
 */
SVCXPRT *
svc_vc_ncreate_clnt(CLIENT *clnt,
                    const u_int sendsz,
                    const u_int recvsz,
                    const uint32_t flags)
{
    int fd;
    socklen_t len;
    struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
    struct ct_data *ct = &xd->cx.data;
    struct sockaddr_storage addr;
    struct __rpc_sockinfo si;
    SVCXPRT *xprt = NULL;
    bool xprt_allocd;

    fd = ct->ct_fd;
    rpc_dplx_rlc(clnt);
    rpc_dplx_slc(clnt);

    len = sizeof (struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&addr, &len) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s: could not retrieve remote addr",
                __func__);
        goto unlock;
    }

    /*
     * make a new transport
     */

    xprt = makefd_xprt(fd, sendsz, recvsz, &xprt_allocd);
    if ((! xprt) ||
        (! xprt_allocd)) /* ref'd existing xprt handle */
        goto unlock;

    if (!__rpc_set_netbuf(&xprt->xp_rtaddr, &addr, len)) {
        /* fatal */
        svc_vc_destroy(xprt);
        goto unlock;
    }

    __xprt_set_raddr(xprt, &addr);

    if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
        len = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
    }

    xd->sx.maxrec = __svc_maxrec; /* XXX check */

#if 0 /* XXX wont currently support */
    if (xd->sx.maxrec != 0) {
        fflags = fcntl(fd, F_GETFL, 0);
        if (fflags  == -1)
            return (FALSE);
        if (fcntl(fd, F_SETFL, fflags | O_NONBLOCK) == -1)
            return (FALSE);
        if (xd->shared.recvsz > xd->sx.maxrec)
            xd->shared.recvsz = xd->sx.maxrec;
        cd->nonblock = TRUE;
        __xdrrec_setnonblock(&cd->xdrs, xd->sx.maxrec);
    } else
        xd->shared.nonblock = FALSE;
#else
    xd->shared.nonblock = FALSE;
#endif
    (void) clock_gettime(CLOCK_MONOTONIC_FAST, &xd->sx.last_recv);

    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_XPRT_NOREG)))
        xprt_register(xprt);

    /* If creating a dedicated channel collect the supplied client
     * without closing fd */
    if ((flags & SVC_VC_CREATE_ONEWAY) &&
        (flags & SVC_VC_CREATE_DISPOSE)) {
        ct->ct_closeit = FALSE; /* must not close */
        CLNT_DESTROY(clnt); /* clean up immediately */
    }

unlock:
    rpc_dplx_ruc(clnt);
    rpc_dplx_suc(clnt);

    return (xprt);
}
