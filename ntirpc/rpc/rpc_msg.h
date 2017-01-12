/* $NetBSD: rpc_msg.h,v 1.11 2000/06/02 22:57:56 fvdl Exp $ */

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
 * from: @(#)rpc_msg.h 1.7 86/07/16 SMI
 * from: @(#)rpc_msg.h 2.1 88/07/29 4.0 RPCSRC
 * $FreeBSD: src/include/rpc/rpc_msg.h,v 1.15 2003/01/01 18:48:42 schweikh Exp $
 */

/*
 * rpc_msg.h
 * rpc message definition
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */

#ifndef _TIRPC_RPC_MSG_H
#define _TIRPC_RPC_MSG_H

#define RPC_MSG_VERSION  ((u_int32_t) 2)
#define RPC_SERVICE_PORT ((u_short) 2048)

#include <rpc/auth.h>
#include <rpc/clnt.h>
#include <misc/queue.h>

/*
 * Bottom up definition of an rpc message.
 * NOTE: call and reply use the same overall stuct but
 * different parts of unions within it.
 */

enum msg_type {
	CALL = 0,
	REPLY = 1
};

enum reply_stat {
	MSG_ACCEPTED = 0,
	MSG_DENIED = 1
};

enum accept_stat {
	SUCCESS = 0,
	PROG_UNAVAIL = 1,
	PROG_MISMATCH = 2,
	PROC_UNAVAIL = 3,
	GARBAGE_ARGS = 4,
	SYSTEM_ERR = 5
};

enum reject_stat {
	RPC_MISMATCH = 0,
	AUTH_ERROR = 1
};

/*
 * Reply part of an rpc exchange
 */

/*
 * Reply to an rpc request that was accepted by the server.
 * Note: there could be an error even though the request was
 * accepted.
 */
struct accepted_reply {
	enum accept_stat ar_stat;
	union {
		struct {
			rpcvers_t low;
			rpcvers_t high;
		} AR_versions;
		struct {
			caddr_t where;
			xdrproc_t proc;
		} AR_results;
		/* and many other null cases */
	} ru;
#define ar_results ru.AR_results
#define ar_vers  ru.AR_versions

	/* after union to avoid (rare) corruption by rejected_reply */
	struct opaque_auth ar_verf;
};

/*
 * Reply to an rpc request that was rejected by the server.
 */
struct rejected_reply {
	enum reject_stat rj_stat;
	union {
		struct {
			rpcvers_t low;
			rpcvers_t high;
		} RJ_versions;
		enum auth_stat RJ_why;	/* why authentication did not work */
	} ru;
#define rj_vers ru.RJ_versions
#define rj_why ru.RJ_why
};

/*
 * Body of a reply to an rpc request.
 */
struct reply_body {
	enum reply_stat rp_stat;
	union {
		struct accepted_reply RP_ar;
		struct rejected_reply RP_dr;
	} ru;
#define rp_acpt ru.RP_ar
#define rp_rjct ru.RP_dr
};

/*
 * Body of an rpc request call.
 */
struct call_body {
	rpcvers_t cb_rpcvers;	/* must be equal to two */
};

/*
 * The rpc message
 */

#define RPC_MSG_FLAG_NONE       0x0000

struct rpc_msg {
	u_int32_t rm_xid;
	enum msg_type rm_direction;
	struct {
		struct call_body RM_cmb;
		struct reply_body RM_rmb;
	} ru;
#define rm_call  ru.RM_cmb
#define rm_reply ru.RM_rmb

	int32_t *rm_ibuf;
	uint32_t rm_flags;

	/* Moved in N TI-RPC; used by auth, logging, replies */
	rpcprog_t cb_prog;
	rpcvers_t cb_vers;
	rpcproc_t cb_proc;

	struct opaque_auth cb_cred;
	struct opaque_auth cb_verf; /* protocol specific - provided by client */

	/* avoid separate alloc/free */
	char rq_cred_body[MAX_AUTH_BYTES];	/* size is excessive */
};
#define RPCM_ack ru.RM_rmb.ru.RP_ar
#define RPCM_rej ru.RM_rmb.ru.RP_dr

__BEGIN_DECLS
/*
 * XDR routine to handle a rpc message.
 * xdr_ncallmsg(xdrs, cmsg)
 *  XDR *xdrs;
 *  struct rpc_msg *cmsg;
 */
extern bool xdr_ncallmsg(XDR *, struct rpc_msg *);
extern bool xdr_call_decode(XDR *, struct rpc_msg *, int32_t *buf);
extern bool xdr_call_encode(XDR *, struct rpc_msg *);

/*
 * XDR routine to handle a duplex rpc message.
 * xdr_dplx_msg(xdrs, cmsg)
 *  XDR *xdrs;
 *  struct rpc_msg *cmsg;
 */
extern bool xdr_dplx_msg(XDR *, struct rpc_msg *);
extern bool xdr_dplx_decode(XDR *, struct rpc_msg *);
extern bool xdr_reply_decode(XDR *, struct rpc_msg *, int32_t *buf);
extern bool xdr_reply_encode(XDR *, struct rpc_msg *);

/*
 * XDR routine to pre-serialize the static part of a rpc message.
 * xdr_ncallhdr(xdrs, cmsg)
 *  XDR *xdrs;
 *  struct rpc_msg *cmsg;
 */
extern bool xdr_ncallhdr(XDR *, struct rpc_msg *);

/*
 * XDR routine to handle a rpc reply.
 * xdr_nreplymsg(xdrs, rmsg)
 *  XDR *xdrs;
 *  struct rpc_msg *rmsg;
 */
extern bool xdr_nreplymsg(XDR *, struct rpc_msg *);

/*
 * XDR routine to handle an accepted rpc reply.
 * xdr_accepted_reply(xdrs, rej)
 *  XDR *xdrs;
 *  struct accepted_reply *rej;
 */
extern bool xdr_naccepted_reply(XDR *, struct accepted_reply *);

/*
 * XDR routine to handle a rejected rpc reply.
 * xdr_rejected_reply(xdrs, rej)
 *  XDR *xdrs;
 *  struct rejected_reply *rej;
 */
extern bool xdr_nrejected_reply(XDR *, struct rejected_reply *);

/*
 * Fills in the error part of a reply message.
 * _seterr_reply(msg, error)
 *  struct rpc_msg *msg;
 *  struct rpc_err *error;
 */
extern void _seterr_reply(struct rpc_msg *, struct rpc_err *);
__END_DECLS
/* For backward compatibility */
#include <rpc/tirpc_compat.h>
#endif				/* !_TIRPC_RPC_MSG_H */
