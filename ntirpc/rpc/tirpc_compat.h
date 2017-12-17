/*
 * Copyright (c) 2012 Linux Box Corporation.
 * Copyright (c) 2012-2017 Red Hat, Inc. and/or its affiliates.
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

#ifndef TIRPC_COMPAT_H
#define TIRPC_COMPAT_H

/* svc.h */
#define svc_create(a, b, c, d) svc_ncreate(a, b, c, d)
#define svc_tp_create(a, b, c, d) svc_tp_ncreate(a, b, c, d)
#define svc_tli_create(a, b, c, d, e) svc_tli_ncreate(a, b, c, d, e)
#define svc_vc_create(a, b, c) svc_vc_ncreate(a, b, c)
#define svc_dg_create(a, b, c) svc_dg_ncreate(a, b, c)
#define svc_fd_create(a, b, c) svc_fd_ncreate(a, b, c)
#define svc_raw_create() svc_raw_ncreate()
#define svc_rdma_create(a, b, c) svc_rdma_ncreate(a, b, c)

/* rpc_msg */
#define xdr_callmsg xdr_ncallmsg
#define xdr_callhdr xdr_ncallhdr
#define xdr_replymsg xdr_nreplymsg
#define xdr_accepted_reply xdr_naccepted_reply
#define xdr_rejected_reply xdr_nrejected_reply

/* xdr */
#define xdr_netobj xdr_nnetobj
#define xdrmem_create(a, b, c, d) xdrmem_ncreate(a, b, c, d)
#define xdr_free xdr_nfree

#endif				/* !TIRPC_COMPAT_H */
