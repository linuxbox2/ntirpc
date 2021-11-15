/*
  svc_auth_none.c

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

  $Id: svc_auth_none.c,v 1.1 2004/10/22 17:24:30 bfields Exp $
*/

#include "config.h"
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>

static bool
svcauth_none_wrap(struct svc_req *req, XDR *xdrs)
{
	return ((*req->rq_msg.RPCM_ack.ar_results.proc)
		(xdrs, req->rq_msg.RPCM_ack.ar_results.where));
}

static bool
svcauth_none_unwrap(struct svc_req *req)
{
	return ((*req->rq_msg.rm_xdr.proc)
		(req->rq_xdrs, req->rq_msg.rm_xdr.where));
}

static bool
svcauth_none_checksum(struct svc_req *req)
{
	XDR *xdrs = req->rq_xdrs;

	SVC_CHECKSUM(req, xdrs->x_data, xdr_size_inline(xdrs));
	return ((*req->rq_msg.rm_xdr.proc) (xdrs, req->rq_msg.rm_xdr.where));
}

static bool
svcauth_none_release(struct svc_req * __attribute__ ((unused)) req)
{
	return (true);
}

static bool
svcauth_none_destroy(SVCAUTH *auth)
{
	return (true);
}

static struct svc_auth_ops svc_auth_none_ops = {
	svcauth_none_wrap,
	svcauth_none_unwrap,
	svcauth_none_checksum,
	svcauth_none_release,
	svcauth_none_destroy
};

SVCAUTH svc_auth_none = {
	&svc_auth_none_ops,
	NULL,
};

enum auth_stat
_svcauth_none(struct svc_req *req)
{
	req->rq_auth = &svc_auth_none;

	return (AUTH_OK);
}
