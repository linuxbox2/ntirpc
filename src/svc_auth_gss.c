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

#include <sys/types.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <rpc/rpc.h>
#include <rpc/rpcsec_gss.h>

#include <gssapi/gssapi_ext.h>

#define UNUSED(x) UNUSED_ ## x __attribute__((unused))

extern SVCAUTH svc_auth_none;

/* Internal only */
bool_t rpc_gss_oid_to_mech(rpc_gss_OID, char **);
bool_t rpc_gss_num_to_qop(char *, u_int, char **);

/*
 * from mit-krb5-1.2.1 mechglue/mglueP.h:
 * Array of context IDs typed by mechanism OID
 */
typedef struct gss_union_ctx_id_t {
  gss_OID     mech_type;
  gss_ctx_id_t    internal_ctx_id;
} gss_union_ctx_id_desc, *gss_union_ctx_id_t;



static bool_t	svcauth_gss_wrap(SVCAUTH *, XDR *, xdrproc_t, caddr_t);
static bool_t	svcauth_gss_unwrap(SVCAUTH *, XDR *, xdrproc_t, caddr_t);
static bool_t	svcauth_gss_destroy(SVCAUTH *);

static struct svc_auth_ops svc_auth_gss_ops = {
	svcauth_gss_wrap,
	svcauth_gss_unwrap,
	svcauth_gss_destroy
};

struct svc_rpc_gss_data {
	bool_t			established;	/* context established */
	gss_ctx_id_t		ctx;		/* context id */
	struct rpc_gss_sec	sec;		/* security triple */
	gss_buffer_desc		cname;		/* GSS client name */
	u_int			seq;		/* sequence number */
	u_int			win;		/* sequence window */
	u_int			seqlast;	/* last sequence number */
	u_int32_t		seqmask;	/* bitmask of seqnums */
	gss_name_t		client_name;	/* unparsed name string */
	rpc_gss_rawcred_t	rcred;		/* raw credential */
	rpc_gss_ucred_t		ucred;		/* cooked credential */
	gid_t			gids[NGRPS];	/* list of groups */
	void *			cookie;		/* callback cookie */
};

#define SVCAUTH_PRIVATE(auth) \
	((struct svc_rpc_gss_data *)(auth)->svc_ah_private)

/* Global server credentials. */
static gss_cred_id_t	_svcauth_gss_creds;
static gss_name_t	_svcauth_gss_name = GSS_C_NO_NAME;
static char *		_svcauth_svc_name = NULL;

bool_t
svcauth_gss_set_svc_name(gss_name_t name)
{
	OM_uint32	maj_stat, min_stat;

	gss_log_debug("in svcauth_gss_set_svc_name()");

	if (_svcauth_gss_name != GSS_C_NO_NAME) {
		maj_stat = gss_release_name(&min_stat, &_svcauth_gss_name);

		if (maj_stat != GSS_S_COMPLETE) {
			gss_log_status("svcauth_gss_set_svc_name: gss_release_name", 
				maj_stat, min_stat);
			return (FALSE);
		}
		_svcauth_gss_name = GSS_C_NO_NAME;
	}
	maj_stat = gss_duplicate_name(&min_stat, name, &_svcauth_gss_name);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_set_svc_name: gss_duplicate_name", 
			maj_stat, min_stat);
		return (FALSE);
	}

	return (TRUE);
}

static bool_t
svcauth_gss_import_name(char *service)
{
	gss_name_t	name;
	gss_buffer_desc	namebuf;
	OM_uint32	maj_stat, min_stat;

	gss_log_debug("in svcauth_gss_import_name()");

	namebuf.value = service;
	namebuf.length = strlen(service);

	maj_stat = gss_import_name(&min_stat, &namebuf,
				   (gss_OID)GSS_C_NT_HOSTBASED_SERVICE, &name);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_import_name: gss_import_name", 
			maj_stat, min_stat);
		return (FALSE);
	}
	if (svcauth_gss_set_svc_name(name) != TRUE) {
		gss_release_name(&min_stat, &name);
		return (FALSE);
	}
	return (TRUE);
}

static bool_t
svcauth_gss_acquire_cred(u_int req_time, gss_OID_set_desc *oid_set)
{
	OM_uint32	maj_stat, min_stat;

	gss_log_debug("in svcauth_gss_acquire_cred()");

	maj_stat = gss_acquire_cred(&min_stat, _svcauth_gss_name, req_time,
				    oid_set, GSS_C_ACCEPT,
				    &_svcauth_gss_creds, NULL, NULL);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_acquire_cred: gss_acquire_cred", 
			maj_stat, min_stat);
		return (FALSE);
	}
	return (TRUE);
}

static bool_t
svcauth_gss_release_cred(void)
{
	OM_uint32	maj_stat, min_stat;

	gss_log_debug("in svcauth_gss_release_cred()");

	maj_stat = gss_release_cred(&min_stat, &_svcauth_gss_creds);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_release_cred: gss_release_cred", 
			maj_stat, min_stat);
		return (FALSE);
	}

	_svcauth_gss_creds = NULL;

	return (TRUE);
}

static rpc_gss_service_t
_rpc_gss_svc_to_service(rpc_gss_svc_t svc)
{
	switch (svc) {
	case RPCSEC_GSS_SVC_NONE:
		return rpcsec_gss_svc_none;
	case RPCSEC_GSS_SVC_INTEGRITY:
		return rpcsec_gss_svc_integrity;
	case RPCSEC_GSS_SVC_PRIVACY:
		return rpcsec_gss_svc_privacy;
	}
	return rpcsec_gss_svc_default;
}

static bool_t
_rpc_gss_fill_in_creds(struct svc_rpc_gss_data *gd, struct rpc_gss_cred *gc)
{
	rpc_gss_rawcred_t *rcred = &gd->rcred;
	OM_uint32 maj_stat, min_stat;
	gss_buffer_desc buf;

	rcred->version = gc->gc_v;
	if (!rpc_gss_oid_to_mech(gd->sec.mech, &rcred->mechanism))
		return FALSE;
	rcred->service = _rpc_gss_svc_to_service(gd->sec.svc);
	maj_stat = gss_export_name(&min_stat, gd->client_name, &buf);
	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_export_name", maj_stat, min_stat);
		return FALSE;
	}

	rcred->client_principal = calloc(1, sizeof(rpc_gss_principal_t) +
								buf.length);
	if (rcred->client_principal == NULL) {
		(void)gss_release_buffer(&min_stat, &buf);
		return FALSE;
	}
	rcred->client_principal->len = buf.length;
	(void)memcpy(rcred->client_principal->name, buf.value, buf.length);
	(void)gss_release_buffer(&min_stat, &buf);

	rcred->svc_principal = _svcauth_svc_name;

	return TRUE;
}

static bool_t
svcauth_gss_accept_sec_context(struct svc_req *rqst,
			       struct rpc_gss_init_res *gr)
{
	struct svc_rpc_gss_data	*gd;
	struct rpc_gss_cred	*gc;
	gss_buffer_desc		 recv_tok, seqbuf, checksum;
	gss_OID			 mech;
	OM_uint32		 maj_stat = 0, min_stat = 0, ret_flags, seq;

	gss_log_debug("in svcauth_gss_accept_context()");

	gd = SVCAUTH_PRIVATE(rqst->rq_xprt->xp_auth);
	gc = (struct rpc_gss_cred *)rqst->rq_clntcred;
	memset(gr, 0, sizeof(*gr));

	/* Deserialize arguments. */
	memset(&recv_tok, 0, sizeof(recv_tok));

	if (!svc_getargs(rqst->rq_xprt, (xdrproc_t)xdr_rpc_gss_init_args,
			 (caddr_t)&recv_tok))
		return (FALSE);

	gr->gr_major = gss_accept_sec_context(&gr->gr_minor,
					      &gd->ctx,
					      _svcauth_gss_creds,
					      &recv_tok,
					      GSS_C_NO_CHANNEL_BINDINGS,
					      &gd->client_name,
					      &mech,
					      &gr->gr_token,
					      &ret_flags,
					      NULL,
					      NULL);

	if (gr->gr_major != GSS_S_COMPLETE &&
	    gr->gr_major != GSS_S_CONTINUE_NEEDED) {
		gss_log_status("svcauth_gss_accept_sec_context: accept_sec_context",
			gr->gr_major, gr->gr_minor);
		gd->ctx = GSS_C_NO_CONTEXT;
		gss_release_buffer(&min_stat, &gr->gr_token);
		return (FALSE);
	}
	/* ANDROS: krb5 mechglue returns ctx of size 8 - two pointers,
	 * one to the mechanism oid, one to the internal_ctx_id */
	if ((gr->gr_ctx.value = mem_alloc(sizeof(gss_union_ctx_id_desc))) == NULL) {
		fprintf(stderr, "svcauth_gss_accept_context: out of memory\n");
		return (FALSE);
	}
	memcpy(gr->gr_ctx.value, gd->ctx, sizeof(gss_union_ctx_id_desc));
	gr->gr_ctx.length = sizeof(gss_union_ctx_id_desc);

	/* ANDROS: change for debugging linux kernel version...
	gr->gr_win = sizeof(gd->seqmask) * 8;
	*/
	gr->gr_win = 0x00000005;

	/* Save client info. */
	gd->sec.mech = mech;
	gd->sec.qop = GSS_C_QOP_DEFAULT;
	gd->sec.svc = gc->gc_svc;
	gd->seq = gc->gc_seq;
	gd->win = gr->gr_win;

	if (gr->gr_major == GSS_S_COMPLETE) {
		maj_stat = gss_display_name(&min_stat, gd->client_name,
					    &gd->cname, &gd->sec.mech);
		if (maj_stat != GSS_S_COMPLETE) {
			gss_log_status("svcauth_gss_accept_sec_context: display_name", 
				maj_stat, min_stat);
			return (FALSE);
		}
		if (!_rpc_gss_fill_in_creds(gd, gc))
			return (FALSE);

#ifdef HAVE_KRB5
		{
			gss_buffer_desc mechname;

			gss_oid_to_str(&min_stat, mech, &mechname);

			gss_log_debug("accepted context for %.*s with "
				      "<mech %.*s, qop %d, svc %d>",
				      gd->cname.length, (char *)gd->cname.value,
				      mechname.length, (char *)mechname.value,
				      gd->sec.qop, gd->sec.svc);

			gss_release_buffer(&min_stat, &mechname);
		}
#elif HAVE_HEIMDAL
		gss_log_debug("accepted context for %.*s with "
			      "<mech {}, qop %d, svc %d>",
			      gd->cname.length, (char *)gd->cname.value,
			      gd->sec.qop, gd->sec.svc);
#endif
		seq = htonl(gr->gr_win);
		seqbuf.value = &seq;
		seqbuf.length = sizeof(seq);

		maj_stat = gss_sign(&min_stat, gd->ctx, GSS_C_QOP_DEFAULT,
				    &seqbuf, &checksum);

		if (maj_stat != GSS_S_COMPLETE)
			return (FALSE);

		rqst->rq_xprt->xp_verf.oa_flavor = RPCSEC_GSS;
		rqst->rq_xprt->xp_verf.oa_base = checksum.value;
		rqst->rq_xprt->xp_verf.oa_length = checksum.length;
	}
	return (TRUE);
}

static bool_t
svcauth_gss_validate(struct svc_rpc_gss_data *gd, struct rpc_msg *msg)
{
	struct opaque_auth	*oa;
	gss_buffer_desc		 rpcbuf, checksum;
	OM_uint32		 maj_stat, min_stat, qop_state;
	u_char			 *rpchdr;
	int32_t			*buf;

	gss_log_debug("in svcauth_gss_validate()");

	/* XXX - Reconstruct RPC header for signing (from xdr_callmsg). */
	oa = &msg->rm_call.cb_cred;
	if (oa->oa_length > MAX_AUTH_BYTES)
		return (FALSE);

	rpchdr = (u_char *)calloc(((8 * BYTES_PER_XDR_UNIT) + 
			RNDUP(oa->oa_length)), 1);
	if (rpchdr == NULL)
		return (FALSE);

	buf = (int32_t *)rpchdr;
	IXDR_PUT_LONG(buf, msg->rm_xid);
	IXDR_PUT_ENUM(buf, msg->rm_direction);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_rpcvers);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_prog);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_vers);
	IXDR_PUT_LONG(buf, msg->rm_call.cb_proc);
	IXDR_PUT_ENUM(buf, oa->oa_flavor);
	IXDR_PUT_LONG(buf, oa->oa_length);
	if (oa->oa_length) {
		memcpy((caddr_t)buf, oa->oa_base, oa->oa_length);
		buf += RNDUP(oa->oa_length) / sizeof(int32_t);
	}
	rpcbuf.value = rpchdr;
	rpcbuf.length = (u_char *)buf - rpchdr;

	checksum.value = msg->rm_call.cb_verf.oa_base;
	checksum.length = msg->rm_call.cb_verf.oa_length;

	maj_stat = gss_verify_mic(&min_stat, gd->ctx, &rpcbuf, &checksum,
				  &qop_state);

	free(rpchdr);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_validate: gss_verify_mic", 
			maj_stat, min_stat);
		return (FALSE);
	}
	return (TRUE);
}

static bool_t
svcauth_gss_nextverf(struct svc_req *rqst, u_int num)
{
	struct svc_rpc_gss_data	*gd;
	gss_buffer_desc		 signbuf, checksum;
	OM_uint32		 maj_stat, min_stat;

	gss_log_debug("in svcauth_gss_nextverf()");

	if (rqst->rq_xprt->xp_auth == NULL)
		return (FALSE);

	gd = SVCAUTH_PRIVATE(rqst->rq_xprt->xp_auth);

	signbuf.value = &num;
	signbuf.length = sizeof(num);

	maj_stat = gss_get_mic(&min_stat, gd->ctx, gd->sec.qop,
			       &signbuf, &checksum);

	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("svcauth_gss_nextverf: gss_get_mic", 
			maj_stat, min_stat);
		return (FALSE);
	}
	rqst->rq_xprt->xp_verf.oa_flavor = RPCSEC_GSS;
	rqst->rq_xprt->xp_verf.oa_base = (caddr_t)checksum.value;
	rqst->rq_xprt->xp_verf.oa_length = (u_int)checksum.length;

	return (TRUE);
}

enum auth_stat
_svcauth_gss(struct svc_req *rqst, struct rpc_msg *msg, bool_t *no_dispatch)
{
	XDR	 		 xdrs;
	SVCAUTH			*auth;
	struct svc_rpc_gss_data	*gd;
	struct rpc_gss_cred	*gc;
	struct rpc_gss_init_res	 gr;
	int			 call_stat, offset;

	gss_log_debug("in svcauth_gss()");

	/* Initialize reply. */
	rqst->rq_xprt->xp_verf = _null_auth;

	/* Allocate and set up server auth handle. */
	if (rqst->rq_xprt->xp_auth == NULL ||
	    rqst->rq_xprt->xp_auth == &svc_auth_none) {
		if ((auth = calloc(sizeof(*auth), 1)) == NULL) {
			fprintf(stderr, "svcauth_gss: out_of_memory\n");
			return (AUTH_FAILED);
		}
		if ((gd = calloc(sizeof(*gd), 1)) == NULL) {
			fprintf(stderr, "svcauth_gss: out_of_memory\n");
			return (AUTH_FAILED);
		}
		auth->svc_ah_ops = &svc_auth_gss_ops;
		auth->svc_ah_private = (caddr_t) gd;
		rqst->rq_xprt->xp_auth = auth;
	}
	else gd = SVCAUTH_PRIVATE(rqst->rq_xprt->xp_auth);

	/* Deserialize client credentials. */
	if (rqst->rq_cred.oa_length <= 0)
		return (AUTH_BADCRED);

	gc = (struct rpc_gss_cred *)rqst->rq_clntcred;
	memset(gc, 0, sizeof(*gc));

	xdrmem_create(&xdrs, rqst->rq_cred.oa_base,
		      rqst->rq_cred.oa_length, XDR_DECODE);

	if (!xdr_rpc_gss_cred(&xdrs, gc)) {
		XDR_DESTROY(&xdrs);
		return (AUTH_BADCRED);
	}
	XDR_DESTROY(&xdrs);

	/* Check version. */
	if (gc->gc_v != RPCSEC_GSS_VERSION)
		return (AUTH_BADCRED);

	/* Check RPCSEC_GSS service. */
	if (gc->gc_svc != RPCSEC_GSS_SVC_NONE &&
	    gc->gc_svc != RPCSEC_GSS_SVC_INTEGRITY &&
	    gc->gc_svc != RPCSEC_GSS_SVC_PRIVACY)
		return (AUTH_BADCRED);

	/* Check sequence number. */
	if (gd->established) {
		if (gc->gc_seq > MAXSEQ)
			return (RPCSEC_GSS_CTXPROBLEM);

		if ((offset = gd->seqlast - gc->gc_seq) < 0) {
			gd->seqlast = gc->gc_seq;
			offset = 0 - offset;
			gd->seqmask <<= offset;
			offset = 0;
		}
		else if (offset >= gd->win || (gd->seqmask & (1 << offset))) {
			*no_dispatch = 1;
			return (RPCSEC_GSS_CTXPROBLEM);
		}
		gd->seq = gc->gc_seq;
		gd->seqmask |= (1 << offset);
	}

	if (gd->established) {
		rqst->rq_clntname = (char *)gd->client_name;
		rqst->rq_svcname = (char *)gd->ctx;
	}

	/* Handle RPCSEC_GSS control procedure. */
	switch (gc->gc_proc) {

	case RPCSEC_GSS_INIT:
	case RPCSEC_GSS_CONTINUE_INIT:
		if (rqst->rq_proc != NULLPROC)
			return (AUTH_FAILED);		/* XXX ? */

		if (_svcauth_gss_name == GSS_C_NO_NAME) {
			if (!svcauth_gss_import_name("nfs"))
				return (AUTH_FAILED);
		}

		if (!svcauth_gss_acquire_cred(0, GSS_C_NULL_OID_SET))
			return (AUTH_FAILED);

		if (!svcauth_gss_accept_sec_context(rqst, &gr))
			return (AUTH_REJECTEDCRED);

		if (!svcauth_gss_nextverf(rqst, htonl(gr.gr_win)))
			return (AUTH_FAILED);

		*no_dispatch = TRUE;

		call_stat = svc_sendreply(rqst->rq_xprt, 
			(xdrproc_t)xdr_rpc_gss_init_res, (caddr_t)&gr);

		if (!call_stat)
			return (AUTH_FAILED);

		if (gr.gr_major == GSS_S_COMPLETE)
			gd->established = TRUE;

		break;

	case RPCSEC_GSS_DATA:
		if (!svcauth_gss_validate(gd, msg))
			return (RPCSEC_GSS_CREDPROBLEM);

		if (!svcauth_gss_nextverf(rqst, htonl(gc->gc_seq)))
			return (AUTH_FAILED);
		break;

	case RPCSEC_GSS_DESTROY:
		if (rqst->rq_proc != NULLPROC)
			return (AUTH_FAILED);		/* XXX ? */

		if (!svcauth_gss_validate(gd, msg))
			return (RPCSEC_GSS_CREDPROBLEM);

		if (!svcauth_gss_nextverf(rqst, htonl(gc->gc_seq)))
			return (AUTH_FAILED);

		if (!svcauth_gss_release_cred())
			return (AUTH_FAILED);

		SVCAUTH_DESTROY(rqst->rq_xprt->xp_auth);
		rqst->rq_xprt->xp_auth = &svc_auth_none;

		break;

	default:
		return (AUTH_REJECTEDCRED);
		break;
	}
	return (AUTH_OK);
}

static bool_t
svcauth_gss_destroy(SVCAUTH *auth)
{
	struct svc_rpc_gss_data	*gd;
	OM_uint32		 min_stat;

	gss_log_debug("in svcauth_gss_destroy()");

	gd = SVCAUTH_PRIVATE(auth);

	gss_delete_sec_context(&min_stat, &gd->ctx, GSS_C_NO_BUFFER);
	gss_release_buffer(&min_stat, &gd->cname);

	if (gd->client_name)
		gss_release_name(&min_stat, &gd->client_name);
	if (gd->rcred.client_principal != NULL)
		free(gd->rcred.client_principal);

	mem_free(gd, sizeof(*gd));
	mem_free(auth, sizeof(*auth));

	return (TRUE);
}

static bool_t
svcauth_gss_wrap(SVCAUTH *auth, XDR *xdrs, xdrproc_t xdr_func, caddr_t xdr_ptr)
{
	struct svc_rpc_gss_data	*gd;

	gss_log_debug("in svcauth_gss_wrap()");

	gd = SVCAUTH_PRIVATE(auth);

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE) {
		return ((*xdr_func)(xdrs, xdr_ptr));
	}
	return (xdr_rpc_gss_data(xdrs, xdr_func, xdr_ptr,
				 gd->ctx, gd->sec.qop,
				 gd->sec.svc, gd->seq));
}

static bool_t
svcauth_gss_unwrap(SVCAUTH *auth, XDR *xdrs, xdrproc_t xdr_func, caddr_t xdr_ptr)
{
	struct svc_rpc_gss_data	*gd;

	gss_log_debug("in svcauth_gss_unwrap()");

	gd = SVCAUTH_PRIVATE(auth);

	if (!gd->established || gd->sec.svc == RPCSEC_GSS_SVC_NONE) {
		return ((*xdr_func)(xdrs, xdr_ptr));
	}
	return (xdr_rpc_gss_data(xdrs, xdr_func, xdr_ptr,
				 gd->ctx, gd->sec.qop,
				 gd->sec.svc, gd->seq));
}

char *
svcauth_gss_get_principal(SVCAUTH *auth)
{
	struct svc_rpc_gss_data *gd;
	char *pname;

	gd = SVCAUTH_PRIVATE(auth);

	if (gd->cname.length == 0)
		return (NULL);

	if ((pname = malloc(gd->cname.length + 1)) == NULL)
		return (NULL);

	memcpy(pname, gd->cname.value, gd->cname.length);
	pname[gd->cname.length] = '\0';

	return (pname);
}

/*
 * External API: Return maximum data size for a security mechanism and transport
 *
 * rqst: an incoming RPC request
 * maxlen: transport's maximum data size, in bytes
 *
 * Returns maximum data size given transformations done by current
 * security setting of "auth", in bytes, or zero if that value
 * cannot be determined.
 */
int
rpc_gss_svc_max_data_length(struct svc_req *rqst, int maxlen)
{
	OM_uint32 max_input_size, maj_stat, min_stat;
	struct svc_rpc_gss_data	*gd;
	int conf_req_flag;
	int result;

	if (!rqst)
		return 0;

	gd = SVCAUTH_PRIVATE(rqst->rq_xprt->xp_auth);

	switch (gd->rcred.service) {
	case rpcsec_gss_svc_none:
		return maxlen;
	case rpcsec_gss_svc_default:
	case rpcsec_gss_svc_integrity:
		conf_req_flag = 0;
		break;
	case rpcsec_gss_svc_privacy:
		conf_req_flag = 1;
		break;
	default:
		return 0;
	}

	result = 0;
	maj_stat = gss_wrap_size_limit(&min_stat, gd->ctx, conf_req_flag,
					gd->sec.qop, maxlen, &max_input_size);
	if (maj_stat == GSS_S_COMPLETE)
		if ((int)max_input_size > 0)
			result = (int)max_input_size;
	return result;
}

/*
 * External API: Set server's GSS principal
 *
 * principal: NUL-terminated C string containing GSS principal
 * mechanism: NUL-terminated C string containing GSS mechanism name
 * req_time: time in seconds until credential expires
 * program: program number of the RPC service
 * version: version number of the RPC service
 *
 * Returns TRUE if successful, otherwise FALSE is returned.
 */
bool_t
rpc_gss_set_svc_name(char *principal, char *mechanism, u_int req_time,
		u_int UNUSED(program), u_int UNUSED(version))
{
	gss_OID_set_desc oid_set;
	rpc_gss_OID oid;
	char *save;

	if (principal == NULL)
		return FALSE;
	save = strdup(principal);
	if (save == NULL)
		return FALSE;

	if (!rpc_gss_mech_to_oid(mechanism, &oid))
		goto out_err;
	oid_set.count = 1;
	oid_set.elements = (gss_OID)oid;

	if (!svcauth_gss_import_name(principal))
		goto out_err;
	if (!svcauth_gss_acquire_cred(req_time, &oid_set))
		goto out_err;

	free(_svcauth_svc_name);
	_svcauth_svc_name = save;
	return TRUE;

out_err:
	free(save);
	return FALSE;
}

static void
_rpc_gss_fill_in_ucreds(struct svc_rpc_gss_data *gd)
{
	rpc_gss_ucred_t *ucred = &gd->ucred;
	OM_uint32 maj_stat, min_stat;
	struct passwd pwd, *pw;
	long buflen;
	uid_t uid;
	char *buf;
	int len;

	/* default: "nfsnobody" */
	ucred->uid = 65534;
	ucred->gid = 65534;
	ucred->gidlen = 0;
	ucred->gidlist = gd->gids;

	maj_stat = gss_pname_to_uid(&min_stat, gd->client_name,
						gd->sec.mech, &uid);
	if (maj_stat != GSS_S_COMPLETE)
		return;

	buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (buflen == -1)
		return;
	buf = malloc((size_t)buflen);
	if (buf == NULL)
		return;

	(void)getpwuid_r(uid, &pwd, buf, buflen, &pw);
	if (pw == NULL) {
		free(buf);
		return;
	}

	ucred->uid = pw->pw_uid;
	ucred->gid = pw->pw_gid;
	len = NGRPS;
	(void)getgrouplist(pw->pw_name, pw->pw_gid, ucred->gidlist, &len);
	ucred->gidlen = len;

	free(buf);
}

/*
 * External API: Retrieve requesting client's GSS credential
 *
 * rqst: an incoming RPC request
 * rcred: address of pointer to fill in, or NULL
 * ucred: address of pointer to fill in, or NULL
 * cookie: address of pointer to fill in, or NULL
 *
 * Returns TRUE if successful. "rcred," "ucred," and "cookie"
 * are filled in with pointers to the relevant structures.
 * Caller must not free the memory returned by this API.
 */
bool_t
rpc_gss_getcred(struct svc_req *rqst, rpc_gss_rawcred_t **rcred,
		rpc_gss_ucred_t **ucred, void **cookie)
{
	struct svc_rpc_gss_data	*gd;
	SVCAUTH *auth;

	if (rqst == NULL)
		return FALSE;

	if (rqst->rq_xprt->xp_verf.oa_flavor != RPCSEC_GSS)
		return FALSE;

	auth = rqst->rq_xprt->xp_auth;
	gd = SVCAUTH_PRIVATE(auth);

	if (rcred != NULL) {
		auth->raw_cred = gd->rcred;
		auth->raw_cred.service = _rpc_gss_svc_to_service(gd->sec.svc);
		(void)rpc_gss_num_to_qop(auth->raw_cred.mechanism, gd->sec.qop,
						&auth->raw_cred.qop);
		*rcred = &auth->raw_cred;
	}

	if (ucred != NULL) {
		_rpc_gss_fill_in_ucreds(gd);
		*ucred = &gd->ucred;
	}

	if (cookie != NULL)
		*cookie = gd->cookie;

	return TRUE;
}

/*
 * External API: Register a callback function
 *
 * callback: callback structure containing function and parameters
 *
 * Returns TRUE if successful, otherwise FALSE.
 *
 * "callback" is copied by rpc_gss_set_callback(), and may be freed
 * immediately after it returns.
 */
bool_t
rpc_gss_set_callback(rpc_gss_callback_t *callback)
{
	/* not yet supported */
	return FALSE;
}

/*
 * External API: Form a generic rpc_gss_principal
 *
 * principal: address of buffer to fill in
 * mechanism: NUL-terminated C string containing GSS mechanism name
 * user_name: NUL-terminated C string containing user or service principal
 * node: NUL-terminated C string containing a hostname
 * secdomain: NUL-terminated C string containing a security realm
 *
 * Returns TRUE if successful, otherwise FALSE.  Caller must free
 * returned "principal" with free(3).
 */
bool_t
rpc_gss_get_principal_name(rpc_gss_principal_t *principal, char *mechanism,
		char *user_name, char *node, char *secdomain)
{
	OM_uint32 maj_stat, min_stat;
	rpc_gss_principal_t result;
	size_t nodelen, secdomlen;
	gss_name_t name, mechname;
	gss_buffer_desc namebuf;
	rpc_gss_OID oid;


	if (principal == NULL || user_name == NULL || strlen(user_name) == 0)
		return FALSE;
	if (!rpc_gss_mech_to_oid(mechanism, &oid))
		return FALSE;

	nodelen = 0;
	if (node != NULL)
		nodelen = strlen(node) + 1;
	secdomlen = 0;
	if (secdomain != NULL)
		secdomlen = strlen(secdomain) + 1;
	namebuf.length = strlen(user_name) + nodelen + secdomlen;
	namebuf.value = calloc(1, namebuf.length);
	if (namebuf.value == NULL)
		return FALSE;
	(void)strcpy(namebuf.value, user_name);
	if (nodelen > 0) {
		(void)strcat(namebuf.value, "/");
		(void)strcat(namebuf.value, node);
	}
	if (secdomlen > 0) {
		(void)strcat(namebuf.value, "@");
		(void)strcat(namebuf.value, secdomain);
	}

	maj_stat = gss_import_name(&min_stat, &namebuf,
					GSS_C_NT_USER_NAME, &name);
	free(namebuf.value);
	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_import_name", maj_stat, min_stat);
		return FALSE;
	}

	maj_stat = gss_canonicalize_name(&min_stat, name, oid, &mechname);
	(void)gss_release_name(&min_stat, &name);
	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_canonicalize_name", maj_stat, min_stat);
		return FALSE;
	}

	maj_stat = gss_export_name(&min_stat, mechname, &namebuf);
	(void)gss_release_name(&min_stat, &mechname);
	if (maj_stat != GSS_S_COMPLETE) {
		gss_log_status("gss_export_name", maj_stat, min_stat);
		return FALSE;
	}

	result = calloc(1, sizeof(*result) + namebuf.length);
	if (result == NULL) {
		(void)gss_release_buffer(&min_stat, &namebuf);
		return FALSE;
	}
	result->len = namebuf.length;
	(void)memcpy(result->name, namebuf.value, namebuf.length);
	(void)gss_release_buffer(&min_stat, &namebuf);

	*principal = result;
	return TRUE;
}
