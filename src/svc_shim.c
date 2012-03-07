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

/* static */ int read_vc(void *, void *, int);
/* static */ int write_vc(void *, void *, int);

/*
 * Duplicate xprt from original to copy.  GOING AWAY.
 */
SVCXPRT *
svc_shim_copy_xprt(SVCXPRT *xprt_copy, SVCXPRT *xprt_orig)
{
  if(xprt_copy)
      mem_free(xprt_copy, sizeof(SVCXPRT));

  xprt_copy = (SVCXPRT *) mem_alloc(sizeof(SVCXPRT));
  if(xprt_copy == NULL)
    goto fail_no_xprt;

  __warnx("svcxprt_copy copying xprt_orig=%p to xprt_copy=%p",
          xprt_orig, xprt_copy);

  memset(xprt_copy, 0, sizeof(SVCXPRT));

  xprt_copy->xp_flags = xprt_orig->xp_flags;
  xprt_copy->xp_flags |= SVC_XPRT_FLAG_COPY;

  xprt_copy->xp_ops  = xprt_orig->xp_ops;
  xprt_copy->xp_ops2 = xprt_orig->xp_ops2;
  xprt_copy->xp_fd   = xprt_orig->xp_fd;
  /* xp_p1 handled below */
  /* xp_p2 handled below */
  xprt_copy->xp_p3   = xprt_orig->xp_p3;
  xprt_copy->xp_p4   = xprt_orig->xp_p4;
  xprt_copy->xp_p5   = xprt_orig->xp_p5;
  xprt_copy->xp_u1   = xprt_orig->xp_u1;

  switch (xprt_orig->xp_type) {
  case XPRT_TCP:
  {
      struct cf_conn *cd_o = (struct cf_conn *)xprt_orig->xp_p1, *cd_c;
      cd_c = (struct cf_conn *) mem_alloc(sizeof(*cd_c));
      if(!cd_c)
          goto fail;
      memcpy(cd_c, cd_o, sizeof(*cd_c));
      xprt_copy->xp_p1 = cd_c;
      /* XXX check--is Ganesha using ordinary read_vc, write_vc? */
      xdrrec_create(&(cd_c->xdrs), cd_c->sendsize, cd_c->recvsize, xprt_copy,
                    read_vc, write_vc);
      if(xprt_orig->xp_verf.oa_base == cd_o->verf_body)
          xprt_copy->xp_verf.oa_base = cd_c->verf_body;
      else
          xprt_copy->xp_verf.oa_base = xprt_orig->xp_verf.oa_base;
      xprt_copy->xp_verf.oa_flavor = xprt_orig->xp_verf.oa_flavor;
      xprt_copy->xp_verf.oa_length = xprt_orig->xp_verf.oa_length;
  }
  break;
  case XPRT_UDP:
      if(su_data(xprt_orig)) {
          struct svc_dg_data *su_o = su_data(xprt_orig), *su_c;
          su_c = (struct svc_dg_data *) mem_alloc(sizeof(*su_c));
          if(!su_c)
              goto fail;
          xprt_copy->xp_p2 = su_c;
          memcpy(su_c, su_o, sizeof(*su_c));
          
          if(su_o->su_cache) {
              struct cl_cache *uc = su_o->su_cache;
              if(!svc_dg_enablecache(xprt_copy, uc->uc_size))
                  goto fail;
          }

          xprt_copy->xp_p1 = mem_alloc(su_c->su_iosz);
          if(! xprt_copy->xp_p1)
              goto fail;
          xdrmem_create(&(su_c->su_xdrs), xprt_copy->xp_p1,
                        su_c->su_iosz, XDR_DECODE);
          if(xprt_orig->xp_verf.oa_base == su_o->su_verfbody)
              xprt_copy->xp_verf.oa_base = su_c->su_verfbody;
          else
              xprt_copy->xp_verf.oa_base = xprt_orig->xp_verf.oa_base;
          xprt_copy->xp_verf.oa_flavor = xprt_orig->xp_verf.oa_flavor;
          xprt_copy->xp_verf.oa_length = xprt_orig->xp_verf.oa_length;
      }
      else
          goto fail;
      break;
  default:
      __warnx("Attempt to copy unknown xprt %p", xprt_orig);
      mem_free(xprt_copy, sizeof(SVCXPRT));
      goto fail_no_xprt;
      break;
  } /* switch */

  if(xprt_orig->xp_tp) {
      xprt_copy->xp_tp = rpc_strdup(xprt_orig->xp_tp);
      if(!xprt_copy->xp_tp)
          goto fail;
  }

  if(xprt_orig->xp_netid) {
      xprt_copy->xp_netid = rpc_strdup(xprt_orig->xp_netid);
      if(!xprt_copy->xp_netid)
          goto fail;
  }

  if(xprt_orig->xp_rtaddr.buf) {
      xprt_copy->xp_rtaddr.buf = mem_alloc(sizeof(struct sockaddr_storage));
      if(!xprt_copy->xp_rtaddr.buf)
          goto fail;
      memset(xprt_copy->xp_rtaddr.buf, 0, sizeof(struct sockaddr_storage));
      xprt_copy->xp_rtaddr.maxlen = sizeof(struct sockaddr_storage);
      xprt_copy->xp_rtaddr.len    = xprt_orig->xp_rtaddr.len;
      memcpy(xprt_copy->xp_rtaddr.buf, xprt_orig->xp_rtaddr.buf,
             xprt_orig->xp_rtaddr.len);
  }

  if(xprt_orig->xp_ltaddr.buf) {
      xprt_copy->xp_ltaddr.buf = mem_alloc(sizeof(struct sockaddr_storage));
      if(!xprt_copy->xp_ltaddr.buf)
          goto fail;
      xprt_copy->xp_ltaddr.maxlen = xprt_orig->xp_ltaddr.maxlen;
      xprt_copy->xp_ltaddr.len    = xprt_orig->xp_ltaddr.len;
      memcpy(xprt_copy->xp_ltaddr.buf, xprt_orig->xp_ltaddr.buf,
             sizeof(struct sockaddr_storage));
  }

  if(!copy_svc_authgss(xprt_copy, xprt_orig))
      goto fail;

  return (xprt_copy);

 fail:
  mem_free(xprt_copy, sizeof(SVCXPRT));

 fail_no_xprt:
  /* Let caller know about failure */
  __warnx("Failed to copy xprt");
  svcerr_systemerr(xprt_orig);
  return (NULL);
}
