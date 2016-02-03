/*
  svc_auth_gss.c

  Copyright (c) 2000 The Regents of the University of Michigan.
  All rights reserved.

  Copyright (c) 2000 Dug Song <dugsong@UMICH.EDU>.
  All rights reserved, all wrongs reversed.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  3. Neither the name of the University nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>
#include "rpc_com.h"
#include <rpc/gss_internal.h>
#include <misc/portable.h>

static bool svcauth_gss_wrap(SVCAUTH *, struct svc_req *, XDR *, xdrproc_t,
			     caddr_t);

static bool svcauth_gss_unwrap(SVCAUTH *, struct svc_req *, XDR *, xdrproc_t,
			       caddr_t);

static bool svcauth_gss_release(SVCAUTH *, struct svc_req *);

bool svcauth_gss_destroy(SVCAUTH *);

struct svc_auth_ops svc_auth_gss_ops = {
	svcauth_gss_wrap,
	svcauth_gss_unwrap,
	svcauth_gss_release,
	svcauth_gss_destroy
};

#define SVCAUTH_PRIVATE(auth) \
	((struct svc_rpc_gss_data *)(auth)->svc_ah_private)

/* Global server credentials. */
gss_cred_id_t svcauth_gss_creds;
static gss_name_t svcauth_gss_name;
static int64_t svcauth_gss_creds_expires;
static mutex_t svcauth_gss_creds_lock = MUTEX_INITIALIZER;
static gss_cred_id_t svcauth_prev_gss_creds;

bool
svcauth_gss_set_svc_name(gss_name_t name)
{
	OM_uint32 maj_stat, min_stat;

	if (svcauth_gss_name != NULL) {
		maj_stat = gss_release_name(&min_stat, &svcauth_gss_name);
		if (maj_stat != GSS_S_COMPLETE)
			return (false);
		svcauth_gss_name = NULL;
	}

	/* XXX Ganesha */
	if (svcauth_gss_name == GSS_C_NO_NAME)
		return (true);

	maj_stat = gss_duplicate_name(&min_stat, name, &svcauth_gss_name);
	if (maj_stat != GSS_S_COMPLETE)
		return (false);

	return (true);
}

bool
svcauth_gss_import_name(char *service)
{
	gss_name_t name;
	gss_buffer_desc namebuf;
	OM_uint32 maj_stat, min_stat;

	namebuf.value = service;
	namebuf.length = strlen(service);

	maj_stat =
	    gss_import_name(&min_stat, &namebuf,
			    (gss_OID) GSS_C_NT_HOSTBASED_SERVICE, &name);
	if (maj_stat != GSS_S_COMPLETE)
		return (false);

	if (svcauth_gss_set_svc_name(name) != true) {
		gss_release_name(&min_stat, &name);
		return (false);
	}

	/* discard duplicate name */
	gss_release_name(&min_stat, &name);

	return (true);
}

#ifdef __APPLE__
/* there's also mach_absolute_time() - don't know if it's faster */
#define get_time_fast()	time(0)
#else
static inline int64_t
get_time_fast(void)
{
	struct timespec ts[1];
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, ts);
	return ts->tv_sec;
}
#endif

bool
svcauth_gss_acquire_cred(void)
{
	OM_uint32 maj_stat, min_stat;
	int64_t now;
	OM_uint32 timerec;
	gss_cred_id_t old_creds, ancient_creds;

	now = get_time_fast();
	if (svcauth_gss_creds && (!svcauth_gss_creds_expires || svcauth_gss_creds_expires > now))
		return (true);

	mutex_lock(&svcauth_gss_creds_lock);
	if (svcauth_gss_creds && (!svcauth_gss_creds_expires || svcauth_gss_creds_expires > now)) {
		maj_stat = GSS_S_COMPLETE;
	} else {
		ancient_creds = svcauth_prev_gss_creds;
		old_creds = svcauth_gss_creds;
		timerec = 0;
		now = get_time_fast();
		maj_stat =
		    gss_acquire_cred(&min_stat, svcauth_gss_name, 0, GSS_C_NULL_OID_SET,
				     GSS_C_ACCEPT, &svcauth_gss_creds, NULL, &timerec);
		if (maj_stat == GSS_S_COMPLETE) {
			if (timerec == GSS_C_INDEFINITE)
				svcauth_gss_creds_expires = 0;
			else
				svcauth_gss_creds_expires = now + timerec;
			if (old_creds) {
				svcauth_prev_gss_creds = old_creds;
			}
			if (ancient_creds) {
				(void) gss_release_cred(&min_stat, &ancient_creds);
			}
		}
	}
	mutex_unlock(&svcauth_gss_creds_lock);
	if (maj_stat != GSS_S_COMPLETE)
		return (false);

	return (true);
}

bool
svcauth_gss_release_cred(void)
{
#if 0
	OM_uint32 maj_stat, min_stat;

	maj_stat = gss_release_cred(&min_stat, &svcauth_gss_creds);
	if (maj_stat != GSS_S_COMPLETE)
		return (false);
	svcauth_gss_creds = NULL;
#else
	svcauth_gss_creds_expires = 1;
	/* make any future callers to svcauth_gss_acquire_cred() should get a new cred. */
#endif

	return (true);
}

static bool
svcauth_gss_accept_sec_context(struct svc_req *req,
			       struct svc_rpc_gss_data *gd,
			       struct rpc_gss_init_res *gr)
{
	struct rpc_gss_cred *gc;
	gss_buffer_desc recv_tok, seqbuf, checksum;
	gss_OID mech;
	OM_uint32 maj_stat = 0, min_stat = 0, ret_flags, seq;
#define INDEF_EXPIRE 60*60*24	/* from mit k5 src/lib/rpc/svc_auth_gssapi.c */
	OM_uint32 time_rec;

	gc = (struct rpc_gss_cred *)req->rq_clntcred;
	memset(gr, 0, sizeof(*gr));

	/* Deserialize arguments. */
	memset(&recv_tok, 0, sizeof(recv_tok));

	if (!svc_getargs
	    (req->rq_xprt, req, (xdrproc_t) xdr_rpc_gss_init_args,
	     (caddr_t) &recv_tok, NULL /* u_data */))
		return (false);

	gr->gr_major =
	    gss_accept_sec_context(&gr->gr_minor, &gd->ctx, svcauth_gss_creds,
				   &recv_tok, GSS_C_NO_CHANNEL_BINDINGS,
				   &gd->client_name, &mech, &gr->gr_token,
				   &ret_flags, &time_rec, NULL);

	svc_freeargs(req->rq_xprt, req, (xdrproc_t) xdr_rpc_gss_init_args,
		     (caddr_t) &recv_tok);

	if ((gr->gr_major != GSS_S_COMPLETE)
	    && (gr->gr_major != GSS_S_CONTINUE_NEEDED)) {
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: auth failed major=%u minor=%u", __func__,
			gr->gr_major, gr->gr_minor);
		gd->ctx = GSS_C_NO_CONTEXT;
		gss_release_buffer(&min_stat, &gr->gr_token);
		return (false);
	}
	/* ANDROS: krb5 mechglue returns ctx of size 8 - two pointers,
	 * one to the mechanism oid, one to the internal_ctx_id */
	gr->gr_ctx.value = mem_alloc(sizeof(gss_union_ctx_id_desc));
	memcpy(gr->gr_ctx.value, gd->ctx, sizeof(gss_union_ctx_id_desc));
	gr->gr_ctx.length = sizeof(gss_union_ctx_id_desc);

	/* ANDROS: change for debugging linux kernel version...
	   gr->gr_win = 0x00000005;
	 */
	gr->gr_win = sizeof(gd->seqmask) * 8;

	/* Save client info. */
	gd->sec.mech = mech;
	gd->sec.qop = GSS_C_QOP_DEFAULT;
	gd->sec.svc = gc->gc_svc;
	gd->win = gr->gr_win;

	if (time_rec == GSS_C_INDEFINITE) time_rec = INDEF_EXPIRE;
	if (time_rec > 10) time_rec -= 5;
	gd->endtime = time_rec + get_time_fast();

	if (gr->gr_major == GSS_S_COMPLETE) {
		maj_stat =
		    gss_display_name(&min_stat, gd->client_name, &gd->cname,
				     &gd->sec.mech);
		if (maj_stat != GSS_S_COMPLETE) {
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: display_name major=%u minor=%u", __func__,
				maj_stat, min_stat);
			gss_release_buffer(&min_stat, &gr->gr_token);
			return (false);
		}
#ifdef DEBUG
#ifdef HAVE_KRB5
		{
			gss_buffer_desc mechname;
			gss_oid_to_str(&min_stat, mech, &mechname);
			__warnx(TIRPC_DEBUG_FLAG_AUTH,
				"%s: accepted context for %.*s with "
				"<mech %.*s, qop %d, svc %d>", __func__,
				gd->cname.length, (char *)gd->cname.value,
				mechname.length, (char *)mechname.value,
				gd->sec.qop, gd->sec.svc);
			gss_release_buffer(&min_stat, &mechname);
		}
#elif HAVE_HEIMDAL
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"%s: accepted context for %.*s with "
			"<mech {}, qop %d, svc %d>", __func__, gd->cname.length,
			(char *)gd->cname.value, gd->sec.qop, gd->sec.svc);
#endif
#endif				/* DEBUG */
		seq = htonl(gr->gr_win);
		seqbuf.value = &seq;
		seqbuf.length = sizeof(seq);

		gss_release_buffer(&min_stat, &gd->checksum);

		maj_stat =
		    gss_sign(&min_stat, gd->ctx, GSS_C_QOP_DEFAULT, &seqbuf,
			     &checksum);

		if (maj_stat != GSS_S_COMPLETE) {
			gss_release_buffer(&min_stat, &gr->gr_token);
			return (false);
		}

		/* XXX ref? (assert gd->locked?) */
		req->rq_verf.oa_flavor = RPCSEC_GSS;
		req->rq_verf.oa_base = checksum.value;
		req->rq_verf.oa_length = checksum.length;
	}
	return (true);
}

#define RPCHDR_LEN ((10 * BYTES_PER_XDR_UNIT) + MAX_AUTH_BYTES)

static int
svcauth_gss_validate(struct svc_req *req,
		     struct svc_rpc_gss_data *gd,
		     struct rpc_msg *msg)
{
	struct opaque_auth *oa;
	gss_buffer_desc rpcbuf, checksum;
	OM_uint32 maj_stat, min_stat, qop_state;
	u_char rpchdr[RPCHDR_LEN];
	int32_t *buf;

	memset(rpchdr, 0, RPCHDR_LEN);

	/* XXX - Reconstruct RPC header for signing (from xdr_callmsg). */
	oa = &msg->rm_call.cb_cred;
	if (oa->oa_length > MAX_AUTH_BYTES)
		return GSS_S_CALL_BAD_STRUCTURE;
	/* XXX since MAX_AUTH_BYTES is 400, the following code trivially
	 * overruns (up to 431 per Coverity, but compare RPCHDR_LEN with
	 * what is marshalled below). */

	buf = (int32_t *) rpchdr;
	IXDR_PUT_LONG(buf, msg->rm_xid);
	IXDR_PUT_ENUM(buf, msg->rm_direction);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_rpcvers);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_prog);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_vers);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_proc);
	IXDR_PUT_ENUM(buf, oa->oa_flavor);
	IXDR_PUT_LONG(buf, oa->oa_length);
	if (oa->oa_length) {
		memcpy((caddr_t) buf, oa->oa_base, oa->oa_length);
		buf += RNDUP(oa->oa_length) / sizeof(int32_t);
	}
	rpcbuf.value = rpchdr;
	rpcbuf.length = (u_char *) buf - rpchdr;

	checksum.value = msg->rm_call.cb_verf.oa_base;
	checksum.length = msg->rm_call.cb_verf.oa_length;

	maj_stat =
	    gss_verify_mic(&min_stat, gd->ctx, &rpcbuf, &checksum, &qop_state);

	if (maj_stat != GSS_S_COMPLETE) {
		__warnx(TIRPC_DEBUG_FLAG_AUTH, "%s: %d %d", __func__, maj_stat,
			min_stat);
		return (maj_stat);
	}
	return GSS_S_COMPLETE;
}

bool
svcauth_gss_nextverf(struct svc_req *req, struct svc_rpc_gss_data *gd,
		     u_int num)
{
	gss_buffer_desc signbuf, checksum;
	OM_uint32 maj_stat, min_stat;

	signbuf.value = &num;
	signbuf.length = sizeof(num);

	maj_stat =
	    gss_get_mic(&min_stat, gd->ctx, gd->sec.qop, &signbuf, &checksum);

	if (maj_stat != GSS_S_COMPLETE) {
		log_status("gss_get_mic", maj_stat, min_stat);
		return (false);
	}
	req->rq_verf.oa_flavor = RPCSEC_GSS;
	req->rq_verf.oa_base = (caddr_t) checksum.value;
	req->rq_verf.oa_length = (u_int) checksum.length;

	return (true);
}

#define svcauth_gss_return(code) \
	do { \
		if (gc) \
			xdr_free((xdrproc_t) xdr_rpc_gss_cred, gc); \
		if (gd_locked) \
			mutex_unlock(&gd->lock); \
		return (code); \
	} while (0)

enum auth_stat
_svcauth_gss(struct svc_req *req, struct rpc_msg *msg,
	     bool *no_dispatch)
{
	XDR xdrs[1];
	SVCAUTH *auth;
	struct svc_rpc_gss_data *gd = NULL;
	struct rpc_gss_cred *gc = NULL;
	struct rpc_gss_init_res gr;
	int call_stat, offset;
	OM_uint32 min_stat;
	bool gd_locked = false;
	bool gd_hashed = false;

	/* Initialize reply. */
	req->rq_verf = _null_auth;

	/* Unserialize client credentials. */
	if (req->rq_cred.oa_length <= 0)
		svcauth_gss_return(AUTH_BADCRED);

	gc = (struct rpc_gss_cred *)req->rq_clntcred;
	memset(gc, 0, sizeof(struct rpc_gss_cred));

	xdrmem_create(xdrs, req->rq_cred.oa_base, req->rq_cred.oa_length,
		      XDR_DECODE);

	if (!xdr_rpc_gss_cred(xdrs, gc)) {
		XDR_DESTROY(xdrs);
		svcauth_gss_return(AUTH_BADCRED);
	}
	XDR_DESTROY(xdrs);

	/* Check version. */
	if (gc->gc_v != RPCSEC_GSS_VERSION)
		svcauth_gss_return(AUTH_BADCRED);

	if (gc->gc_seq > RPCSEC_GSS_MAXSEQ)
		svcauth_gss_return(RPCSEC_GSS_CTXPROBLEM);

	if (gc->gc_proc > RPCSEC_GSS_MAXPROC)
		svcauth_gss_return(AUTH_BADCRED);

	/* Check RPCSEC_GSS service. */
	if (gc->gc_svc != RPCSEC_GSS_SVC_NONE
	    && gc->gc_svc != RPCSEC_GSS_SVC_INTEGRITY
	    && gc->gc_svc != RPCSEC_GSS_SVC_PRIVACY)
		svcauth_gss_return(AUTH_BADCRED);

	/* Context lookup. */
	if ((gc->gc_proc == RPCSEC_GSS_DATA)
	    || (gc->gc_proc == RPCSEC_GSS_DESTROY)) {

		/* XXX fix prototype, toss junk args */
		gd = authgss_ctx_hash_get(gc);
		if (!gd)
			svcauth_gss_return(AUTH_REJECTEDCRED);
		gd_hashed = true;
		if (gc->gc_svc != gd->sec.svc)
			gd->sec.svc = gc->gc_svc;
	}

	if (!gd) {
		/* Allocate and set up server auth handle. */
		auth = mem_alloc(sizeof(SVCAUTH));
		gd = alloc_svc_rpc_gss_data();
		auth->svc_ah_ops = &svc_auth_gss_ops;
		auth->svc_ah_private = (caddr_t) gd;
		gd->auth = auth;
	}

	/* Serialize context. */
	mutex_lock(&gd->lock);
	gd_locked = true;

	/* thread auth */
	req->rq_auth = gd->auth;

	/* Check sequence number. */
	if (gd->established) {
		if (get_time_fast() >= gd->endtime) {
			*no_dispatch = true;
			svcauth_gss_return(RPCSEC_GSS_CREDPROBLEM);
		}

		/* XXX implied serialization?  or just fudging?  advance if
		 * greater? */
		offset = gd->seqlast - gc->gc_seq;
		if (offset < 0) {
			gd->seqlast = gc->gc_seq;
			offset = 0 - offset;
			gd->seqmask <<= offset;
			offset = 0;
		} else if (offset >= gd->win || (gd->seqmask & (1 << offset))) {
			*no_dispatch = true;
			svcauth_gss_return(AUTH_OK);
		}
		gd->seqmask |= (1 << offset);	/* XXX harmless */

		req->rq_ap1 = (void *)(uintptr_t) gc->gc_seq; /* GCC casts */
		req->rq_clntname = (char *) gd->client_name;
		req->rq_svcname = (char *) gd->ctx;
	}

	/* gd->established */
	/* Handle RPCSEC_GSS control procedure. */
	switch (gc->gc_proc) {

	case RPCSEC_GSS_INIT:
	case RPCSEC_GSS_CONTINUE_INIT:

		if (req->rq_proc != NULLPROC)
			svcauth_gss_return(AUTH_FAILED); /* XXX ? */

		/* XXX why unconditionally acquire creds? */
		if (!svcauth_gss_acquire_cred())
			svcauth_gss_return(AUTH_FAILED);

		if (!svcauth_gss_accept_sec_context(req, gd, &gr))
			svcauth_gss_return(AUTH_REJECTEDCRED);

		if (!svcauth_gss_nextverf(req, gd, htonl(gr.gr_win))) {
			/* XXX check */
			gss_release_buffer(&min_stat, &gr.gr_token);
			mem_free(gr.gr_ctx.value, 0);
			svcauth_gss_return(AUTH_FAILED);
		}

		*no_dispatch = true;

		call_stat =
		    svc_sendreply(req->rq_xprt, req,
				  (xdrproc_t) xdr_rpc_gss_init_res,
				  (caddr_t) &gr);

		/* XXX */
		gss_release_buffer(&min_stat, &gr.gr_token);
		gss_release_buffer(&min_stat, &gd->checksum);
		mem_free(gr.gr_ctx.value, 0);

		if (!call_stat)
			svcauth_gss_return(AUTH_FAILED);

		if (gr.gr_major == GSS_S_COMPLETE) {
			gd->established = true;
			if (!gd_hashed) {

				/* krb5 pac -- try all that apply */
				gss_buffer_desc attr, display_buffer;

				/* completely generic */
				int auth = 1, comp = 0, more = -1;

				memset(&gd->pac.ms_pac, 0,
				       sizeof(gss_buffer_desc));
				memset(&display_buffer, 0,
				       sizeof(gss_buffer_desc));

				/* MS AD */
				attr.value = "urn:mspac:";
				attr.length = 10;

				gr.gr_major =
				    gss_get_name_attribute(&gr.gr_minor,
							   gd->client_name,
							   &attr, &auth, &comp,
							   &gd->pac.ms_pac,
							   &display_buffer,
							   &more);

				if (gr.gr_major == GSS_S_COMPLETE) {
					/* dont need it */
					gss_release_buffer(&gr.gr_minor,
							   &display_buffer);
					gd->flags |= SVC_RPC_GSS_FLAG_MSPAC;
				}

				(void)authgss_ctx_hash_set(gd);
			}
		}
		break;

		/* XXX next 2 cases:  is it correct to leave gd in cache
		 * after a validate or verf failure ? */

	case RPCSEC_GSS_DATA:
		call_stat = svcauth_gss_validate(req, gd, msg);
		switch (call_stat) {
		default:
			svcauth_gss_return(RPCSEC_GSS_CREDPROBLEM);
		case 0:
			break;
		}

		if (!svcauth_gss_nextverf(req, gd, htonl(gc->gc_seq)))
			svcauth_gss_return(AUTH_FAILED);
		break;

	case RPCSEC_GSS_DESTROY:
		if (req->rq_proc != NULLPROC)
			svcauth_gss_return(AUTH_FAILED);	/* XXX ? */

		if (svcauth_gss_validate(req, gd, msg))
			svcauth_gss_return(RPCSEC_GSS_CREDPROBLEM);

		if (!svcauth_gss_nextverf(req, gd, htonl(gc->gc_seq)))
			svcauth_gss_return(AUTH_FAILED);

		*no_dispatch = true;

		(void)authgss_ctx_hash_del(gd);	/* unrefs, can destroy gd */

		/* avoid lock order reversal gd->lock, xprt->xp_lock */
		mutex_unlock(&gd->lock);
		gd_locked = false;

		call_stat =
		    svc_sendreply(req->rq_xprt, req, (xdrproc_t) xdr_void,
				  (caddr_t) NULL);

		req->rq_auth = &svc_auth_none;

		break;

	default:
		svcauth_gss_return(AUTH_REJECTEDCRED);
		break;
	}

	svcauth_gss_return(AUTH_OK);
}

static bool
svcauth_gss_release(SVCAUTH *auth, struct svc_req *req)
{
	struct svc_rpc_gss_data *gd;
	caddr_t last_oa_base;

	gd = SVCAUTH_PRIVATE(auth);
	if (gd)
		unref_svc_rpc_gss_data(gd, SVC_RPC_GSS_FLAG_NONE);
	req->rq_auth = NULL;

	if ((last_oa_base = req->rq_verf.oa_base)) {
		/* XXX wrapper conflict: mem_free vs. gssalloc_free ? */
		/* ... but this only matters for win32 | kernel */
		req->rq_verf.oa_base = 0;
		mem_free(last_oa_base, req->rq_verf.oa_length);
	}

	return (true);
}

bool
svcauth_gss_destroy(SVCAUTH *auth)
{
	struct svc_rpc_gss_data *gd;
	OM_uint32 min_stat;

	gd = SVCAUTH_PRIVATE(auth);

	gss_delete_sec_context(&min_stat, &gd->ctx, GSS_C_NO_BUFFER);
	gss_release_buffer(&min_stat, &gd->cname);

	if (gd->client_name)
		gss_release_name(&min_stat, &gd->client_name);

	if (gd->flags & SVC_RPC_GSS_FLAG_MSPAC)
		gss_release_buffer(&min_stat, &gd->pac.ms_pac);

	gss_release_buffer(&min_stat, &gd->checksum);
	mutex_destroy(&gd->lock);

	mem_free(gd, sizeof(*gd));
	mem_free(auth, sizeof(*auth));

	return (true);
}

bool
svcauth_gss_wrap(SVCAUTH *auth, struct svc_req *req, XDR *xdrs,
		 xdrproc_t xdr_func, caddr_t xdr_ptr)
{
	bool result;
	struct svc_rpc_gss_data *gd = SVCAUTH_PRIVATE(req->rq_auth);
	u_int gc_seq = (u_int) (uintptr_t) req->rq_ap1;

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE)
		return ((*xdr_func) (xdrs, xdr_ptr));

	mutex_lock(&gd->lock);
	result = xdr_rpc_gss_data(xdrs, xdr_func, xdr_ptr, gd->ctx,
				  gd->sec.qop, gd->sec.svc, gc_seq);
	mutex_unlock(&gd->lock);
	return (result);
}

bool
svcauth_gss_unwrap(SVCAUTH *auth, struct svc_req *req, XDR *xdrs,
		   xdrproc_t xdr_func, caddr_t xdr_ptr)
{
	bool result;
	struct svc_rpc_gss_data *gd = SVCAUTH_PRIVATE(req->rq_auth);
	u_int gc_seq = (u_int) (uintptr_t) req->rq_ap1;

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE)
		return ((*xdr_func) (xdrs, xdr_ptr));

	mutex_lock(&gd->lock);
	result = xdr_rpc_gss_data(xdrs, xdr_func, xdr_ptr, gd->ctx,
				  gd->sec.qop, gd->sec.svc, gc_seq);
	mutex_unlock(&gd->lock);
	return (result);
}

char *
svcauth_gss_get_principal(SVCAUTH *auth)
{
	struct svc_rpc_gss_data *gd;
	char *pname;

	gd = SVCAUTH_PRIVATE(auth);

	if (gd->cname.length == 0)
		return (NULL);

	pname = mem_alloc(gd->cname.length + 1);
	memcpy(pname, gd->cname.value, gd->cname.length);
	pname[gd->cname.length] = '\0';

	return (pname);
}
