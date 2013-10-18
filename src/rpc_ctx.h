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

#ifndef TIRPC_RPC_CTX_H
#define TIRPC_RPC_CTX_H

#define RPC_CTX_FLAG_NONE     0x0000
#define RPC_CTX_FLAG_LOCKED   0x0001

#define RPC_CTX_FLAG_WAITSYNC 0x0002
#define RPC_CTX_FLAG_SYNCDONE 0x0004
#define RPC_CTX_FLAG_ACKSYNC  0x0008

rpc_ctx_t *alloc_rpc_call_ctx(CLIENT *, rpcproc_t, xdrproc_t,
			      void *, xdrproc_t, void *, struct timeval);
void rpc_ctx_next_xid(rpc_ctx_t *, uint32_t);
int rpc_ctx_wait_reply(rpc_ctx_t *, uint32_t);
bool rpc_ctx_xfer_replymsg(struct x_vc_data *, struct rpc_msg *);
void rpc_ctx_ack_xfer(rpc_ctx_t *);
void free_rpc_call_ctx(rpc_ctx_t *, uint32_t);

#endif				/* TIRPC_RPC_CTX_H */
