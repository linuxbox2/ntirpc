/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
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
 * pmap_getport.c
 * Client interface to pmap rpc service.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */

#include <config.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>

#include <assert.h>
#include <unistd.h>

#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>

static const struct timeval timeout = { 5, 0 };
static const struct timespec to = { 3, 0 };

/*
 * Find the mapped port for program,version.
 * Calls the pmap service remotely to do the lookup.
 * Returns 0 if no map exists.
 */
u_short pmap_getport(struct sockaddr_in *address, u_long program,
		     u_long version, u_int protocol)
{
	CLIENT *client;
	struct pmap parms;
	int sock = -1;
	u_short port = 0;

	assert(address != NULL);

	address->sin_port = htons(PMAPPORT);
	client =
	    clntudp_nbufcreate(address, PMAPPROG, PMAPVERS, timeout, &sock,
			       RPCSMALLMSGSIZE, RPCSMALLMSGSIZE);
	if (client != NULL) {
		struct clnt_req *cc = mem_alloc(sizeof(*cc));

		parms.pm_prog = program;
		parms.pm_vers = version;
		parms.pm_prot = protocol;
		parms.pm_port = 0;	/* not needed or used */

		clnt_req_fill(cc, client, authnone_create(), PMAPPROC_GETPORT,
			      (xdrproc_t) xdr_pmap, &parms,
			      (xdrproc_t) xdr_u_short, &port);
		if (clnt_req_setup(cc, to)) {
			enum clnt_stat clnt_stat = CLNT_CALL(cc);

			if (clnt_stat != RPC_SUCCESS) {
				rpc_createerr.cf_stat = RPC_PMAPFAILURE;
				clnt_geterr(client, &rpc_createerr.cf_error);
			} else if (port == 0) {
				rpc_createerr.cf_stat = RPC_PROGNOTREGISTERED;
			}
		}
		clnt_req_release(cc);
		CLNT_DESTROY(client);
	}
	address->sin_port = 0;
	return (port);
}
