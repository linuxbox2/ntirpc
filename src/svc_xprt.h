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

#ifndef TIRPC_SVC_XPRT_H
#define TIRPC_SVC_XPRT_H

struct svc_xprt_rec
{
    struct opr_rbtree_node node_k;
    int fd_k;
    SVCXPRT *xprt;
    uint64_t gen; /* generation number */
    mutex_t mtx;
};

struct svc_xprt_set
{
    mutex_t lock;
    struct rbtree_x xt;
};

SVCXPRT* svc_xprt_set(SVCXPRT *xprt);
SVCXPRT* svc_xprt_get(int fd);
SVCXPRT *svc_xprt_clear(SVCXPRT *xprt);

/* iterator callback prototype */
#define SVC_XPRT_FOREACH_NONE    0x0000
#define SVC_XPRT_FOREACH_CLEAR   0x0001 /* each_f destroyed xprt */

typedef uint32_t (*svc_xprt_each_func_t) (SVCXPRT *xprt, void *arg);
int svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg);

void svc_xprt_dump_xprts(const char *tag);
void svc_xprt_shutdown();

#endif /* TIRPC_SVC_XPRT_H */
