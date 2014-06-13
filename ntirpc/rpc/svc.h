/* $netbsd: svc.h,v 1.17 2000/06/02 22:57:56 fvdl Exp $ */

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
 *
 * from: @(#)svc.h 1.35 88/12/17 SMI
 * from: @(#)svc.h      1.27    94/04/25 SMI
 * $FreeBSD: src/include/rpc/svc.h,v 1.24 2003/06/15 10:32:01 mbr Exp $
 */

/*
 * svc.h, Server-side remote procedure call interface.
 *
 * Copyright (C) 1986-1993 by Sun Microsystems, Inc.
 */

#ifndef _TIRPC_SVC_H
#define _TIRPC_SVC_H
#include <sys/cdefs.h>
#include <rpc/types.h>
#include "reentrant.h"

/*
 * This interface must manage two items concerning remote procedure calling:
 *
 * 1) An arbitrary number of transport connections upon which rpc requests
 * are received.  The two most notable transports are TCP and UDP;  they are
 * created and registered by routines in svc_tcp.c and svc_udp.c, respectively;
 * they in turn call xprt_register and xprt_unregister.
 *
 * 2) An arbitrary number of locally registered services.  Services are
 * described by the following four data: program number, version number,
 * "service dispatch" function, a transport handle, and a boolean that
 * indicates whether or not the exported program should be registered with a
 * local binder service;  if true the program's number and version and the
 * port number from the transport handle are registered with the binder.
 * These data are registered with the rpc svc system via svc_register.
 *
 * A service's dispatch function is called whenever an rpc request comes in
 * on a transport.  The request's program and version numbers must match
 * those of the registered service.  The dispatch function is passed two
 * parameters, struct svc_req * and SVCXPRT *, defined below.
 */

/* Package init flags */
#define SVC_INIT_DEFAULT        0x0000
#define SVC_INIT_XPRTS          0x0001
#define SVC_INIT_EPOLL          0x0002
#define SVC_INIT_WARNX          0x0004
#define SVC_INIT_NOREG_XPRTS    0x0008

#define SVC_SHUTDOWN_FLAG_NONE  0x0000

/*
 *      Service control requests
 */
#define SVCGET_VERSQUIET 1
#define SVCSET_VERSQUIET 2
#define SVCGET_CONNMAXREC 3
#define SVCSET_CONNMAXREC 4
#define SVCGET_XP_RECV  5
#define SVCSET_XP_RECV  6
#define SVCGET_XP_FLAGS  7
#define SVCSET_XP_FLAGS  8
#define SVCGET_XP_GETREQ        9
#define SVCSET_XP_GETREQ        10
#define SVCGET_XP_DISPATCH      11
#define SVCSET_XP_DISPATCH      12
#define SVCGET_XP_RDVS          13
#define SVCSET_XP_RDVS          14
#define SVCGET_XP_FREE_XPRT     15
#define SVCSET_XP_FREE_XPRT     16

/*
 * Operations for rpc_control().
 */
#define RPC_SVC_CONNMAXREC_SET  0	/* set max rec size, enable nonblock */
#define RPC_SVC_CONNMAXREC_GET  1
#define RPC_SVC_XPRTS_GET       2
#define RPC_SVC_XPRTS_SET       3
#define RPC_SVC_FDSET_GET       4
#define RPC_SVC_FDSET_SET       5

/*
 * Flags for svc_fd_ncreate2
 */

#define SVC_VCCR_NONE             0x0000
#define SVC_VCCR_RADDR            0x0001
#define SVC_VCCR_RADDR_INET       0x0002
#define SVC_VCCR_RADDR_INET6      0x0004
#define SVC_VCCR_RADDR_LOCAL      0x0008
#define SVC_VCCR_MAP6_V1          0x0010

/* Svc event strategy */
enum svc_event_type {
	SVC_EVENT_FDSET /* trad. using select and poll (currently unhooked) */ ,
	SVC_EVENT_EPOLL		/* Linux epoll interface */
};

typedef struct svc_init_params {
	u_long flags;
	u_int max_connections;	/* xprts */
	u_int max_events;	/* evchan events */
	u_int svc_ioq_maxbuf;
	int32_t idle_timeout;
	u_int gss_ctx_hash_partitions;
	u_int gss_max_ctx;
	u_int gss_max_idle_gen;
	u_int gss_max_gc;
	warnx_t warnx;
} svc_init_params;

/* Svc param flags */
#define SVC_FLAG_NONE             0x0000
#define SVC_FLAG_NOREG_XPRTS      0x0001

/*
 * SVCXPRT xp_flags
 */

#define SVC_XPRT_FLAG_NONE               0x0000
#define SVC_XPRT_FLAG_SETNEWFDS          0x0001
#define SVC_XPRT_FLAG_DONTCLOSE          0x0002
#define SVC_XPRT_FLAG_EVCHAN             0x0004
#define SVC_XPRT_FLAG_GCHAN              0x0008
#define SVC_XPRT_FLAG_COPY               0x0010	/* XXX */
#define SVC_XPRT_FLAG_DESTROYED          0x0020
#define SVC_XPRT_FLAG_DESTROYING         0x0040

/*
 * SVC_REF flags
 */

#define SVC_REF_FLAG_NONE            0x0000
#define SVC_REF_FLAG_LOCKED          0x0001

/*
 * SVC_RELEASE flags
 */

#define SVC_RELEASE_FLAG_NONE            0x0000
#define SVC_RELEASE_FLAG_LOCKED          0x0001

/* XXX Ganesha
 * Don't confuse with (currently incomplete) transport type, nee
 * socktype. */
typedef enum xprt_type {
	XPRT_UNKNOWN,
	XPRT_UDP,
	XPRT_TCP,
	XPRT_TCP_RENDEZVOUS,
	/* 6 types? */
	/* placeholders */
	XPRT_SCTP,
	XPRT_RDMA
} xprt_type_t;

enum xprt_stat {
	XPRT_DIED,
	XPRT_MOREREQS,
	XPRT_IDLE,
	XPRT_DESTROYED
};

struct cf_rendezvous {		/* kept in xprt->xp_p1 for rendezvouser */
	u_int sendsize;
	u_int recvsize;
	int maxrec;
};

struct SVCAUTH;			/* forward decl. */

/*
 * Server side transport handle
 */
typedef struct rpc_svcxprt {
	int xp_fd;
	u_short xp_port;	/* associated port number */
	struct xp_ops {
		/* receive incoming requests */
		bool(*xp_recv) (struct rpc_svcxprt *, struct svc_req *);

		/* get transport status */
		enum xprt_stat (*xp_stat) (struct rpc_svcxprt *);

		/* get arguments, thread u_data in arg4 */
		bool(*xp_getargs) (struct rpc_svcxprt *, struct svc_req *,
				    xdrproc_t, void *, void *);

		/* send reply */
		bool(*xp_reply) (struct rpc_svcxprt *, struct svc_req *,
				  struct rpc_msg *);

		/* free mem allocated for args */
		 bool(*xp_freeargs) (struct rpc_svcxprt *, xdrproc_t, void *);

		/* take lifecycle ref */
		bool(*xp_ref) (struct rpc_svcxprt *, u_int,
			       const char *, const int);

		/* release ref (destroy if no refs) */
		void (*xp_release) (struct rpc_svcxprt *, u_int,
				    const char *, const int);

		/* release and mark destroyed */
		void (*xp_destroy) (struct rpc_svcxprt *);

		/* xprt locking (may be duplex-aware, etc) */
		void (*xp_lock) (struct rpc_svcxprt *, uint32_t,
				 const char *, int);
		void (*xp_unlock) (struct rpc_svcxprt *, uint32_t,
				   const char *, int);

	} *xp_ops;

	int xp_addrlen;		/* length of remote address */
	struct sockaddr_in6 xp_raddr;	/* remote addr (backward ABI compat) */

	struct xp_ops2 {
		/* catch-all function */
		bool(*xp_control) (struct rpc_svcxprt *, const u_int, void *);
		/* handle incoming requests (calls xp_recv) */
		bool(*xp_getreq) (struct rpc_svcxprt *);
		/* call dispatch strategy function */
		void (*xp_dispatch) (struct rpc_svcxprt *, struct rpc_msg **);
		/* rendezvous (epilogue) */
		 u_int(*xp_rdvs) (struct rpc_svcxprt *, struct rpc_svcxprt *,
				  const u_int, void *);
		/* xprt free hook */
		 bool(*xp_free_xprt) (struct rpc_svcxprt *);
	} *xp_ops2;

	char *xp_tp;		/* transport provider device name */
	char *xp_netid;		/* network token */
	struct netbuf xp_ltaddr;	/* local transport address */
	struct netbuf xp_rtaddr;	/* remote transport address */

	/* auth */
	mutex_t xp_auth_lock;	/* lock owned by installed authenticator */

	/* serialize private data */
	mutex_t xp_lock;
	uint32_t xp_refcnt;	/* handle refcnt */

	void *xp_ev;		/* event handle */
	void *xp_p1;		/* private: for use by svc ops */
	void *xp_p2;		/* private: for use by svc ops */
	void *xp_p3;		/* private: for use by svc lib */
	void *xp_p4;		/* private: for use by svc lib */
	void *xp_p5;		/* private: for use by svc lib */
	void *xp_u1;		/* client user data */
	int xp_si_type;		/* si type */
	int xp_type;		/* xprt type */
	u_int xp_flags;		/* flags */
	uint64_t xp_gen;	/* svc_xprt generation number */

} SVCXPRT;

/* Service record used by exported search routines */
typedef struct svc_record {
	rpcprog_t sc_prog;
	rpcvers_t sc_vers;
	char *sc_netid;
	void (*sc_dispatch) (struct svc_req *, SVCXPRT *);
} svc_rec_t;

typedef struct svc_vers_range {
	rpcvers_t lowvers;
	rpcvers_t highvers;
} svc_vers_range_t;

typedef enum svc_lookup_result {
	SVC_LKP_SUCCESS = 0,
	SVC_LKP_PROG_NOTFOUND = 1,
	SVC_LKP_VERS_NOTFOUND = 2,
	SVC_LKP_NETID_NOTFOUND = 3,
	SVC_LKP_ERR = 667,
} svc_lookup_result_t;

/* functions which can be installed using a control function, e.g.,
 * xp_ops2->xp_control */
typedef bool(*xp_recv_t) (struct rpc_svcxprt *, struct svc_req *);
typedef bool(*xp_getreq_t) (struct rpc_svcxprt *);
typedef void (*xp_dispatch_t) (struct rpc_svcxprt *, struct rpc_msg **);
typedef u_int(*xp_rdvs_t) (struct rpc_svcxprt *, struct rpc_svcxprt *,
			   const u_int, void *);
typedef bool(*xp_free_xprt_t) (struct rpc_svcxprt *);

/*
 * Service request
 */
struct svc_req {
	u_int32_t rq_prog;	/* service program number */
	u_int32_t rq_vers;	/* service protocol version */
	u_int32_t rq_proc;	/* the desired procedure */

	struct opaque_auth rq_cred;	/* raw creds from the wire */
	void *rq_clntcred;	/* read only cooked cred */
	SVCXPRT *rq_xprt;	/* associated transport */

	/* New with TI-RPC */
	caddr_t rq_clntname;	/* read only client name */
	caddr_t rq_svcname;	/* read only cooked service cred */

	/* New with N TI-RPC */
	u_int32_t rq_xid;	/* xid */
	struct rpc_msg *rq_msg;	/* decoded rpc_msg */
	uint64_t rq_cksum;
	void *rq_u1;		/* user data */
	void *rq_u2;		/* user data */

	/* Moved in N TI-RPC */
	struct opaque_auth rq_verf;	/* raw response verifier */
	struct SVCAUTH *rq_auth;	/* auth handle */
	void *rq_ap1;		/* auth private */
	void *rq_ap2;		/* auth private */

	struct netbuf rq_rtaddr;	/* remote transport address */
};

/*
 *  Approved way of getting address of caller
 */
#define svc_getrpccaller(x) (&(x)->xp_rtaddr)

/*
 * Ganesha.  Get connected transport type.
 */
#define svc_get_xprt_type(x) ((x)->xp_type);

/*
 * Ganesha.  Original TI-RPC si type.
 */
#define svc_get_xprt_si_type(x) ((x)->xp_si_type);

/*
 * Operations defined on an SVCXPRT handle
 *
 * SVCXPRT *xprt;
 * struct svc_req *req;
 * xdrproc_t xargs;
 * void * argsp;
 */
#define SVC_RECV(xprt, req) \
	(*(xprt)->xp_ops->xp_recv)((xprt), (req))
#define svc_recv(xprt, req)			\
	(*(xprt)->xp_ops->xp_recv)((xprt), (req))

#define SVC_STAT(xprt) \
	(*(xprt)->xp_ops->xp_stat)(xprt)
#define svc_stat(xprt)				\
	(*(xprt)->xp_ops->xp_stat)(xprt)

#define SVC_GETARGS(xprt, req, xargs, argsp, u_data)			\
	(*(xprt)->xp_ops->xp_getargs)((xprt), (req), (xargs), (argsp),	\
				      (u_data))
#define svc_getargs(xprt, req, xargs, argsp, u_data)		       \
	(*(xprt)->xp_ops->xp_getargs)((xprt), (req), (xargs), (argsp), \
				      (u_data))

#define SVC_REPLY(xprt, req, msg)				\
	(*(xprt)->xp_ops->xp_reply) ((xprt), (req), (msg))
#define svc_reply(xprt, req, msg) \
	(*(xprt)->xp_ops->xp_reply) ((xprt), (req), (msg))

#define SVC_FREEARGS(xprt, xargs, argsp)			\
	(*(xprt)->xp_ops->xp_freeargs)((xprt), (xargs), (argsp))
#define svc_freeargs(xprt, xargs, argsp)			\
	(*(xprt)->xp_ops->xp_freeargs)((xprt), (xargs), (argsp))

#define SVC_REF2(xprt, flags, tag, line)		\
	(*(xprt)->xp_ops->xp_ref)(xprt, flags, tag, line)
#define svc_ref2(xprt, flags, tag, line)		\
	(*(xprt)->xp_ops->xp_ref)(xprt, flags, tag, line)

#define SVC_RELEASE2(xprt, flags, tag, line) \
	(*(xprt)->xp_ops->xp_release)(xprt, flags, tag, line)
#define svc_release2(xprt, flags, tag, line)			\
	(*(xprt)->xp_ops->xp_release)(xprt, flags, tag, line)

#define SVC_REF(xprt, flags)					\
	(*(xprt)->xp_ops->xp_ref)(xprt, flags, __func__, __LINE__)
#define svc_ref(xprt, flags)					\
	(*(xprt)->xp_ops->xp_ref)(xprt, flags, __func__, __LINE__)

#define SVC_RELEASE(xprt, flags)					\
	(*(xprt)->xp_ops->xp_release)(xprt, flags, __func__, __LINE__)
#define svc_release(xprt, flags)					\
	(*(xprt)->xp_ops->xp_release)(xprt, flags, __func__, __LINE__)

#define SVC_DESTROY(xprt)			\
	(*(xprt)->xp_ops->xp_destroy)(xprt)
#define svc_destroy(xprt)			\
	(*(xprt)->xp_ops->xp_destroy)(xprt)

#define SVC_CONTROL(xprt, rq, in)			\
	(*(xprt)->xp_ops2->xp_control)((xprt), (rq), (in))

#define XP_LOCK_NONE    0x0000
#define XP_LOCK_SEND    0x0001
#define XP_LOCK_RECV    0x0002

#define SVC_LOCK(xprt, flags, func, line) \
	(*(xprt)->xp_ops->xp_lock)((xprt), (flags), (func), (line))

#define svc_lock(xprt, flags, func, line) \
	(*(xprt)->xp_ops->xp_lock)((xprt), (flags), (func), (line))

#define SVC_UNLOCK(xprt, flags, func, line) \
	(*(xprt)->xp_ops->xp_unlock)((xprt), (flags), (func), (line))

#define svc_unlock(xprt, flags, func, line) \
	(*(xprt)->xp_ops->xp_unlock)((xprt), (flags), (func), (line))

/*
 * Service init (optional).
 */

__BEGIN_DECLS
void svc_init(struct svc_init_params *);
__END_DECLS
/*
 * Service shutdown (optional).
 */
__BEGIN_DECLS
int svc_shutdown(u_long flags);
__END_DECLS
/*
 * Service registration
 *
 * svc_reg(xprt, prog, vers, dispatch, nconf)
 * const SVCXPRT *xprt;
 * const rpcprog_t prog;
 * const rpcvers_t vers;
 * const void (*dispatch)();
 * const struct netconfig *nconf;
 */
__BEGIN_DECLS
extern bool svc_reg(SVCXPRT *, const rpcprog_t, const rpcvers_t,
		    void (*)(struct svc_req *, SVCXPRT *),
		    const struct netconfig *);
__END_DECLS
/*
 * Service un-registration
 *
 * svc_unreg(prog, vers)
 * const rpcprog_t prog;
 * const rpcvers_t vers;
 */
__BEGIN_DECLS
extern void svc_unreg(const rpcprog_t, const rpcvers_t);
__END_DECLS
/*
 * Transport registration.
 *
 * xprt_register(xprt)
 * SVCXPRT *xprt;
 */
__BEGIN_DECLS
extern void xprt_register(SVCXPRT *);
__END_DECLS
/*
 * Transport un-register
 *
 * xprt_unregister(xprt)
 * SVCXPRT *xprt;
 */
__BEGIN_DECLS
extern void xprt_unregister(SVCXPRT *);
__END_DECLS
/*
 * This is used to set xprt->xp_raddr in a way legacy
 * apps can deal with
 *
 * __xprt_set_raddr(xprt, ss)
 * SVCXPRT *xprt;
 *      const struct sockaddr_storage *ss;
 */
__BEGIN_DECLS
extern void __xprt_set_raddr(SVCXPRT *,
			     const struct sockaddr_storage *);
__END_DECLS
/*
 * When the service routine is called, it must first check to see if it
 * knows about the procedure;  if not, it should call svcerr_noproc
 * and return.  If so, it should deserialize its arguments via
 * SVC_GETARGS (defined above).  If the deserialization does not work,
 * svcerr_decode should be called followed by a return.  Successful
 * decoding of the arguments should be followed the execution of the
 * procedure's code and a call to svc_sendreply.
 *
 * Also, if the service refuses to execute the procedure due to too-
 * weak authentication parameters, svcerr_weakauth should be called.
 * Note: do not confuse access-control failure with weak authentication!
 *
 * NB: In pure implementations of rpc, the caller always waits for a reply
 * msg.  This message is sent when svc_sendreply is called.
 * Therefore pure service implementations should always call
 * svc_sendreply even if the function logically returns void;  use
 * xdr.h - xdr_void for the xdr routine.  HOWEVER, tcp based rpc allows
 * for the abuse of pure rpc via batched calling or pipelining.  In the
 * case of a batched call, svc_sendreply should NOT be called since
 * this would send a return message, which is what batching tries to avoid.
 * It is the service/protocol writer's responsibility to know which calls are
 * batched and which are not.  Warning: responding to batch calls may
 * deadlock the caller and server processes!
 */
__BEGIN_DECLS
extern bool svc_sendreply(SVCXPRT *, struct svc_req *, xdrproc_t,
			  void *);
extern void svcerr_decode(SVCXPRT *, struct svc_req *);
extern void svcerr_weakauth(SVCXPRT *, struct svc_req *);
extern void svcerr_noproc(SVCXPRT *, struct svc_req *);
extern void svcerr_progvers(SVCXPRT *, struct svc_req *, rpcvers_t, rpcvers_t);
extern void svcerr_auth(SVCXPRT *, struct svc_req *, enum auth_stat);
extern void svcerr_noprog(SVCXPRT *, struct svc_req *);
extern void svcerr_systemerr(SVCXPRT *, struct svc_req *);
extern int rpc_reg(rpcprog_t, rpcvers_t, rpcproc_t, char *(*)(char *),
		   xdrproc_t, xdrproc_t, char *);
__END_DECLS
/*
 * a small program implemented by the svc_rpc implementation itself;
 * also see clnt.h for protocol numbers.
 */
__BEGIN_DECLS
extern void rpctest_service(void);
__END_DECLS
__BEGIN_DECLS
extern void svc_getreq(int);
extern void svc_getreqset(fd_set *);
extern void svc_getreq_common(int);
struct pollfd;
extern void svc_getreq_poll(struct pollfd *, int);
extern void svc_run(void);
extern void svc_exit(void);
__END_DECLS
/*
 * Socket to use on svcxxx_ncreate call to get default socket
 */
#define RPC_ANYSOCK -1
#define RPC_ANYFD RPC_ANYSOCK
/*
 * These are the existing service side transport implementations
 */
__BEGIN_DECLS
/*
 * Transport independent svc_create routine.
 */
extern int svc_ncreate(void (*)(struct svc_req *, SVCXPRT *), const rpcprog_t,
		       const rpcvers_t, const char *);
/*
 *      void (*dispatch)();             -- dispatch routine
 *      const rpcprog_t prognum;        -- program number
 *      const rpcvers_t versnum;        -- version number
 *      const char *nettype;            -- network type
 */

/*
 * Generic server creation routine. It takes a netconfig structure
 * instead of a nettype.
 */

extern SVCXPRT *svc_tp_ncreate(void (*)(struct svc_req *, SVCXPRT *),
			       const rpcprog_t, const rpcvers_t,
			       const struct netconfig *);
/*
 * void (*dispatch)();            -- dispatch routine
 * const rpcprog_t prognum;       -- program number
 * const rpcvers_t versnum;       -- version number
 * const struct netconfig *nconf; -- netconfig structure
 */

/*
 * Generic TLI create routine
 */
extern SVCXPRT *svc_tli_ncreate(const int, const struct netconfig *,
				const struct t_bind *, const u_int,
				const u_int);
/*
 *      const int fd;                   -- connection end point
 *      const struct netconfig *nconf;  -- netconfig structure for network
 *      const struct t_bind *bindaddr;  -- local bind address
 *      const u_int sendsz;             -- max sendsize
 *      const u_int recvsz;             -- max recvsize
 */

/*
 * Connectionless and connectionful create routines
 */

extern SVCXPRT *svc_vc_ncreate(const int, const u_int, const u_int);
/*
 *      const int fd;                           -- open connection end point
 *      const u_int sendsize;                   -- max send size
 *      const u_int recvsize;                   -- max recv size
 */

extern SVCXPRT *svc_vc_ncreate2(const int, const u_int, const u_int,
				const u_int);
/*
 *      const int fd;                           -- open connection end point
 *      const u_int sendsize;                   -- max send size
 *      const u_int recvsize;                   -- max recv size
 *      const u_int flags;                      -- flags
 */

__END_DECLS
#define SVC_VC_CREATE_NONE             0x0000
#define SVC_VC_CREATE_BOTHWAYS         0x0001
#define SVC_VC_CREATE_ONEWAY           0x0002	/* !bothways */
#define SVC_VC_CREATE_DISPOSE          0x0004	/* !bothways */
#define SVC_VC_CREATE_XPRT_NOREG       0x0008
#define SVC_VC_CREATE_LISTEN           0x0010

__BEGIN_DECLS
/*
 * Create a client handle from an active service transport handle.
 */
extern CLIENT *clnt_vc_ncreate_svc(SVCXPRT *, const rpcprog_t, const rpcvers_t,
				   const uint32_t);
/*
 *      SVCXPRT *xprt;                          -- active service xprt
 *      const rpcprog_t prog;                   -- RPC program number
 *      const rpcvers_t vers;                   -- RPC program version
 */

__END_DECLS
__BEGIN_DECLS
/*
 * Create an RPC SVCXPRT handle from an active client transport
 * handle, i.e., to service RPC requests
 */
extern SVCXPRT *svc_vc_ncreate_clnt(CLIENT *, u_int, u_int, const uint32_t);
/*
 *
 * CLIENT *cl;                                  -- connected client
 * const u_int sendsize;                        -- max send size
 * const u_int recvsize;                        -- max recv size
 * const uint32_t flags;                        -- flags
 */

/*
 * Destroy a transport handle.  Do not alter connected transport state.
 */
extern void svc_vc_destroy_handle(SVCXPRT *);

/*
 * Construct a service transport, unassociated with any transport
 * connection.
 */
extern SVCXPRT *svc_vc_create_xprt(u_long, u_long);

/*
 * Destroy a transport handle.  Do not alter connected transport state.
 */
extern void svc_vc_destroy_xprt(SVCXPRT *);

/*
 * Added for compatibility to old rpc 4.0. Obsoleted by svc_vc_create().
 */
extern SVCXPRT *svcunix_ncreate(int, u_int, u_int, char *);

extern SVCXPRT *svc_dg_ncreate(const int, const u_int, const u_int);
/*
 * const int fd;                                -- open connection
 * const u_int sendsize;                        -- max send size
 * const u_int recvsize;                        -- max recv size
 */

/*
 * the routine takes any *open* connection
 * descriptor as its first input and is used for open connections.
 */
extern SVCXPRT *svc_fd_ncreate(const int, const u_int, const u_int);
/*
 *      const int fd;                           -- open connection end point
 *      const u_int sendsize;                   -- max send size
 *      const u_int recvsize;                   -- max recv size
 */

/*
 * Added for compatibility to old rpc 4.0. Obsoleted by svc_fd_create().
 */
extern SVCXPRT *svcunixfd_ncreate(int, u_int, u_int);

/*
 * Memory based rpc (for speed check and testing)
 */
extern SVCXPRT *svc_raw_ncreate(void);

/*
 * Getreq plug-out prototype
 */
extern bool svc_getreq_default(SVCXPRT *);

/*
 * Dispatch plug-out prototype
 */
extern void svc_dispatch_default(SVCXPRT *, struct rpc_msg **);

/*
 * Convenience functions for implementing these
 */
extern bool svc_validate_xprt_list(SVCXPRT *);
extern struct rpc_msg *alloc_rpc_msg(void);
extern void free_rpc_msg(struct rpc_msg *);

/*
 * svc_dg_enable_cache() enables the cache on dg transports.
 */
int svc_dg_enablecache(SVCXPRT *, const u_int);

int __rpc_get_local_uid(SVCXPRT *, uid_t *);

#define XPRT_TRACE_RADDR(xprt, func, tag, line)				\
	if (__pkg_params.debug_flags & TIRPC_DEBUG_FLAG_REFCNT) {	\
		xprt_trace_raddr((xprt), (func), (tag), (line));	\
	}

void xprt_trace_raddr(SVCXPRT *, const char *, const char *,
		      const int);

__END_DECLS
/* for backward compatibility */
#include <rpc/svc_soc.h>
#include <rpc/tirpc_compat.h>
#endif				/* !_TIRPC_SVC_H */
