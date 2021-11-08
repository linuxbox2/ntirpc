
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
  #if defined(LIBC_SCCS) && !defined(lint)
  static char *sccsid = "@(#)auth_none.c 1.19 87/08/11 Copyr 1984 Sun Micro";
  static char *sccsid = "@(#)auth_none.c 2.1 88/07/29 4.0 RPCSRC";
  #endif
  #include <sys/cdefs.h>
  __FBSDID("$FreeBSD: src/lib/libc/rpc/auth_none.c,v 1.12 2002/03/22 23:18:35 obrien Exp $");
*/

/*
 * auth_none.c
 * Creates a client authentication handle for passing "null"
 * credentials and verifiers to remote systems.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */
#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/xdr_inline.h>
#include <rpc/auth_inline.h>
#include <rpc/auth.h>
#include <misc/opr.h>

#define MAX_MARSHAL_SIZE 20

/*
 * Authenticator operations routines
 */

static bool authnone_marshal(AUTH *, XDR *);
static void authnone_verf(AUTH *);
static bool authnone_validate(AUTH *, struct opaque_auth *);
static bool authnone_refresh(AUTH *, void *);
static void authnone_destroy(AUTH *);

static struct auth_ops *authnone_dummy(void);
static struct auth_ops *authnone_ops(void);

struct andata {
	AUTH no_client;
	char marshalled_client[MAX_MARSHAL_SIZE];
	u_int mcnt;
};
#define AUTH_PRIVATE(p) (opr_containerof((p), struct andata, no_client))

static struct andata auth_none_priv;
static struct andata *ap;

static pthread_mutex_t init_lock = MUTEX_INITIALIZER;

AUTH *
authnone_ncreate(void)
{
	XDR xdrs[1];

	if (!ap) {
		mutex_lock(&init_lock);
		if (!ap) {
			ap = &auth_none_priv; /* shared */
			ap->no_client.ah_cred =
			ap->no_client.ah_verf = _null_auth;
			ap->no_client.ah_ops = authnone_ops();
			ap->no_client.ah_private = NULL;
			ap->no_client.ah_error.re_status = RPC_SUCCESS;

			xdrmem_create(xdrs, ap->marshalled_client,
				      (u_int) MAX_MARSHAL_SIZE, XDR_ENCODE);
			(void)xdr_opaque_auth_encode(xdrs,
						     &ap->no_client.ah_cred);
			(void)xdr_opaque_auth_encode(xdrs,
						     &ap->no_client.ah_verf);
			ap->mcnt = XDR_GETPOS(xdrs);
			XDR_DESTROY(xdrs);
		}
		mutex_unlock(&init_lock);
	}

	return (&ap->no_client);
}

AUTH *
authnone_ncreate_dummy(void)
{
	struct andata *ap = mem_alloc(sizeof(*ap));

	ap->no_client.ah_ops = authnone_dummy();
	ap->no_client.ah_private = NULL;
	ap->no_client.ah_error.re_status = RPC_SUCCESS;
	ap->no_client.ah_refcnt = 1;

	return (&ap->no_client);
}

 /*ARGSUSED*/
static bool authnone_marshal(AUTH *client, XDR *xdrs)
{
	struct andata *ap = AUTH_PRIVATE(client);

	return ((*xdrs->x_ops->x_putbytes)
		(xdrs, ap->marshalled_client, ap->mcnt));
}

/* All these unused parameters are required to keep ANSI-C from grumbling */
 /*ARGSUSED*/
static void authnone_verf(__attribute__ ((unused)) AUTH *client)
{
	/* do nothing */
}

 /*ARGSUSED*/
static bool authnone_validate(
	__attribute__ ((unused)) AUTH *client,
	__attribute__ ((unused)) struct opaque_auth *opaque)
{
	return (true);
}

 /*ARGSUSED*/
static bool
authnone_refresh(__attribute__ ((unused)) AUTH *client,
		 __attribute__ ((unused)) void *dummy)
{
	return (false);
}

 /*ARGSUSED*/
static void authnone_destroy(__attribute__ ((unused)) AUTH *client)
{
	/* do nothing */
}

static void authnone_destroy_dummy(AUTH *auth)
{
	struct andata *ap = AUTH_PRIVATE(auth);

	assert(auth != NULL);

	mem_free(ap, sizeof(*ap));
}

static bool
authnone_wrap(AUTH *auth, XDR *xdrs, xdrproc_t xfunc, void *xwhere)
{
	return ((*xfunc) (xdrs, xwhere, 0));
}

static struct auth_ops *
authnone_dummy(void)
{
	static struct auth_ops ops;
	extern mutex_t ops_lock; /* XXXX does this need to be extern? */

       /* VARIABLES PROTECTED BY ops_lock: ops */
	mutex_lock(&ops_lock);
	if (ops.ah_nextverf == NULL) {
		ops.ah_nextverf = authnone_verf;
		ops.ah_marshal = authnone_marshal;
		ops.ah_validate = authnone_validate;
		ops.ah_refresh = authnone_refresh;
		ops.ah_destroy = authnone_destroy_dummy;
		ops.ah_wrap = authnone_wrap;
		ops.ah_unwrap = authnone_wrap;
	}
	mutex_unlock(&ops_lock);
	return (&ops);
}

static struct auth_ops *
authnone_ops(void)
{
	static struct auth_ops ops;
	extern mutex_t ops_lock; /* XXXX does this need to be extern? */

       /* VARIABLES PROTECTED BY ops_lock: ops */
	mutex_lock(&ops_lock);
	if (ops.ah_nextverf == NULL) {
		ops.ah_nextverf = authnone_verf;
		ops.ah_marshal = authnone_marshal;
		ops.ah_validate = authnone_validate;
		ops.ah_refresh = authnone_refresh;
		ops.ah_destroy = authnone_destroy;
		ops.ah_wrap = authnone_wrap;
		ops.ah_unwrap = authnone_wrap;
	}
	mutex_unlock(&ops_lock);
	return (&ops);
}
