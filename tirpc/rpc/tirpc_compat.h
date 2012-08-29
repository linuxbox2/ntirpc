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

#ifndef TIRPC_COMPAT_H
#define TIRPC_COMPAT_H

/* clnt_soc.h */
#define clnttcp_create(a, b, c, d, e, f) clnttcp_ncreate(a, b, c, d, e, f)
#define clntraw_create(a, b) clntraw_ncreate(a, b)
#define clnttcp6_create(a, b, c, d, e, f) clnttcp6_ncreate(a, b, c, d, e, f)
#define clntudp_create(a, b, c, d, e) clntudp_ncreate(a, b, c, d, e)
#define clntudp_bufcreate(a, b, c, d, e, f, g) \
    clntudp_nbufcreate(a, b, c, d, e, f, g)
#define clntudp6_create(a, b, c, d, e) clntudp6_ncreate(a, b, c, d, e)
#define clntudp6_bufcreate(a, b, c, d, e, f, g) \
    clntudp6_nbufcreate(a, b, c, d, e, f, g)

/* clnt.h */
#define clnt_create(a, b, c, d) clnt_ncreate(a, b, c, d)
#define clnt_create_time(a, b, c, d, e) clnt_ncreate_time(a, b, c, d, e)
#define clnt_create_vers(a, b, c, d, e, f) clnt_ncreate_vers(a, b, c, d, e, f)
#define clnt_create_vers_timed(a, b, c, d, e, f, g) \
    clnt_ncreate_vers_timed(a, b, c, d, e, f, g)
#define clnt_tp_create(a, b, c, d) clnt_tp_ncreate(a, b, c, d)
#define clnt_tp_create_timed(a, b, c, d, e) clnt_tp_ncreate_timed(a, b, c, d, e)
#define clnt_tli_create(a, b, c, d, e, f, g) \
    clnt_tli_ncreate(a, b, c, d, e, f, g)
#define clnt_vc_create(a, b, c, d, e, f) clnt_vc_ncreate(a, b, c, d, e, f)
#define clnt_vc_create2(a, b, c, d, e, f, g) \
    clnt_vc_ncreate2(a, b, c, d, e, f, g)
#define clntunix_create(a, b, c, d, e) clntunix_ncreate(a, b, c, d, e)
#define clnt_dg_create(a, b, c, d, e, f) clnt_dg_ncreate(a, b, c, d, e, f)
#define clnt_raw_create(a, b) clnt_raw_ncreate(a, b)

/* svc_soc.h */
#define svcraw_create() svcraw_ncreate()
#define svcudp_create(a) svcudp_ncreate(a) 
#define svcudp_bufcreate(a, b, c) svcudp_nbufcreate(a, b, c)
#define svcudp6_create(a) svcudp6_ncreate(a)
#define svcudp6_bufcreate(a, b, c) svcudp6_nbufcreate(a, b, c)
#define svctcp_create(a, b, c) svctcp_ncreate(a, b, c)
#define svctcp6_create(a, b, c) svctcp6_ncreate(a, b, c)

/* svc.h */
#define svc_create(a, b, c, d) svc_ncreate(a, b, c, d)
#define svc_tp_create(a, b, c, d) svc_tp_ncreate(a, b, c, d)
#define svc_tli_create(a, b, c, d, e) svc_tli_ncreate(a, b, c, d, e)
#define svc_vc_create(a, b, c) svc_vc_ncreate(a, b, c)
#define svc_vc_create2(a, b, c, d) svc_vc_ncreate2(a, b, c, d)
#define clnt_vc_create_svc(a, b, c, d) clnt_vc_ncreate_svc(a, b, c, d)
#define svc_vc_create_clnt(a, b, c, d) svc_vc_ncreate_clnt(a, b, c, d)
#define svcunix_create(a, b, c, d) svcunix_ncreate(a, b, c, d)
#define svc_dg_create(a, b, c) svc_dg_ncreate(a, b, c)
#define svc_fd_create(a, b, c) svc_fd_ncreate(a, b, c)
#define svcunixfd_create(a, b, c) svcunixfd_ncreate(a, b, c)
#define svc_raw_create() svc_raw_ncreate() 

#endif /* !TIRPC_COMPAT_H */
