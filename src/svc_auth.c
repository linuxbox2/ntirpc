
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
 * Copyright (c) 1986-1991 by Sun Microsystems Inc.
 */

/*
 * svc_auth.c, Server-side rpc authenticator interface.
 *
 */
#include <config.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>
#include <stdlib.h>

/*
 * svcauthsw is the bdevsw of server side authentication.
 *
 * Server side authenticators are called from authenticate by
 * using the client auth struct flavor field to index into svcauthsw.
 * The server auth flavors must implement a routine that looks
 * like:
 *
 *	enum auth_stat
 *	flavorx_auth(rqst, msg)
 *		struct svc_req *rqst;
 *		struct rpc_msg *msg;
 *
 */

/* declarations to allow servers to specify new authentication flavors */
struct authsvc {
	int flavor;
	enum auth_stat (*handler) (struct svc_req *, struct rpc_msg *);
	struct authsvc *next;
};
static struct authsvc *Auths;

/*
 * The call rpc message, msg has been obtained from the wire.  The msg contains
 * the raw form of credentials and verifiers.  authenticate returns AUTH_OK
 * if the msg is successfully authenticated.  If AUTH_OK then the routine also
 * does the following things:
 * set req->rq_verf to the appropriate response verifier;
 * sets req->rq_client_cred to the "cooked" form of the credentials.
 *
 * NB: req->rq_verf must be pre-alloctaed, its length is set appropriately.
 *
 * The caller still owns and is responsible for msg->u.cmb.cred and
 * msg->u.cmb.verf.  The authentication system retains ownership of
 * req->rq_client_cred, the cooked credentials.
 *
 * There is an assumption that any flavour less than AUTH_NULL is invalid.
 */
enum auth_stat
svc_auth_authenticate(struct svc_req *req, struct rpc_msg *msg,
		      bool *no_dispatch)
{
	int cred_flavor;
	struct authsvc *asp;
	enum auth_stat rslt;
	extern mutex_t authsvc_lock;

	/* VARIABLES PROTECTED BY authsvc_lock: asp, Auths */
	req->rq_cred = msg->rm_call.cb_cred;
	req->rq_verf.oa_flavor = _null_auth.oa_flavor;
	req->rq_verf.oa_length = 0;
	cred_flavor = req->rq_cred.oa_flavor;
	switch (cred_flavor) {
	case RPCSEC_GSS:
		rslt = _svcauth_gss(req, msg, no_dispatch);
		return (rslt);
	case AUTH_NONE:
		rslt = _svcauth_none(req, msg);
		return (rslt);
		break;
	case AUTH_SYS:
		rslt = _svcauth_unix(req, msg);
		return (rslt);
	case AUTH_SHORT:
		rslt = _svcauth_short(req, msg);
		return (rslt);
#ifdef DES_BUILTIN
	case AUTH_DES:
		rslt = _svcauth_des(req, msg);
		return (rslt);
#endif
	default:
		break;
	}

	/* flavor doesn't match any of the builtin types, so try new ones */
	mutex_lock(&authsvc_lock);
	for (asp = Auths; asp; asp = asp->next) {
		if (asp->flavor == cred_flavor) {
			enum auth_stat as;
			as = (*asp->handler) (req, msg);
			mutex_unlock(&authsvc_lock);
			return (as);
		}
	}
	mutex_unlock(&authsvc_lock);

	return (AUTH_REJECTEDCRED);
}

/*
 *  Allow the rpc service to register new authentication types that it is
 *  prepared to handle.  When an authentication flavor is registered,
 *  the flavor is checked against already registered values.  If not
 *  registered, then a new Auths entry is added on the list.
 *
 *  There is no provision to delete a registration once registered.
 *
 *  This routine returns:
 *	 0 if registration successful
 *	 1 if flavor already registered
 *	-1 if can't register (errno set)
 */

int svc_auth_reg(int cred_flavor,
		 enum auth_stat (*handler) (struct svc_req *, struct rpc_msg *))
{
	struct authsvc *asp;
	extern mutex_t authsvc_lock;

	switch (cred_flavor) {
	case AUTH_NULL:
	case AUTH_SYS:
	case AUTH_SHORT:
	case RPCSEC_GSS:
#ifdef DES_BUILTIN
	case AUTH_DES:
#endif
		/* already registered */
		return (1);

	default:
		mutex_lock(&authsvc_lock);
		for (asp = Auths; asp; asp = asp->next) {
			if (asp->flavor == cred_flavor) {
				/* already registered */
				mutex_unlock(&authsvc_lock);
				return (1);
			}
		}

		/* this is a new one, so go ahead and register it */
		asp = mem_alloc(sizeof(*asp));
		if (asp == NULL) {
			mutex_unlock(&authsvc_lock);
			return (-1);
		}
		asp->flavor = cred_flavor;
		asp->handler = handler;
		asp->next = Auths;
		Auths = asp;
		mutex_unlock(&authsvc_lock);
		break;
	}
	return (0);
}
