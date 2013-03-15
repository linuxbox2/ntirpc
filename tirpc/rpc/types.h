/* $NetBSD: types.h,v 1.13 2000/06/13 01:02:44 thorpej Exp $ */

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
 *
 * from: @(#)types.h 1.18 87/07/24 SMI
 * from: @(#)types.h 2.3 88/08/15 4.0 RPCSRC
 * $FreeBSD: src/include/rpc/types.h,v 1.10.6.1 2003/12/18 00:59:50 peter Exp $
 */

/*
 * Rpc additions to <sys/types.h>
 */
#ifndef _TIRPC_TYPES_H
#define _TIRPC_TYPES_H

#ifdef _MSC_VER
#include <misc/stdint.h>
#else
#include <stdint.h>
#endif
#include <sys/types.h>

#if defined(_WIN32)

#define __BEGIN_DECLS
#define __END_DECLS

/* integral types */
#ifndef _MSC_VER
#include <_bsd_types.h> /* XXX mingw (defines u_long) */
#endif
typedef uint8_t u_char;
typedef uint16_t u_int16_t;
typedef uint16_t u_short_t;
typedef uint16_t u_short;
typedef uint32_t u_int;
typedef uint32_t u_int32_t;
typedef int64_t quad_t;
typedef uint64_t u_int64_t;
typedef uint64_t u_quad_t;

/* misc */
typedef char * caddr_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

struct iovec {
  void *iov_base;
  size_t iov_len;
};

#include <winsock2.h>
#include <ws2tcpip.h> /* XXX mingw */

#endif

typedef int32_t bool_t;
typedef int32_t enum_t;

typedef u_int32_t rpcprog_t;
typedef u_int32_t rpcvers_t;
typedef u_int32_t rpcproc_t;
typedef u_int32_t rpcprot_t;
typedef u_int32_t rpcport_t;
typedef int32_t rpc_inline_t;

#ifndef NULL
# define NULL 0
#endif
#define __dontcare__ -1

#define honey_badger __dontcare__

#ifndef FALSE
# define FALSE (0)
#endif
#ifndef TRUE
# define TRUE (1)
#endif

/*
 * Package params support
 */

#define TIRPC_GET_MALLOC           1
#define TIRPC_SET_MALLOC           2
#define TIRPC_GET_MEM_FREE         3
#define TIRPC_SET_MEM_FREE         4
#define TIRPC_GET_FREE             5
#define TIRPC_SET_FREE             6
#define TIRPC_GET_FLAGS            7
#define TIRPC_SET_FLAGS            8
#define TIRPC_GET_DEBUG_FLAGS      9
#define TIRPC_SET_DEBUG_FLAGS      10
#define TIRPC_GET_WARNX            11
#define TIRPC_SET_WARNX            12

/*
 * Debug flags support
 */

#define TIRPC_FLAG_NONE                 0x0000000
#define TIRPC_DEBUG_FLAG_NONE           0x0000000
#define TIRPC_DEBUG_FLAG_DEFAULT        0x0000001
#define TIRPC_DEBUG_FLAG_RPC_CACHE      0x0000002
#define TIRPC_DEBUG_FLAG_RPC_MSG        0x0000004
#define TIRPC_DEBUG_FLAG_LOCK           0x0000008
#define TIRPC_DEBUG_FLAG_MEM            0x0000010
#define TIRPC_DEBUG_FLAG_XDR            0x0000020
#define TIRPC_DEBUG_FLAG_RPCB           0x0000040
#define TIRPC_DEBUG_FLAG_AUTH           0x0000080
#define TIRPC_DEBUG_FLAG_CLNT_BCAST     0x0000100
#define TIRPC_DEBUG_FLAG_CLNT_DG        0x0000200
#define TIRPC_DEBUG_FLAG_CLNT_GEN       0x0000400
#define TIRPC_DEBUG_FLAG_CLNT_RAW       0x0000800
#define TIRPC_DEBUG_FLAG_CLNT_RDMA      0x0001000
#define TIRPC_DEBUG_FLAG_CLNT_SCTP      0x0002000
#define TIRPC_DEBUG_FLAG_CLNT_VC        0x0004000
#define TIRPC_DEBUG_FLAG_SVC            0x0008000
#define TIRPC_DEBUG_FLAG_SVC_DG         0x0010000
#define TIRPC_DEBUG_FLAG_SVC_RQST       0x0020000
#define TIRPC_DEBUG_FLAG_SVC_XPRT       0x0040000
#define TIRPC_DEBUG_FLAG_SVC_RDMA       0x0080000
#define TIRPC_DEBUG_FLAG_SVC_SCTP       0x0100000
#define TIRPC_DEBUG_FLAG_SVC_VC         0x0200000
#define TIRPC_DEBUG_FLAG_XDRREC         0x0400000
#define TIRPC_DEBUG_FLAG_RBTREE         0x0800000
#define TIRPC_DEBUG_FLAG_RPC_CTX        0x1000000
#define TIRPC_DEBUG_FLAG_RPCSEC_GSS     0x2000000
#define TIRPC_DEBUG_FLAG_REFCNT         0x4000000

typedef void *(*mem_alloc_t)(size_t);
typedef void (*mem_free_t)(void *, size_t);
typedef  void (*std_free_t)(void *);
typedef void (*warnx_t)(const char *fmt, ...);

/*
 * Package params support
 */
typedef struct tirpc_pkg_params {
    u_int flags;
    u_int debug_flags;
    warnx_t warnx;
} tirpc_pkg_params;

extern tirpc_pkg_params __pkg_params;

#include <misc/abstract_atomic.h>

#define __warnx(flags, ...) \
    do { \
        if (__pkg_params.debug_flags & (flags)) {  \
            __pkg_params.warnx(__VA_ARGS__); \
        } \
    } while (0)

#define __debug_flag(flags) (__pkg_params.debug_flags & (flags)))

#define mem_alloc(size) malloc((size))
#define mem_zalloc(size) calloc(1, (size))
#define mem_alloc_aligned(size, align) aligned_alloc((align), (size))
#define mem_free(ptr, size) free(ptr)

#ifndef _MSC_VER
#include <sys/time.h>
#include <sys/param.h>
#endif
#include <stdlib.h>
#include <netconfig.h>

/*
 * The netbuf structure is defined here, because FreeBSD / NetBSD only use
 * it inside the RPC code. It's in <xti.h> on SVR4, but it would be confusing
 * to have an xti.h, since FreeBSD / NetBSD do not support XTI/TLI.
 */

/*
 * The netbuf structure is used for transport-independent address storage.
 */
struct netbuf {
    unsigned int maxlen;
    unsigned int len;
    void *buf;
};

/*
 * The format of the addres and options arguments of the XTI t_bind call.
 * Only provided for compatibility, it should not be used.
 */

struct t_bind {
    struct netbuf   addr;
    unsigned int    qlen;
};

/*
 * Internal library and rpcbind use. This is not an exported interface, do
 * not use.
 */
struct __rpc_sockinfo {
    int si_af;
    int si_proto;
    int si_socktype;
    int si_alen;
};

#endif /* _TIRPC_TYPES_H */
