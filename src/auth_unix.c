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
#include <sys/cdefs.h>

/*
 * auth_unix.c, Implements UNIX style authentication parameters.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * The system is very weak.  The client uses no encryption for it's
 * credentials and only sends null verifiers.  The server sends backs
 * null verifiers or optionally a verifier that suggests a new short hand
 * for the credentials.
 *
 */
#include <sys/param.h>

#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpc/clnt.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>

/* auth_unix.c */
static void authunix_nextverf(AUTH *);
static bool authunix_marshal(AUTH *, XDR *);
static bool authunix_validate(AUTH *, struct opaque_auth *);
static bool authunix_refresh(AUTH *, void *);
static void authunix_destroy(AUTH *);
static void marshal_new_auth(AUTH *);
static struct auth_ops *authunix_ops(void);

/*
 * This struct is pointed to by the ah_private field of an auth_handle.
 */
struct audata {
	struct opaque_auth au_origcred;	/* original credentials */
	struct opaque_auth au_shcred;	/* short hand cred */
	u_long au_shfaults;	/* short hand cache faults */
	char au_marshed[MAX_AUTH_BYTES];
	u_int au_mpos;		/* xdr pos at end of marshed */
};
#define AUTH_PRIVATE(auth) ((struct audata *)auth->ah_private)

/*
 * Create a unix style authenticator.
 * Returns an auth handle with the given stuff in it.
 */
AUTH *
authunix_ncreate(char *machname, uid_t uid, gid_t gid, int len,
		 gid_t *aup_gids)
{
	struct authunix_parms aup;
	char mymem[MAX_AUTH_BYTES];
	struct timespec now;
	XDR xdrs;
	AUTH *auth;
	struct audata *au;

	memset(&rpc_createerr, 0, sizeof(rpc_createerr));

	/*
	 * Allocate and set up auth handle
	 */
	au = NULL;
	auth = mem_alloc(sizeof(*auth));
	au = mem_alloc(sizeof(*au));

	auth->ah_ops = authunix_ops();
	auth->ah_private = (caddr_t) au;
	auth->ah_verf = au->au_shcred = _null_auth;
	auth->ah_refcnt = 1;
	au->au_shfaults = 0;
	au->au_origcred.oa_base = NULL;

	/*
	 * fill in param struct from the given params
	 */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &now);
	aup.aup_time = now.tv_sec;
	aup.aup_machname = machname;
	aup.aup_uid = uid;
	aup.aup_gid = gid;
	aup.aup_len = (u_int) len;
	aup.aup_gids = aup_gids;

	/*
	 * Serialize the parameters into origcred
	 */
	xdrmem_create(&xdrs, mymem, MAX_AUTH_BYTES, XDR_ENCODE);
	if (!xdr_authunix_parms(&xdrs, &aup)) {
		rpc_createerr.cf_stat = RPC_CANTENCODEARGS;
		goto cleanup_authunix_create;
	}
	au->au_origcred.oa_length = len = XDR_GETPOS(&xdrs);
	au->au_origcred.oa_flavor = AUTH_UNIX;

	au->au_origcred.oa_base = mem_alloc((size_t) len);
	memmove(au->au_origcred.oa_base, mymem, (size_t) len);

	/*
	 * set auth handle to reflect new cred.
	 */
	auth->ah_cred = au->au_origcred;
	/* auth_get not needed:  ah_refcnt == 1, as desired */
	marshal_new_auth(auth);
	/* */
	return (auth);

cleanup_authunix_create:
	if (auth)
		mem_free(auth, sizeof(*auth));
	if (au) {
		if (au->au_origcred.oa_base)
			mem_free(au->au_origcred.oa_base, (size_t) len);
		mem_free(au, sizeof(*au));
	}
	return (NULL);
}

/*
 * Returns an auth handle with parameters determined by doing lots of
 * syscalls.
 */
AUTH *
authunix_ncreate_default(void)
{
	int len;
	char machname[MAXHOSTNAMELEN + 1];
	uid_t uid;
	gid_t gid, *gids;
	AUTH *result;

	memset(&rpc_createerr, 0, sizeof(rpc_createerr));

	if (gethostname(machname, sizeof(machname)) == -1) {
		rpc_createerr.cf_error.re_errno = errno;
		goto out_err;
	}
	machname[sizeof(machname) - 1] = 0;
	uid = geteuid();
	gid = getegid();

	/* According to glibc comments, an intervening setgroups(2)
	 * call can increase the number of supplemental groups between
	 * these two getgroups(2) calls. */
retry:
	len = getgroups(0, NULL);
	if (len == -1) {
		rpc_createerr.cf_error.re_errno = errno;
		goto out_err;
	}

	/* Bump allocation size.  A zero allocation size may result in a
	 * NULL calloc(3) result, which is not reliably distinguishable
	 * from a memory allocation error. */
	gids = mem_alloc((len + 1) * sizeof(gid_t));

	len = getgroups(len, gids);
	if (len == -1) {
		rpc_createerr.cf_error.re_errno = errno;
		mem_free(gids, (len + 1) * sizeof(gid_t));
		if (rpc_createerr.cf_error.re_errno == EINVAL) {
			rpc_createerr.cf_error.re_errno = 0;
			goto retry;
		}
		goto out_err;
	}

	/*
	 * AUTH_UNIX sends on the wire only the first NGRPS groups in the
	 * supplemental groups list.
	 */
	if (len > NGRPS)
		len = NGRPS;

	/* XXX: interface problem; those should all have been unsigned */
	result = authunix_ncreate(machname, uid, gid, len, gids);
	mem_free(gids, (len + 1) * sizeof(gid_t));
	return result;

out_err:
	rpc_createerr.cf_stat = RPC_SYSTEMERROR;
	return NULL;
}

/*
 * authunix operations
 */

/* ARGSUSED */
static void
authunix_nextverf(AUTH *auth)
{
	/* no action necessary */
}

static bool
authunix_marshal(AUTH *auth, XDR *xdrs)
{
	struct audata *au;

	assert(auth != NULL);
	assert(xdrs != NULL);

	au = AUTH_PRIVATE(auth);
	return (XDR_PUTBYTES(xdrs, au->au_marshed, au->au_mpos));
}

static bool
authunix_validate(AUTH *auth, struct opaque_auth *verf)
{
	struct audata *au;
	XDR xdrs;

	assert(auth != NULL);
	assert(verf != NULL);

	if (verf->oa_flavor == AUTH_SHORT) {
		au = AUTH_PRIVATE(auth);
		xdrmem_create(&xdrs, verf->oa_base, verf->oa_length,
			      XDR_DECODE);

		if (au->au_shcred.oa_base != NULL) {
			mem_free(au->au_shcred.oa_base,
				 au->au_shcred.oa_length);
			au->au_shcred.oa_base = NULL;
		}
		if (xdr_opaque_auth(&xdrs, &au->au_shcred)) {
			auth->ah_cred = au->au_shcred;
		} else {
			xdrs.x_op = XDR_FREE;
			(void)xdr_opaque_auth(&xdrs, &au->au_shcred);
			au->au_shcred.oa_base = NULL;
			auth->ah_cred = au->au_origcred;
		}
		marshal_new_auth(auth);
	}
	return (true);
}

static bool
authunix_refresh(AUTH *auth, void *dummy)
{
	struct audata *au = AUTH_PRIVATE(auth);
	struct authunix_parms aup;
	struct timespec now;
	XDR xdrs;
	int stat;

	assert(auth != NULL);

	if (auth->ah_cred.oa_base == au->au_origcred.oa_base) {
		/* there is no hope.  Punt */
		return (false);
	}
	au->au_shfaults++;

	/* first deserialize the creds back into a struct authunix_parms */
	aup.aup_machname = NULL;
	aup.aup_gids = NULL;
	xdrmem_create(&xdrs, au->au_origcred.oa_base, au->au_origcred.oa_length,
		      XDR_DECODE);
	stat = xdr_authunix_parms(&xdrs, &aup);
	if (!stat)
		goto done;

	/* update the time and serialize in place */
	(void)clock_gettime(CLOCK_MONOTONIC_FAST, &now);
	aup.aup_time = now.tv_sec;
	xdrs.x_op = XDR_ENCODE;
	XDR_SETPOS(&xdrs, 0);
	stat = xdr_authunix_parms(&xdrs, &aup);
	if (!stat)
		goto done;
	auth->ah_cred = au->au_origcred;
	marshal_new_auth(auth);
 done:
	/* free the struct authunix_parms created by deserializing */
	xdrs.x_op = XDR_FREE;
	(void)xdr_authunix_parms(&xdrs, &aup);
	XDR_DESTROY(&xdrs);
	return (stat);
}

static void
authunix_destroy(AUTH *auth)
{
	struct audata *au;

	assert(auth != NULL);

	au = AUTH_PRIVATE(auth);
	mem_free(au->au_origcred.oa_base, au->au_origcred.oa_length);

	if (au->au_shcred.oa_base != NULL)
		mem_free(au->au_shcred.oa_base, au->au_shcred.oa_length);

	mem_free(auth->ah_private, sizeof(struct audata));

	if (auth->ah_verf.oa_base != NULL)
		mem_free(auth->ah_verf.oa_base, auth->ah_verf.oa_length);

	mem_free(auth, sizeof(*auth));
}

/*
 * Marshals (pre-serializes) an auth struct.
 * sets private data, au_marshed and au_mpos
 */
static void
marshal_new_auth(AUTH *auth)
{
	XDR xdr_stream;
	XDR *xdrs = &xdr_stream;
	struct audata *au;

	assert(auth != NULL);

	au = AUTH_PRIVATE(auth);
	xdrmem_create(xdrs, au->au_marshed, MAX_AUTH_BYTES, XDR_ENCODE);
	if ((!xdr_opaque_auth(xdrs, &(auth->ah_cred)))
	    || (!xdr_opaque_auth(xdrs, &(auth->ah_verf))))
		__warnx(TIRPC_DEBUG_FLAG_AUTH,
			"auth_none.c - Fatal marshalling " "problem");
	else
		au->au_mpos = XDR_GETPOS(xdrs);
	XDR_DESTROY(xdrs);
}

static bool
authunix_wrap(AUTH *auth, XDR *xdrs, xdrproc_t xfunc,
	      caddr_t xwhere)
{
	return ((*xfunc) (xdrs, xwhere));
}

static struct auth_ops *
authunix_ops(void)
{
	static struct auth_ops ops;
	extern mutex_t ops_lock; /* XXXX does it need to be extern? */

	/* VARIABLES PROTECTED BY ops_lock: ops */
	mutex_lock(&ops_lock);
	if (ops.ah_nextverf == NULL) {
		ops.ah_nextverf = authunix_nextverf;
		ops.ah_marshal = authunix_marshal;
		ops.ah_validate = authunix_validate;
		ops.ah_refresh = authunix_refresh;
		ops.ah_destroy = authunix_destroy;
		ops.ah_wrap = authunix_wrap;
		ops.ah_unwrap = authunix_wrap;
	}
	mutex_unlock(&ops_lock);
	return (&ops);
}
