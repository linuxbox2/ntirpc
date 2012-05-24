/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Interfaces which replace or carry forward "shim" or "glue" (originally)
 * between a generic TI-RPC and the Ganesha NFS server.  Some of these
 * should clearly be deprecated altogether, others may be quite ordinary,
 * relative to the prevailing design of TI-RPC.
 *
 * The creation by Ganesha of several "switch on xprt type, get secret data"
 * idioms hopefully will suggest cleaner API ideas for plug-out interfacing--
 * or else places where the existing modular abstraction of TI-RPC may be
 * just getting in the way.  CLIENT and SVCXPRT (with their respective private
 * data members) are examples in themselves.
 */

#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h>
#endif
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <rpc/types.h>
#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"

#include <rpc/svc.h>
#include <misc/rbtree.h>
#include <misc/opr_queue.h>
#include "clnt_internal.h"
#include <rpc/svc_rqst.h>
#include "svc_xprt.h"
#include <rpc/svc_dg.h>

#include "clnt_internal.h"
#include "svc_internal.h"

/*
 * XXX Ganesha.  Return "current" xid/su_xid.  Deprecated, going away (layering
 * and cardinality problem).
 */
u_int svc_shim_get_xid(SVCXPRT *xprt)
{
    union {
        struct cf_conn *cd;
        struct svc_dg_data *su;
    } csu;

    switch (xprt->xp_type)
    {
    case XPRT_TCP:
        csu.cd = (struct cf_conn *) xprt->xp_p1;
        if(csu.cd != NULL)
            return csu.cd->x_id;
        break;
    case XPRT_UDP:
        csu.su = su_data(xprt);
        if(csu.su != NULL)
            return csu.su->su_xid;
        break;
    default:
        break;
    }

    return (0);
}
